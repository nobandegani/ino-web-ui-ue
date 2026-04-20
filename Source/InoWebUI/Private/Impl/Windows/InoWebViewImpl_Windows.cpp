// Copyright Inoksan. All Rights Reserved.

#include "InoWebViewImpl_Windows.h"

#if PLATFORM_WINDOWS

#include "InoWebUILog.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// ── Windows + WebView2 headers ─────────────────────────────────────────────
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"

THIRD_PARTY_INCLUDES_START
#include <Windows.h>
#include <wrl.h>
#include <wrl/event.h>
#include <WebView2.h>
THIRD_PARTY_INCLUDES_END

#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// ─────────────────────────────────────────────────────────────────────────────
//  URI helpers (lockdown)
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    /** Extract the host component of a URI. Returns empty for non-URL inputs. */
    FString ExtractHost(const FString& URI)
    {
        // Find "://"
        int32 SchemeEnd;
        if (!URI.FindChar(TEXT(':'), SchemeEnd) || URI.Mid(SchemeEnd, 3) != TEXT("://"))
        {
            return FString();
        }
        FString AfterScheme = URI.Mid(SchemeEnd + 3);

        // Strip userinfo "user:pass@"
        int32 AtIdx;
        if (AfterScheme.FindChar(TEXT('@'), AtIdx))
        {
            AfterScheme = AfterScheme.Mid(AtIdx + 1);
        }

        // Find end of host (first of '/', '?', '#', ':')
        int32 HostEnd = AfterScheme.Len();
        for (TCHAR Ch : { TEXT('/'), TEXT('?'), TEXT('#'), TEXT(':') })
        {
            int32 Idx;
            if (AfterScheme.FindChar(Ch, Idx) && Idx < HostEnd)
            {
                HostEnd = Idx;
            }
        }
        return AfterScheme.Left(HostEnd);
    }

    /**
     * Decide whether a URI is allowed under the current lockdown config.
     * Returns true if navigation should proceed; false if it should be cancelled.
     */
    bool IsURIAllowed(const FString& URI, const FInoWebViewConfig& Config)
    {
        // Internal browser schemes always pass.
        if (URI.IsEmpty()
            || URI.StartsWith(TEXT("about:"))
            || URI.StartsWith(TEXT("data:"))
            || URI.StartsWith(TEXT("blob:")))
        {
            return true;
        }

        // Lockdown off → everything goes.
        if (!Config.bLockToVirtualHost)
        {
            return true;
        }

        // Virtual host whole-host match (case-insensitive).
        if (!Config.VirtualHostName.IsEmpty())
        {
            const FString Host = ExtractHost(URI);
            if (Host.Equals(Config.VirtualHostName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }

        // User wildcard allowlist. MatchesWildcard supports * and ?.
        for (const FString& Pattern : Config.AllowedURIPatterns)
        {
            if (URI.MatchesWildcard(Pattern))
            {
                return true;
            }
        }

        return false;
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  GInoWebUIBridgeScript
//
//  Injected into every page via ICoreWebView2::AddScriptToExecuteOnDocumentCreated,
//  which runs the script *before* any page script. This gives every page a
//  consistent `window.InoWebUI` API:
//
//    window.InoWebUI.send(channel, payload)   — push a message to UE
//    window.InoWebUI.on(channel, handler)     — subscribe to UE messages
//    window.InoWebUI.off(channel, handler)    — unsubscribe
//
//  Wire format is a fixed envelope { channel: string, payload: any }
//  serialized as JSON, carried by window.chrome.webview.postMessage /
//  window.chrome.webview onmessage. The UE side (UInoWebView) parses the
//  same envelope, so both directions stay symmetric.
//
//  Kept as a raw string literal — no build-time asset dependency, no file
//  I/O at runtime, script gets folded into the DLL.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
//  GInoWebUIDevToolsOverlayScript
//
//  Injected ONLY when FInoWebViewConfig::bEnableDevTools is true. Creates a
//  floating circular button in the bottom-right corner of the page; clicking
//  it expands seven action buttons on an arc. Each button fires a
//  "_devtools.<action>" channel through window.InoWebUI.send; UInoWebView
//  intercepts that prefix in DispatchIncomingEnvelope and handles it
//  internally (never forwarded to the user's OnMessageReceived).
//
//  If you modify this JS, also update the identical copy in the Android
//  helper's DEVTOOLS_OVERLAY_JS constant (InoWebViewAndroid.java).
// ─────────────────────────────────────────────────────────────────────────────
static const TCHAR* GInoWebUIDevToolsOverlayScript = TEXT(R"JS(
(function() {
  if (window.__inoDevOverlayLoaded) return;
  window.__inoDevOverlayLoaded = true;

  // "handler" on an action means JS-only — don't hop to UE. Info shows an
  // inline modal instead so the user gets immediate visual feedback.
  var ACTIONS = [
    { id: 'refresh',            icon: '\u21BB', title: 'Refresh' },
    { id: 'openDevTools',       icon: '\u2325', title: 'Open DevTools' },
    { id: 'clearData',          icon: '\u232B', title: 'Clear Data' },
    { id: 'info',               icon: '\u24D8', title: 'Info',
      handler: function() { showInfoModal(); } },
    { id: 'toggleTransparency', icon: '\u25C9', title: 'Toggle Transparency' },
    { id: 'hideWebUI',          icon: '\u2298', title: 'Hide WebUI' },
    { id: 'devCallback',        icon: '\u25C6', title: 'Dev Callback' }
  ];

  function detectPlatform() {
    if (window.chrome && window.chrome.webview) return 'Windows (WebView2)';
    if (window._InoWebUIHost)                   return 'Android (WebView)';
    return 'Browser (no bridge)';
  }

  function showInfoModal() {
    var existing = document.getElementById('__ino-dev-info-modal');
    if (existing) { existing.remove(); return; }

    var info = {
      'URL':                 location.href,
      'Title':               document.title || '(none)',
      'Platform':            detectPlatform(),
      'Viewport':            window.innerWidth + ' x ' + window.innerHeight,
      'Screen':              screen.width + ' x ' + screen.height,
      'Device Pixel Ratio':  window.devicePixelRatio,
      'Language':            navigator.language,
      'Online':              navigator.onLine ? 'yes' : 'no',
      'Touch':               ('ontouchstart' in window) ? 'yes' : 'no',
      'InoWebUI bridge':     window.InoWebUI ? ('loaded v' + window.InoWebUI.version) : 'NOT loaded',
      'User Agent':          navigator.userAgent
    };

    var modal = document.createElement('div');
    modal.id = '__ino-dev-info-modal';
    modal.style.cssText = 'position:fixed;inset:0;z-index:2147483646;'
      + 'background:rgba(0,0,0,0.62);backdrop-filter:blur(4px);'
      + '-webkit-backdrop-filter:blur(4px);'
      + 'display:flex;align-items:center;justify-content:center;'
      + 'pointer-events:auto;padding:24px;'
      + 'font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;';

    var card = document.createElement('div');
    card.style.cssText = S('background:rgba(18,18,26,0.96);'
      + 'border:1px solid rgba(255,255,255,0.12);border-radius:14px;'
      + 'padding:20px 24px;max-width:560px;width:100%;'
      + 'color:#e7ecf3;font-size:13px;'
      + 'box-shadow:0 20px 60px rgba(0,0,0,0.5);');

    var header = document.createElement('div');
    header.style.cssText = 'display:flex;align-items:center;justify-content:space-between;'
      + 'margin-bottom:14px;';
    var h = document.createElement('div');
    h.textContent = 'WebView Info';
    h.style.cssText = 'font-size:15px;font-weight:600;color:#fff;';
    var x = document.createElement('button');
    x.textContent = '\u00D7';
    x.style.cssText = S('background:transparent;border:0;color:rgba(255,255,255,0.6);'
      + 'font-size:22px;cursor:pointer;padding:0 4px;line-height:1;');
    header.appendChild(h); header.appendChild(x);
    card.appendChild(header);

    var table = document.createElement('div');
    table.style.cssText = 'display:grid;grid-template-columns:auto 1fr;gap:6px 14px;';
    Object.keys(info).forEach(function(k) {
      var kEl = document.createElement('div');
      kEl.textContent = k;
      kEl.style.cssText = 'color:rgba(255,255,255,0.55);font-size:12px;';
      var vEl = document.createElement('div');
      vEl.textContent = info[k];
      vEl.style.cssText = 'font-family:"SF Mono",Menlo,Consolas,monospace;'
        + 'font-size:12px;word-break:break-all;user-select:text;-webkit-user-select:text;';
      table.appendChild(kEl); table.appendChild(vEl);
    });
    card.appendChild(table);

    modal.appendChild(card);
    x.addEventListener('click', function() { modal.remove(); });
    modal.addEventListener('click', function(e) { if (e.target === modal) modal.remove(); });
    document.body.appendChild(modal);
  }
)JS") TEXT(R"JS(
  // MSVC's string-literal limit is 16380 chars per token, so the overlay
  // script is split into two adjacent raw literals that the preprocessor
  // concatenates. No runtime cost — this happens at compile time.

  // 90-degree arc from 0 (up) to 90 (left), 15-degree step, radius 140.
  var POSITIONS = [
    { tx: 0,    ty: -140 }, { tx: -36,  ty: -135 },
    { tx: -70,  ty: -121 }, { tx: -99,  ty: -99  },
    { tx: -121, ty: -70  }, { tx: -135, ty: -36  },
    { tx: -140, ty: 0    }
  ];

  function S(extras) {
    return 'all:initial;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",'
         + 'Roboto,sans-serif;line-height:1;color:#fff;' + extras;
  }

  function build() {
    var root = document.createElement('div');
    root.id = '__ino-dev-overlay';
    root.style.cssText = 'position:fixed;bottom:16px;right:16px;'
                      + 'width:180px;height:180px;pointer-events:none;'
                      + 'z-index:2147483647;';

    var main = document.createElement('button');
    main.textContent = '\u2699';
    main.style.cssText = S('position:absolute;bottom:0;right:0;'
      + 'width:52px;height:52px;border-radius:50%;'
      + 'background:rgba(20,20,28,0.82);'
      + 'border:1px solid rgba(255,255,255,0.18);'
      + 'box-shadow:0 6px 24px rgba(0,0,0,0.4);'
      + 'cursor:pointer;pointer-events:auto;'
      + 'display:flex;align-items:center;justify-content:center;font-size:24px;'
      + 'backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);'
      + 'transition:transform 0.2s,background 0.2s;');
    root.appendChild(main);

    var children = [];
    ACTIONS.forEach(function(a, i) {
      var pos = POSITIONS[i];
      var btn = document.createElement('button');
      btn.textContent = a.icon;
      btn.title = a.title;
      btn.style.cssText = S('position:absolute;bottom:6px;right:6px;'
        + 'width:40px;height:40px;border-radius:50%;'
        + 'background:rgba(20,20,28,0.9);'
        + 'border:1px solid rgba(255,255,255,0.12);'
        + 'box-shadow:0 4px 12px rgba(0,0,0,0.4);'
        + 'cursor:pointer;pointer-events:none;'
        + 'display:flex;align-items:center;justify-content:center;font-size:18px;'
        + 'opacity:0;transform:translate(0,0) scale(0.3);'
        + 'transition:transform 0.25s cubic-bezier(0.175,0.885,0.32,1.275),'
        + 'opacity 0.2s,background 0.15s;');
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        collapse();
        if (a.handler) { a.handler(); return; }
        if (window.InoWebUI && typeof window.InoWebUI.send === 'function') {
          try { window.InoWebUI.send('_devtools.' + a.id, {}); }
          catch (err) { console.error('InoDevOverlay:', err); }
        }
      });
      btn.addEventListener('mouseenter', function() {
        if (expanded) {
          btn.style.transform = 'translate(' + pos.tx + 'px,' + pos.ty + 'px) scale(1.12)';
          btn.style.background = 'rgba(60,60,80,0.95)';
        }
      });
      btn.addEventListener('mouseleave', function() {
        if (expanded) {
          btn.style.transform = 'translate(' + pos.tx + 'px,' + pos.ty + 'px) scale(1)';
          btn.style.background = 'rgba(20,20,28,0.9)';
        }
      });
      root.appendChild(btn);
      children.push({ btn: btn, pos: pos });
    });

    var expanded = false;
    function expand() {
      expanded = true;
      main.style.transform = 'rotate(45deg)';
      main.style.background = 'rgba(60,60,80,0.92)';
      children.forEach(function(c) {
        c.btn.style.opacity = '1';
        c.btn.style.pointerEvents = 'auto';
        c.btn.style.transform = 'translate(' + c.pos.tx + 'px,' + c.pos.ty + 'px) scale(1)';
      });
    }
    function collapse() {
      expanded = false;
      main.style.transform = 'rotate(0deg)';
      main.style.background = 'rgba(20,20,28,0.82)';
      children.forEach(function(c) {
        c.btn.style.opacity = '0';
        c.btn.style.pointerEvents = 'none';
        c.btn.style.transform = 'translate(0,0) scale(0.3)';
      });
    }

    main.addEventListener('click', function(e) {
      e.stopPropagation();
      if (expanded) collapse(); else expand();
    });
    main.addEventListener('mouseenter', function() {
      if (!expanded) main.style.transform = 'scale(1.08)';
    });
    main.addEventListener('mouseleave', function() {
      if (!expanded) main.style.transform = 'scale(1)';
    });
    document.addEventListener('click', function(e) {
      if (expanded && !root.contains(e.target)) collapse();
    });

    return root;
  }

  function mount() {
    if (document.body) document.body.appendChild(build());
    else document.addEventListener('DOMContentLoaded', mount);
  }
  mount();
})();
)JS");

static const TCHAR* GInoWebUIBridgeScript = TEXT(R"JS(
(function() {
  if (typeof window === 'undefined' || window.InoWebUI) return;
  if (!window.chrome || !window.chrome.webview) return;

  var listeners = {};

  window.InoWebUI = {
    version: '1.0',
    send: function(channel, payload) {
      try {
        var envelope = { channel: String(channel), payload: payload };
        window.chrome.webview.postMessage(JSON.stringify(envelope));
      } catch (e) { console.error('InoWebUI.send failed:', e); }
    },
    on: function(channel, handler) {
      if (typeof handler !== 'function') return;
      if (!listeners[channel]) listeners[channel] = [];
      listeners[channel].push(handler);
    },
    off: function(channel, handler) {
      var arr = listeners[channel];
      if (!arr) return;
      var i = arr.indexOf(handler);
      if (i >= 0) arr.splice(i, 1);
    }
  };

  window.chrome.webview.addEventListener('message', function(evt) {
    try {
      var data = evt.data;
      var envelope = (typeof data === 'string') ? JSON.parse(data) : data;
      if (!envelope || typeof envelope.channel !== 'string') return;
      var arr = listeners[envelope.channel];
      if (!arr) return;
      for (var i = 0; i < arr.length; i++) {
        try { arr[i](envelope.payload); }
        catch (e) { console.error('InoWebUI handler error:', e); }
      }
    } catch (e) { console.error('InoWebUI receive error:', e); }
  });
})();
)JS");

// ─────────────────────────────────────────────────────────────────────────────
//  FInternal — all WebView2 COM state lives here.
// ─────────────────────────────────────────────────────────────────────────────
struct FInoWebViewImpl_Windows::FInternal
{
    HWND              ParentHwnd = nullptr;
    FInoWebViewConfig Config;

    ComPtr<ICoreWebView2Environment> Environment;
    ComPtr<ICoreWebView2Controller>  Controller;
    ComPtr<ICoreWebView2>            WebView;

    // Operations queued before the controller was ready. Replayed once ready.
    TOptional<FString> PendingNavigate;
    TOptional<bool>    PendingVisible;

    struct FRect { int32 X = 0; int32 Y = 0; int32 W = 0; int32 H = 0; };
    TOptional<FRect>   PendingBounds;
    bool               bPendingReload = false;

    /** Messages queued before the WebView was ready; replayed on ready. */
    TArray<FString>    PendingOutboundMessages;

    /** JS scripts queued before the WebView was ready; replayed on ready. */
    TArray<FString>    PendingScripts;

    /** Last SetMuted() call before the WebView was ready; applied on ready. */
    TOptional<bool>    PendingMute;

    /** Tokens for event registrations. */
    EventRegistrationToken MessageReceivedToken{};
    EventRegistrationToken NavigationStartingToken{};
    EventRegistrationToken NavigationCompletedToken{};
    EventRegistrationToken DocumentTitleChangedToken{};
    EventRegistrationToken ScriptDialogOpeningToken{};
    EventRegistrationToken NewWindowRequestedToken{};
    EventRegistrationToken GotFocusToken{};
    EventRegistrationToken LostFocusToken{};
    EventRegistrationToken ProcessFailedToken{};

    /**
     * Lifetime token. Async callbacks capture a TWeakPtr to this; if the
     * impl is destroyed before the callback fires, the weak ptr is invalid
     * and the callback becomes a safe no-op.
     */
    TSharedPtr<int>    LifetimeToken = MakeShared<int>(0);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
FInoWebViewImpl_Windows::FInoWebViewImpl_Windows()
    : Internal(MakeUnique<FInternal>())
{
}

FInoWebViewImpl_Windows::~FInoWebViewImpl_Windows()
{
    Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initialize — kicks off the async WebView2 creation pipeline.
//
//     CreateCoreWebView2EnvironmentWithOptions   (async)
//              ↓
//     OnEnvironmentReady → Environment->CreateCoreWebView2Controller  (async)
//              ↓
//     OnControllerReady  → configure + flush pending ops
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Windows::Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    if (bInitStarted)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("FInoWebViewImpl_Windows::Initialize called more than once — ignoring."));
        return false;
    }
    if (!ParentNativeHandle)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("FInoWebViewImpl_Windows::Initialize: parent HWND is null."));
        return false;
    }

    Internal->ParentHwnd = static_cast<HWND>(ParentNativeHandle);
    Internal->Config     = Config;

    // Seed pending ops from config so they apply as soon as the WebView is ready.
    if (!Config.InitialURL.IsEmpty())
    {
        Internal->PendingNavigate = Config.InitialURL;
    }
    Internal->PendingVisible = Config.bVisibleOnCreate;

    // Verify the WebView2 Runtime is installed (Edge on Win10+ normally provides this).
    {
        LPWSTR VersionString = nullptr;
        const HRESULT HrVer = GetAvailableCoreWebView2BrowserVersionString(nullptr, &VersionString);
        if (FAILED(HrVer) || VersionString == nullptr)
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("WebView2 Runtime not found on this system. Install Microsoft Edge "
                     "WebView2 Runtime from https://developer.microsoft.com/microsoft-edge/webview2/"));
            return false;
        }
        UE_LOG(LogInoWebUI, Log, TEXT("WebView2 Runtime version: %s"), VersionString);
        CoTaskMemFree(VersionString);
    }

    // Per-WebView user data folder under the project's Saved dir.
    const FString UserDataPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / Config.UserDataSubfolder);
    IFileManager::Get().MakeDirectory(*UserDataPath, /*Tree=*/true);

    // ── Async step 1: create environment ────────────────────────────────────
    // Log BEFORE the call — WebView2 sometimes fires the completion callback
    // synchronously inside this call (when the environment is already warm),
    // so logging after would make the "ready" log appear before "started".
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 async initialization started."));

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;

    const HRESULT Hr = CreateCoreWebView2EnvironmentWithOptions(
        /*browserExecutableFolder=*/ nullptr,   // use installed Edge
        /*userDataFolder=*/          *UserDataPath,
        /*environmentOptions=*/      nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, WeakLifetime](HRESULT Result, ICoreWebView2Environment* Env) -> HRESULT
            {
                if (!WeakLifetime.IsValid())
                {
                    return S_OK; // impl destroyed — drop the callback silently
                }
                OnEnvironmentReady(static_cast<int32>(Result), Env);
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateCoreWebView2EnvironmentWithOptions failed synchronously: 0x%08X"),
            static_cast<uint32>(Hr));
        return false;
    }

    bInitStarted = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OnEnvironmentReady — environment is alive; create the controller.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::OnEnvironmentReady(int32 HResult, void* EnvironmentPtr)
{
    check(IsInGameThread());

    if (FAILED(HResult) || EnvironmentPtr == nullptr)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("WebView2 environment creation failed: 0x%08X"), static_cast<uint32>(HResult));
        return;
    }

    auto* Env = static_cast<ICoreWebView2Environment*>(EnvironmentPtr);
    Internal->Environment = Env;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 environment ready; creating controller..."));

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;

    const HRESULT Hr = Env->CreateCoreWebView2Controller(
        Internal->ParentHwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, WeakLifetime](HRESULT Result, ICoreWebView2Controller* Ctrl) -> HRESULT
            {
                if (!WeakLifetime.IsValid())
                {
                    return S_OK;
                }
                OnControllerReady(static_cast<int32>(Result), Ctrl);
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateCoreWebView2Controller call failed synchronously: 0x%08X"),
            static_cast<uint32>(Hr));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OnControllerReady — final step; configure & flush the queue.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::OnControllerReady(int32 HResult, void* ControllerPtr)
{
    check(IsInGameThread());

    if (FAILED(HResult) || ControllerPtr == nullptr)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("WebView2 controller creation failed: 0x%08X"), static_cast<uint32>(HResult));
        return;
    }

    auto* Ctrl = static_cast<ICoreWebView2Controller*>(ControllerPtr);
    Internal->Controller = Ctrl;

    // Fetch the WebView interface.
    if (FAILED(Ctrl->get_CoreWebView2(&Internal->WebView)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("get_CoreWebView2 failed."));
        return;
    }

    // ── Inject the JS-side bridge (window.InoWebUI) on every page load ──────
    // AddScriptToExecuteOnDocumentCreated runs BEFORE any page script, so
    // by the time the page's own JS runs, window.InoWebUI is already available.
    {
        const HRESULT HrScript = Internal->WebView->AddScriptToExecuteOnDocumentCreated(
            GInoWebUIBridgeScript,
            /*completed handler=*/ nullptr);
        if (FAILED(HrScript))
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("AddScriptToExecuteOnDocumentCreated failed: 0x%08X; "
                     "window.InoWebUI bridge will not be available."),
                static_cast<uint32>(HrScript));
        }
    }

    // ── Dev overlay — only when the user opted in via config ────────────────
    if (Internal->Config.bEnableDevTools)
    {
        Internal->WebView->AddScriptToExecuteOnDocumentCreated(
            GInoWebUIDevToolsOverlayScript,
            /*completed handler=*/ nullptr);
    }

    // ── Hook navigation events (Phase 5) ────────────────────────────────────
    // NavigationStarting is where lockdown lives: we cancel if the target
    // URI isn't whitelisted. The BP-visible delegate always fires for
    // observation, regardless of whether we cancel.
    {
        TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;

        Internal->WebView->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    LPWSTR UriRaw = nullptr;
                    if (FAILED(Args->get_Uri(&UriRaw)) || !UriRaw) return S_OK;
                    const FString URI(UriRaw);
                    CoTaskMemFree(UriRaw);

                    // Lockdown check.
                    if (!IsURIAllowed(URI, Internal->Config))
                    {
                        Args->put_Cancel(1);
                        UE_LOG(LogInoWebUI, Warning,
                            TEXT("Navigation blocked by lockdown: %s"), *URI);
                    }

                    // Always fire the BP delegate for observability.
                    if (OnNavigationStartingCallback)
                    {
                        OnNavigationStartingCallback(URI);
                    }
                    return S_OK;
                }).Get(),
            &Internal->NavigationStartingToken);

        Internal->WebView->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    BOOL bSuccess = 0;
                    Args->get_IsSuccess(&bSuccess);

                    // Pull the current Source as the "landed" URI — the args
                    // don't carry it, and we want something meaningful for BP.
                    FString URI;
                    LPWSTR SourceRaw = nullptr;
                    if (SUCCEEDED(Internal->WebView->get_Source(&SourceRaw)) && SourceRaw)
                    {
                        URI = SourceRaw;
                        CoTaskMemFree(SourceRaw);
                    }

                    if (OnNavigationCompletedCallback)
                    {
                        OnNavigationCompletedCallback(bSuccess != 0, URI);
                    }
                    return S_OK;
                }).Get(),
            &Internal->NavigationCompletedToken);

        Internal->WebView->add_DocumentTitleChanged(
            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, IUnknown*) -> HRESULT
                {
                    if (!WeakLifetime.IsValid()) return S_OK;

                    LPWSTR TitleRaw = nullptr;
                    if (FAILED(Internal->WebView->get_DocumentTitle(&TitleRaw)) || !TitleRaw)
                    {
                        return S_OK;
                    }
                    const FString Title(TitleRaw);
                    CoTaskMemFree(TitleRaw);

                    if (OnDocumentTitleChangedCallback)
                    {
                        OnDocumentTitleChangedCallback(Title);
                    }
                    return S_OK;
                }).Get(),
            &Internal->DocumentTitleChangedToken);

        // ── JS dialog suppression ───────────────────────────────────────────
        // Default: silently suppress all alert/confirm/prompt/beforeunload.
        // Subscribing but NOT calling Accept() on the args means the dialog
        // is cancelled — JS sees false for confirm, null for prompt, etc.
        Internal->WebView->add_ScriptDialogOpening(
            Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    COREWEBVIEW2_SCRIPT_DIALOG_KIND KindRaw = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
                    Args->get_Kind(&KindRaw);

                    LPWSTR MsgRaw = nullptr;
                    FString Message;
                    if (SUCCEEDED(Args->get_Message(&MsgRaw)) && MsgRaw)
                    {
                        Message = MsgRaw;
                        CoTaskMemFree(MsgRaw);
                    }

                    const EInoScriptDialogKind Kind = [&]
                    {
                        switch (KindRaw)
                        {
                            case COREWEBVIEW2_SCRIPT_DIALOG_KIND_CONFIRM:      return EInoScriptDialogKind::Confirm;
                            case COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT:       return EInoScriptDialogKind::Prompt;
                            case COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD: return EInoScriptDialogKind::BeforeUnload;
                            default:                                          return EInoScriptDialogKind::Alert;
                        }
                    }();

                    // Config-gated behavior: if the user explicitly allowed
                    // dialogs, Accept() lets the native dialog proceed. Default
                    // is to suppress (never call Accept), which cancels it.
                    if (Internal->Config.bAllowScriptDialogs)
                    {
                        Args->Accept();
                    }
                    else
                    {
                        UE_LOG(LogInoWebUI, Verbose,
                            TEXT("Suppressed JS dialog (%d): %s"),
                            static_cast<int32>(Kind), *Message);
                    }

                    if (OnScriptDialogCallback)
                    {
                        OnScriptDialogCallback(Kind, Message);
                    }
                    return S_OK;
                }).Get(),
            &Internal->ScriptDialogOpeningToken);

        // ── Focus events (Controller, not WebView) ──────────────────────────
        Internal->Controller->add_GotFocus(
            Callback<ICoreWebView2FocusChangedEventHandler>(
                [this, WeakLifetime](ICoreWebView2Controller*, IUnknown*) -> HRESULT
                {
                    if (WeakLifetime.IsValid() && OnGotFocusCallback) OnGotFocusCallback();
                    return S_OK;
                }).Get(),
            &Internal->GotFocusToken);

        Internal->Controller->add_LostFocus(
            Callback<ICoreWebView2FocusChangedEventHandler>(
                [this, WeakLifetime](ICoreWebView2Controller*, IUnknown*) -> HRESULT
                {
                    if (WeakLifetime.IsValid() && OnLostFocusCallback) OnLostFocusCallback();
                    return S_OK;
                }).Get(),
            &Internal->LostFocusToken);

        // ── Chromium subprocess failures ────────────────────────────────────
        Internal->WebView->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    COREWEBVIEW2_PROCESS_FAILED_KIND KindRaw =
                        COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                    Args->get_ProcessFailedKind(&KindRaw);

                    // Build a readable description. ICoreWebView2ProcessFailedEventArgs2
                    // (newer) has ProcessDescription; fall back to the kind name otherwise.
                    FString Description = FString::Printf(TEXT("Kind=%d"), static_cast<int32>(KindRaw));

                    UE_LOG(LogInoWebUI, Error,
                        TEXT("WebView2 subprocess failed: %s"), *Description);

                    if (OnProcessFailedCallback) OnProcessFailedCallback(Description);
                    return S_OK;
                }).Get(),
            &Internal->ProcessFailedToken);

        // ── window.open / target="_blank" ───────────────────────────────────
        // Default: block popups. put_Handled(TRUE) tells WebView2 we've taken
        // responsibility for the request; not setting a NewWindow means JS
        // window.open() returns null.
        Internal->WebView->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    FString URI;
                    LPWSTR UriRaw = nullptr;
                    if (SUCCEEDED(Args->get_Uri(&UriRaw)) && UriRaw)
                    {
                        URI = UriRaw;
                        CoTaskMemFree(UriRaw);
                    }

                    if (!Internal->Config.bAllowNewWindows)
                    {
                        Args->put_Handled(1);
                        UE_LOG(LogInoWebUI, Verbose,
                            TEXT("Blocked window.open / new window request: %s"), *URI);
                    }

                    if (OnNewWindowRequestedCallback)
                    {
                        OnNewWindowRequestedCallback(URI);
                    }
                    return S_OK;
                }).Get(),
            &Internal->NewWindowRequestedToken);
    }

    // ── Hook JS -> UE messaging ─────────────────────────────────────────────
    {
        TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
        const HRESULT HrMsg = Internal->WebView->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    // Prefer string mode (our bridge uses postMessage with a JSON string),
                    // fall back to JSON mode if the page sent a raw object.
                    LPWSTR Raw = nullptr;
                    HRESULT Hr = Args->TryGetWebMessageAsString(&Raw);
                    if (FAILED(Hr) || !Raw)
                    {
                        if (Raw) { CoTaskMemFree(Raw); Raw = nullptr; }
                        Hr = Args->get_WebMessageAsJson(&Raw);
                    }
                    if (SUCCEEDED(Hr) && Raw)
                    {
                        const FString Msg(Raw);
                        CoTaskMemFree(Raw);
                        if (OnMessageReceivedJson)
                        {
                            OnMessageReceivedJson(Msg);
                        }
                    }
                    return S_OK;
                }).Get(),
            &Internal->MessageReceivedToken);

        if (FAILED(HrMsg))
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("add_WebMessageReceived failed: 0x%08X"), static_cast<uint32>(HrMsg));
        }
    }

    // Transparent background (ICoreWebView2Controller2 was added in Runtime 90+).
    if (Internal->Config.bTransparentBackground)
    {
        ComPtr<ICoreWebView2Controller2> Ctrl2;
        if (SUCCEEDED(Internal->Controller.As(&Ctrl2)))
        {
            const COREWEBVIEW2_COLOR Transparent = { 0, 0, 0, 0 };
            Ctrl2->put_DefaultBackgroundColor(Transparent);
            UE_LOG(LogInoWebUI, Verbose, TEXT("WebView2 default background set to transparent."));
        }
        else
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("ICoreWebView2Controller2 unavailable; background will be opaque. "
                     "Upgrade the WebView2 Runtime to fix."));
        }
    }

    // ── Phase 3: apply runtime settings from the config ─────────────────────
    {
        ComPtr<ICoreWebView2Settings> Settings;
        if (SUCCEEDED(Internal->WebView->get_Settings(&Settings)))
        {
            Settings->put_AreDefaultContextMenusEnabled(Internal->Config.bEnableContextMenus   ? 1 : 0);
            Settings->put_AreDevToolsEnabled          (Internal->Config.bEnableDevTools         ? 1 : 0);

            // Settings3 — browser accelerator keys (F5/F12/Ctrl+F/…). Runtime 89+.
            ComPtr<ICoreWebView2Settings3> Settings3;
            if (SUCCEEDED(Settings.As(&Settings3)))
            {
                Settings3->put_AreBrowserAcceleratorKeysEnabled(
                    Internal->Config.bEnableAcceleratorKeys ? 1 : 0);
            }

            // Settings2 — UserAgent override. Runtime 86+.
            if (!Internal->Config.UserAgentOverride.IsEmpty())
            {
                ComPtr<ICoreWebView2Settings2> Settings2;
                if (SUCCEEDED(Settings.As(&Settings2)))
                {
                    Settings2->put_UserAgent(*Internal->Config.UserAgentOverride);
                    UE_LOG(LogInoWebUI, Verbose, TEXT("UserAgent override applied: %s"),
                        *Internal->Config.UserAgentOverride);
                }
            }
        }

        // Mute at startup if requested. ICoreWebView2_8 — Runtime 88+.
        if (Internal->Config.bStartMuted)
        {
            ComPtr<ICoreWebView2_8> WebView8;
            if (SUCCEEDED(Internal->WebView.As(&WebView8)))
            {
                WebView8->put_IsMuted(1);
            }
        }
    }

    // ── Phase 4: virtual-host → folder mapping ──────────────────────────────
    if (!Internal->Config.VirtualHostName.IsEmpty() &&
        !Internal->Config.VirtualHostFolder.IsEmpty())
    {
        ComPtr<ICoreWebView2_3> WebView3;
        if (FAILED(Internal->WebView.As(&WebView3)))
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("ICoreWebView2_3 unavailable (Runtime < 101); "
                     "virtual-host mapping skipped — fall back to file:// URIs."));
        }
        else
        {
            // Resolve folder: absolute stays absolute; relative is anchored at
            // the project's Content/ directory (matches LoadLocalFile's rule).
            const FString& FolderIn = Internal->Config.VirtualHostFolder;
            const FString AbsoluteFolder = FPaths::IsRelative(FolderIn)
                ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / FolderIn)
                : FPaths::ConvertRelativePathToFull(FolderIn);

            // Warn early if the folder doesn't exist. Mapping still succeeds
            // but every request will 404, which is confusing to debug.
            if (!IFileManager::Get().DirectoryExists(*AbsoluteFolder))
            {
                UE_LOG(LogInoWebUI, Warning,
                    TEXT("VirtualHostFolder does not exist: %s — requests to "
                         "https://%s/ will all return 404."),
                    *AbsoluteFolder, *Internal->Config.VirtualHostName);
            }

            const HRESULT Hr = WebView3->SetVirtualHostNameToFolderMapping(
                *Internal->Config.VirtualHostName,
                *AbsoluteFolder,
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

            if (FAILED(Hr))
            {
                UE_LOG(LogInoWebUI, Warning,
                    TEXT("SetVirtualHostNameToFolderMapping failed: 0x%08X  (%s -> %s)"),
                    static_cast<uint32>(Hr),
                    *Internal->Config.VirtualHostName, *AbsoluteFolder);
            }
            else
            {
                UE_LOG(LogInoWebUI, Log,
                    TEXT("Virtual host mapped:  https://%s/  ->  %s"),
                    *Internal->Config.VirtualHostName, *AbsoluteFolder);
            }
        }
    }

    // Initial bounds: the subsystem's BroadcastClientRectToAll runs right
    // after CreateWebView, so pending bounds are almost always queued by the
    // time we get here. If nothing was queued (edge case, e.g., viewport not
    // yet in a world), fall back to the raw HWND client rect — correct for
    // OS-chromed standalone windows, approximate for Slate-chromed PIE
    // windows until the next ViewportResizedEvent refines it.
    if (!Internal->PendingBounds.IsSet())
    {
        RECT ClientRect;
        ::GetClientRect(Internal->ParentHwnd, &ClientRect);
        Internal->Controller->put_Bounds(ClientRect);
    }

    bReady = true;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 controller ready."));

    ApplyPendingOperations();

    // One-shot "native WebView is live" signal. Fires AFTER the pending
    // queue replays so the initial Navigate / bounds / visibility are
    // already applied by the time the BP OnReady handler runs.
    if (OnReadyCallback)
    {
        OnReadyCallback();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Flush queued pre-ready calls.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::ApplyPendingOperations()
{
    check(IsInGameThread());
    if (!bReady) return;

    if (Internal->PendingNavigate.IsSet())
    {
        Navigate(Internal->PendingNavigate.GetValue());
        Internal->PendingNavigate.Reset();
    }
    if (Internal->PendingVisible.IsSet())
    {
        SetVisible(Internal->PendingVisible.GetValue());
        Internal->PendingVisible.Reset();
    }
    if (Internal->PendingBounds.IsSet())
    {
        const auto& R = Internal->PendingBounds.GetValue();
        SyncBounds(R.X, R.Y, R.W, R.H);
        Internal->PendingBounds.Reset();
    }
    if (Internal->bPendingReload)
    {
        Reload();
        Internal->bPendingReload = false;
    }

    // Replay outbound messages queued before the WebView was ready. Iterate
    // locally + empty first so that any side-effects that post back during
    // replay don't re-enter the array (safety, rare but free).
    if (Internal->PendingOutboundMessages.Num() > 0)
    {
        TArray<FString> Replay = MoveTemp(Internal->PendingOutboundMessages);
        for (const FString& Msg : Replay)
        {
            const HRESULT Hr = Internal->WebView->PostWebMessageAsString(*Msg);
            if (FAILED(Hr))
            {
                UE_LOG(LogInoWebUI, Warning,
                    TEXT("Queued PostWebMessageAsString failed: 0x%08X"),
                    static_cast<uint32>(Hr));
            }
        }
    }

    // Replay JS snippets queued before ready.
    if (Internal->PendingScripts.Num() > 0)
    {
        TArray<FString> Replay = MoveTemp(Internal->PendingScripts);
        for (const FString& Code : Replay)
        {
            ExecuteJavaScript(Code);
        }
    }

    // Apply a queued mute request.
    if (Internal->PendingMute.IsSet())
    {
        SetMuted(Internal->PendingMute.GetValue());
        Internal->PendingMute.Reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public operations
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::Navigate(const FString& URL)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingNavigate = URL;
        return;
    }

    const HRESULT Hr = Internal->WebView->Navigate(*URL);
    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("Navigate('%s') failed: 0x%08X"), *URL, static_cast<uint32>(Hr));
    }
}

void FInoWebViewImpl_Windows::Reload()
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->bPendingReload = true;
        return;
    }

    Internal->WebView->Reload();
}

void FInoWebViewImpl_Windows::SetVisible(bool bVisible)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingVisible = bVisible;
        return;
    }

    // Use 1/0 rather than TRUE/FALSE — those macros were un-#defined by
    // HideWindowsPlatformTypes.h above. put_IsVisible takes BOOL (typedef int).
    Internal->Controller->put_IsVisible(bVisible ? 1 : 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 3 — runtime polish
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::OpenDevTools()
{
    check(IsInGameThread());

    if (!bReady)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("OpenDevTools called before WebView is ready; ignored."));
        return;
    }
    if (!Internal->Config.bEnableDevTools)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("OpenDevTools called but bEnableDevTools was false at construction. "
                 "Set FInoWebViewConfig::bEnableDevTools = true to allow this."));
        return;
    }

    const HRESULT Hr = Internal->WebView->OpenDevToolsWindow();
    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("OpenDevToolsWindow failed: 0x%08X"), static_cast<uint32>(Hr));
    }
}

void FInoWebViewImpl_Windows::ExecuteJavaScript(const FString& Code)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingScripts.Add(Code);
        return;
    }

    // Fire-and-forget — pass a no-op handler rather than nullptr, since
    // some WebView2 runtime versions don't tolerate a null handler.
    const HRESULT Hr = Internal->WebView->ExecuteScript(
        *Code,
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("ExecuteScript failed: 0x%08X"), static_cast<uint32>(Hr));
    }
}

void FInoWebViewImpl_Windows::SetMuted(bool bMuted)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingMute = bMuted;
        return;
    }

    ComPtr<ICoreWebView2_8> WebView8;
    if (FAILED(Internal->WebView.As(&WebView8)))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetMuted: ICoreWebView2_8 unavailable (Runtime < 88)."));
        return;
    }
    WebView8->put_IsMuted(bMuted ? 1 : 0);
}

void FInoWebViewImpl_Windows::FocusWebView()
{
    check(IsInGameThread());
    if (!bReady || !Internal->Controller) return;

    Internal->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void FInoWebViewImpl_Windows::SetZoomFactor(float Factor)
{
    check(IsInGameThread());
    if (!bReady || !Internal->Controller) return;

    Internal->Controller->put_ZoomFactor(static_cast<double>(Factor));
}

float FInoWebViewImpl_Windows::GetZoomFactor() const
{
    if (!bReady || !Internal->Controller) return 1.0f;

    double Current = 1.0;
    if (FAILED(Internal->Controller->get_ZoomFactor(&Current))) return 1.0f;
    return static_cast<float>(Current);
}

void FInoWebViewImpl_Windows::ClearAllCookies()
{
    check(IsInGameThread());
    if (!bReady) return;

    // CookieManager lives on ICoreWebView2_2 (Runtime 85+). Fall through gracefully.
    ComPtr<ICoreWebView2_2> WebView2;
    if (FAILED(Internal->WebView.As(&WebView2)))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("ClearAllCookies: ICoreWebView2_2 unavailable."));
        return;
    }
    ComPtr<ICoreWebView2CookieManager> CookieMgr;
    if (FAILED(WebView2->get_CookieManager(&CookieMgr)) || !CookieMgr)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("ClearAllCookies: could not obtain CookieManager."));
        return;
    }
    CookieMgr->DeleteAllCookies();
    UE_LOG(LogInoWebUI, Log, TEXT("All cookies cleared for this WebView's profile."));
}

void FInoWebViewImpl_Windows::SetBackgroundOpaque(bool bOpaque)
{
    check(IsInGameThread());
    if (!bReady) return;

    ComPtr<ICoreWebView2Controller2> Ctrl2;
    if (FAILED(Internal->Controller.As(&Ctrl2))) return;

    // Opaque: full-alpha white; Transparent: zero alpha, any RGB.
    COREWEBVIEW2_COLOR Color;
    if (bOpaque) { Color = { 255, 255, 255, 255 }; }
    else         { Color = { 0, 0, 0, 0 }; }
    Ctrl2->put_DefaultBackgroundColor(Color);
}

void FInoWebViewImpl_Windows::PostMessageJson(const FString& Json)
{
    check(IsInGameThread());

    if (!bReady)
    {
        // Queue for replay from ApplyPendingOperations once the WebView finishes
        // async construction. Consistent with Navigate/SetVisible/SyncBounds.
        Internal->PendingOutboundMessages.Add(Json);
        return;
    }

    const HRESULT Hr = Internal->WebView->PostWebMessageAsString(*Json);
    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("PostWebMessageAsString failed: 0x%08X"), static_cast<uint32>(Hr));
    }
}

void FInoWebViewImpl_Windows::SyncBounds(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingBounds = FInternal::FRect{ ScreenX, ScreenY, Width, Height };
        return;
    }

    // Convert from screen coords to the parent HWND's client-area coords.
    // This is what makes PIE work correctly: for Slate-chromed windows the
    // HWND's "client area" includes Slate's drawn title bar, so plain (0,0)
    // would cover the title. Going through screen-space avoids that — the
    // subsystem tells us the real content rect in screen coords, and the
    // ScreenToClient call lands us correctly inside the HWND.
    POINT TopLeft = { ScreenX, ScreenY };
    ::ScreenToClient(Internal->ParentHwnd, &TopLeft);

    RECT Bounds;
    Bounds.left   = TopLeft.x;
    Bounds.top    = TopLeft.y;
    Bounds.right  = TopLeft.x + Width;
    Bounds.bottom = TopLeft.y + Height;
    Internal->Controller->put_Bounds(Bounds);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown — MUST be called before the parent HWND dies.
//
//  Idempotent: safe to call multiple times. Typically called twice in normal
//  teardown (once from UInoWebView::ShutdownImpl, once from the impl's own
//  destructor as a safety net). Only the first call does work or logs.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::Shutdown()
{
    check(IsInGameThread());
    if (!Internal) return;

    // Use LifetimeToken as our "already shut down" sentinel — it's set by
    // FInternal's ctor and we reset it here. Second call sees invalid token
    // and silently returns.
    if (!Internal->LifetimeToken.IsValid())
    {
        return;
    }

    // Invalidate the lifetime token first — any stray async callback fires
    // into a dead TWeakPtr and safely returns without touching our state.
    Internal->LifetimeToken.Reset();

    // Close the controller. This is the one required WebView2 teardown call:
    // it stops the Chromium processes and detaches the child HWND. Must happen
    // while the parent HWND is still alive.
    if (Internal->Controller)
    {
        Internal->Controller->Close();
    }

    Internal->WebView.Reset();
    Internal->Controller.Reset();
    Internal->Environment.Reset();

    bReady = false;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 shutdown complete."));
}

#endif // PLATFORM_WINDOWS
