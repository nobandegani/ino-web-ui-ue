// Copyright Inoland. All Rights Reserved.

#include "InoWebViewImpl_Windows_Composition.h"

#if PLATFORM_WINDOWS

#include "InoWebUILog.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// ── Windows + WebView2 + DComp headers ────────────────────────────────────
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"

THIRD_PARTY_INCLUDES_START
#include <Windows.h>
#include <windowsx.h>           // GET_X_LPARAM / GET_Y_LPARAM / GET_KEYSTATE_WPARAM
#include <wrl.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <dcomp.h>              // IDCompositionDevice / Target / Visual
THIRD_PARTY_INCLUDES_END

#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// ─────────────────────────────────────────────────────────────────────────────
//  URI helpers (lockdown) — duplicated from InoWebViewImpl_Windows.cpp so the
//  two files stay independent. If the logic changes, update both.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    FString ExtractHost(const FString& URI)
    {
        int32 SchemeEnd;
        if (!URI.FindChar(TEXT(':'), SchemeEnd) || URI.Mid(SchemeEnd, 3) != TEXT("://"))
        {
            return FString();
        }
        FString AfterScheme = URI.Mid(SchemeEnd + 3);

        int32 AtIdx;
        if (AfterScheme.FindChar(TEXT('@'), AtIdx))
        {
            AfterScheme = AfterScheme.Mid(AtIdx + 1);
        }

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

    bool IsURIAllowed(const FString& URI, const FInoWebViewConfig& Config)
    {
        if (URI.IsEmpty()
            || URI.StartsWith(TEXT("about:"))
            || URI.StartsWith(TEXT("data:"))
            || URI.StartsWith(TEXT("blob:")))
        {
            return true;
        }
        if (!Config.bLockToVirtualHost)
        {
            return true;
        }
        if (!Config.VirtualHostName.IsEmpty())
        {
            const FString Host = ExtractHost(URI);
            if (Host.Equals(Config.VirtualHostName, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
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
//  Bridge + dev-overlay JS
//
//  These are identical to the copies in InoWebViewImpl_Windows.cpp. Kept
//  separately so the two Windows impls have zero compile-time coupling and
//  either can evolve without affecting the other. If you modify either
//  script, also update:
//    • Source/InoWebUI/Private/Impl/Windows/InoWebViewImpl_Windows.cpp
//    • Source/InoWebUI/Java/src/net/inoland/webui/InoWebViewAndroid.java
//      (the Android copy — Java syntax, same semantics)
//
//  Dev overlay is split into two adjacent TEXT(R"JS(...)JS") chunks because
//  MSVC has a 16380-character limit on a single string literal. The
//  preprocessor concatenates them at compile time.
// ─────────────────────────────────────────────────────────────────────────────
static const TCHAR* GInoWebUIDevToolsOverlayScript_Composition = TEXT(R"JS(
(function() {
  if (window.__inoDevOverlayLoaded) return;
  window.__inoDevOverlayLoaded = true;

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
    card.style.cssText = 'background:rgba(18,18,26,0.96);'
      + 'border:1px solid rgba(255,255,255,0.12);border-radius:14px;'
      + 'padding:20px 24px;max-width:560px;width:100%;'
      + 'color:#e7ecf3;font-size:13px;'
      + 'box-shadow:0 20px 60px rgba(0,0,0,0.5);';

    var header = document.createElement('div');
    header.style.cssText = 'display:flex;align-items:center;justify-content:space-between;'
      + 'margin-bottom:14px;';
    var h = document.createElement('div');
    h.textContent = 'WebView Info';
    h.style.cssText = 'font-size:15px;font-weight:600;color:#fff;';
    var x = document.createElement('button');
    x.textContent = '\u00D7';
    x.style.cssText = 'background:transparent;border:0;color:rgba(255,255,255,0.6);'
      + 'font-size:22px;cursor:pointer;padding:0 4px;line-height:1;';
    header.appendChild(h); header.appendChild(x); card.appendChild(header);

    var table = document.createElement('div');
    table.style.cssText = 'display:grid;grid-template-columns:auto 1fr;gap:6px 14px;';
    Object.keys(info).forEach(function(k) {
      var kEl = document.createElement('div'); kEl.textContent = k;
      kEl.style.cssText = 'color:rgba(255,255,255,0.55);font-size:12px;';
      var vEl = document.createElement('div'); vEl.textContent = info[k];
      vEl.style.cssText = 'font-family:monospace;font-size:12px;word-break:break-all;'
        + 'user-select:text;-webkit-user-select:text;';
      table.appendChild(kEl); table.appendChild(vEl);
    });
    card.appendChild(table);
    modal.appendChild(card);
    x.addEventListener('click', function() { modal.remove(); });
    modal.addEventListener('click', function(e) { if (e.target === modal) modal.remove(); });
    document.body.appendChild(modal);
  }

  function mount() {
    if (document.getElementById('__ino-dev-overlay-root')) return;

    var root = document.createElement('div');
    root.id = '__ino-dev-overlay-root';
    root.style.cssText = 'position:fixed;right:18px;bottom:18px;'
      + 'z-index:2147483647;pointer-events:none;'
      + 'font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;';

    var gear = document.createElement('button');
    gear.textContent = '\u2699';
    gear.title = 'Dev tools';
    gear.style.cssText = 'position:absolute;right:0;bottom:0;width:44px;height:44px;'
      + 'border-radius:50%;border:0;font-size:22px;cursor:pointer;pointer-events:auto;'
      + 'background:rgba(18,18,26,0.85);color:#fff;'
      + 'box-shadow:0 4px 12px rgba(0,0,0,0.4);'
      + 'transition:transform 0.2s,background 0.2s;';
    gear.onmouseenter = function() { gear.style.background = 'rgba(28,28,40,0.95)'; };
    gear.onmouseleave = function() { gear.style.background = 'rgba(18,18,26,0.85)'; };

    var open = false;
    var actionButtons = [];

    ACTIONS.forEach(function(action, i) {
      var btn = document.createElement('button');
      btn.textContent = action.icon;
      btn.title = action.title;
      btn.dataset.action = action.id;
      btn.style.cssText = 'position:absolute;right:0;bottom:0;width:36px;height:36px;'
        + 'border-radius:50%;border:0;font-size:16px;cursor:pointer;pointer-events:auto;'
        + 'background:rgba(40,40,55,0.9);color:#fff;'
        + 'box-shadow:0 2px 8px rgba(0,0,0,0.4);'
        + 'transform:translate(0,0);opacity:0;'
        + 'transition:transform 0.3s cubic-bezier(0.25,0.8,0.25,1),opacity 0.2s;';
      btn.onmouseenter = function() { btn.style.background = 'rgba(55,55,75,0.95)'; };
      btn.onmouseleave = function() { btn.style.background = 'rgba(40,40,55,0.9)'; };
      btn.onclick = function() {
        if (action.handler) { action.handler(); return; }
        try {
          if (window.InoWebUI && window.InoWebUI.send) {
            window.InoWebUI.send('_devtools.' + action.id, {});
          }
        } catch (e) { console.error('dev-overlay send failed', e); }
      };
      root.appendChild(btn);
      actionButtons.push(btn);
    });

    function layout() {
      var angleStep = (Math.PI * 0.5) / (ACTIONS.length - 1);
      var radius = 72;
      actionButtons.forEach(function(btn, i) {
        if (open) {
          var angle = -Math.PI + angleStep * i;
          var x = Math.cos(angle) * radius;
          var y = Math.sin(angle) * radius;
          btn.style.transform = 'translate(' + x + 'px,' + y + 'px)';
          btn.style.opacity = '1';
        } else {
          btn.style.transform = 'translate(0,0)';
          btn.style.opacity = '0';
        }
      });
    }

    gear.onclick = function() { open = !open; layout(); };
    root.appendChild(gear);

    if (document.body) document.body.appendChild(root);
    else document.addEventListener('DOMContentLoaded', mount);
  }
  mount();
})();
)JS");

static const TCHAR* GInoWebUIBridgeScript_Composition = TEXT(R"JS(
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
//  Overlay window class — registered lazily, once per process.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    const TCHAR* kOverlayClassName = TEXT("InoWebUIComposition_Overlay");
    bool         bOverlayClassRegistered = false;

    bool EnsureOverlayClassRegistered()
    {
        if (bOverlayClassRegistered) return true;

        WNDCLASSEX Wc = {};
        Wc.cbSize        = sizeof(WNDCLASSEX);
        Wc.style         = CS_HREDRAW | CS_VREDRAW;
        Wc.lpfnWndProc   = FInoWebViewImpl_Windows_Composition::OverlayWndProcStatic;
        Wc.hInstance     = GetModuleHandle(nullptr);
        Wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        Wc.hbrBackground = nullptr;          // DComp paints; no GDI fill
        Wc.lpszClassName = kOverlayClassName;

        if (!RegisterClassEx(&Wc))
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("RegisterClassEx for overlay failed: 0x%08X"),
                static_cast<uint32>(GetLastError()));
            return false;
        }
        bOverlayClassRegistered = true;
        return true;
    }

    /** Poll timer id; identifies our overlay HWND's tracking timer. */
    constexpr UINT_PTR kOverlayTrackTimerId = 0x494E4F01; // "INO\x01"
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  FInternal — WebView2 + DComp + overlay-window state.
// ─────────────────────────────────────────────────────────────────────────────
struct FInoWebViewImpl_Windows_Composition::FInternal
{
    //
    // UE's game window — we only READ its geometry / visibility. Our overlay
    // is top-level, not a child of it.
    //
    HWND ParentHwnd = nullptr;

    //
    // Our own top-level overlay HWND. Hosts the DComp visual tree. Positioned
    // and sized to match the WebView's intended screen rect on every
    // SyncBounds. Owner = ParentHwnd (for z-order / auto-destroy).
    //
    HWND OverlayHwnd = nullptr;

    //
    // Last observed geometry of the intended WebView rect (screen coords).
    // Used by the tracking timer to detect UE window moves.
    //
    int32 DesiredX = 0;
    int32 DesiredY = 0;
    int32 DesiredW = 0;
    int32 DesiredH = 0;

    //
    // Last observed parent-window RECT (screen coords). The tracking timer
    // compares against this to detect moves (UE doesn't fire SyncBounds on
    // a pure window move — only resize).
    //
    RECT LastParentRect {0, 0, 0, 0};

    //
    // Whether the user has asked for the WebView to be visible. Separate
    // from the OS window's WS_VISIBLE flag because we also hide the overlay
    // when the parent minimises.
    //
    bool bUserVisible = true;

    FInoWebViewConfig Config;

    //
    // COM objects — kept minimal. The composition controller is the
    // composition-specific interface; the Controller pointer is a QI'd
    // ICoreWebView2Controller for the shared methods (put_Bounds, MoveFocus,
    // put_IsVisible, etc.).
    //
    ComPtr<ICoreWebView2Environment>             Environment;
    ComPtr<ICoreWebView2CompositionController>   CompositionController;
    ComPtr<ICoreWebView2Controller>              Controller;
    ComPtr<ICoreWebView2>                        WebView;

    //
    // DirectComposition stack — bound to OverlayHwnd.
    //
    ComPtr<IDCompositionDesktopDevice> DCompDevice;
    ComPtr<IDCompositionTarget>        DCompTarget;
    ComPtr<IDCompositionVisual2>       RootVisual;

    //
    // Pending-ops queue — identical to the sibling impl.
    //
    TOptional<FString> PendingNavigate;
    TOptional<bool>    PendingVisible;

    struct FRect { int32 X = 0; int32 Y = 0; int32 W = 0; int32 H = 0; };
    TOptional<FRect>   PendingBounds;
    bool               bPendingReload = false;

    TArray<FString>    PendingOutboundMessages;
    TArray<FString>    PendingScripts;
    TOptional<bool>    PendingMute;

    //
    // Event registration tokens.
    //
    EventRegistrationToken MessageReceivedToken{};
    EventRegistrationToken NavigationStartingToken{};
    EventRegistrationToken NavigationCompletedToken{};
    EventRegistrationToken DocumentTitleChangedToken{};
    EventRegistrationToken ScriptDialogOpeningToken{};
    EventRegistrationToken NewWindowRequestedToken{};
    EventRegistrationToken GotFocusToken{};
    EventRegistrationToken LostFocusToken{};
    EventRegistrationToken ProcessFailedToken{};
    EventRegistrationToken CursorChangedToken{};

    //
    // Lifetime token — same pattern as the sibling impl. Async callbacks
    // capture a TWeakPtr; Shutdown resets the token; stragglers no-op.
    //
    TSharedPtr<int> LifetimeToken = MakeShared<int>(0);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
FInoWebViewImpl_Windows_Composition::FInoWebViewImpl_Windows_Composition()
    : Internal(MakeUnique<FInternal>())
{
}

FInoWebViewImpl_Windows_Composition::~FInoWebViewImpl_Windows_Composition()
{
    Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initialize
//
//    1. Stash ParentHwnd (UE's game window — read-only, not parented)
//    2. Register + create our own overlay HWND (top-level, DComp-friendly)
//    3. Verify WebView2 runtime, create the environment (async)
//    4. On env ready → CreateCoreWebView2CompositionController (async)
//    5. On comp-controller ready → build DComp stack, mount WebView visual,
//         configure controller, hook events, flush pending queue
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Windows_Composition::Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    if (bInitStarted)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("FInoWebViewImpl_Windows_Composition::Initialize called more than once — ignoring."));
        return false;
    }
    if (!ParentNativeHandle)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("FInoWebViewImpl_Windows_Composition::Initialize: parent HWND is null."));
        return false;
    }

    Internal->ParentHwnd = static_cast<HWND>(ParentNativeHandle);
    Internal->Config     = Config;

    // Seed pending ops from config.
    if (!Config.InitialURL.IsEmpty())
    {
        Internal->PendingNavigate = Config.InitialURL;
    }
    Internal->PendingVisible = Config.bVisibleOnCreate;
    Internal->bUserVisible   = Config.bVisibleOnCreate;

    // Verify the WebView2 Runtime is installed.
    {
        LPWSTR VersionString = nullptr;
        const HRESULT HrVer = GetAvailableCoreWebView2BrowserVersionString(nullptr, &VersionString);
        if (FAILED(HrVer) || VersionString == nullptr)
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("WebView2 Runtime not found. Install from "
                     "https://developer.microsoft.com/microsoft-edge/webview2/"));
            return false;
        }
        UE_LOG(LogInoWebUI, Log,
            TEXT("WebView2 Runtime version (composition impl): %s"), VersionString);
        CoTaskMemFree(VersionString);
    }

    // Create our overlay HWND BEFORE the async environment call. The HWND is
    // the composition controller's "parent window" (used for DPI + input
    // routing) — WebView2 needs a valid HWND at CreateCoreWebView2CompositionController time.
    CreateOverlayWindow();
    if (!Internal->OverlayHwnd)
    {
        UE_LOG(LogInoWebUI, Error, TEXT("Failed to create overlay HWND."));
        return false;
    }

    // Per-WebView user data folder.
    const FString UserDataPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / Config.UserDataSubfolder);
    IFileManager::Get().MakeDirectory(*UserDataPath, /*Tree=*/true);

    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 composition-hosting async init started."));

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;

    const HRESULT Hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        *UserDataPath,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, WeakLifetime](HRESULT Result, ICoreWebView2Environment* Env) -> HRESULT
            {
                if (!WeakLifetime.IsValid()) return S_OK;
                OnEnvironmentReady(static_cast<int32>(Result), Env);
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateCoreWebView2EnvironmentWithOptions failed: 0x%08X"),
            static_cast<uint32>(Hr));
        return false;
    }

    bInitStarted = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OnEnvironmentReady — environment alive; spin up the COMPOSITION controller
//  (the whole point of this impl; the sibling Windows class uses the plain
//  CreateCoreWebView2Controller here instead).
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::OnEnvironmentReady(int32 HResult, void* EnvironmentPtr)
{
    check(IsInGameThread());

    if (FAILED(HResult) || !EnvironmentPtr)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("WebView2 environment creation failed: 0x%08X"), static_cast<uint32>(HResult));
        return;
    }

    auto* Env = static_cast<ICoreWebView2Environment*>(EnvironmentPtr);
    Internal->Environment = Env;

    // CreateCoreWebView2CompositionController lives on ICoreWebView2Environment3+.
    ComPtr<ICoreWebView2Environment3> Env3;
    if (FAILED(Env->QueryInterface(IID_PPV_ARGS(&Env3))) || !Env3)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("ICoreWebView2Environment3 unavailable — composition-hosting requires a "
                 "modern WebView2 Runtime. Update the Evergreen runtime and retry."));
        return;
    }

    UE_LOG(LogInoWebUI, Log,
        TEXT("WebView2 environment ready (composition); creating composition controller..."));

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;

    const HRESULT Hr = Env3->CreateCoreWebView2CompositionController(
        Internal->OverlayHwnd,
        Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [this, WeakLifetime](HRESULT Result, ICoreWebView2CompositionController* Ctrl) -> HRESULT
            {
                if (!WeakLifetime.IsValid()) return S_OK;
                OnCompositionControllerReady(static_cast<int32>(Result), Ctrl);
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateCoreWebView2CompositionController failed: 0x%08X"),
            static_cast<uint32>(Hr));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OnCompositionControllerReady — build DComp, mount WebView visual,
//  configure the controller, hook events, and flush queued operations.
//
//  This is the big one. Structure mirrors the sibling impl's OnControllerReady
//  except for the DComp bits at the top.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::OnCompositionControllerReady(int32 HResult, void* CompositionControllerPtr)
{
    check(IsInGameThread());

    if (FAILED(HResult) || !CompositionControllerPtr)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("WebView2 composition controller creation failed: 0x%08X"),
            static_cast<uint32>(HResult));
        return;
    }

    auto* CompCtrl = static_cast<ICoreWebView2CompositionController*>(CompositionControllerPtr);
    Internal->CompositionController = CompCtrl;

    // QI for the shared ICoreWebView2Controller — this is how we access all the
    // usual controller operations (put_Bounds, put_IsVisible, MoveFocus,
    // zoom, focus events). The composition controller exposes both faces.
    if (FAILED(CompCtrl->QueryInterface(IID_PPV_ARGS(&Internal->Controller))))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("QI ICoreWebView2Controller on composition controller failed."));
        return;
    }

    if (FAILED(Internal->Controller->get_CoreWebView2(&Internal->WebView)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("get_CoreWebView2 failed."));
        return;
    }

    // ── Build the DComp stack and attach the WebView visual ─────────────────
    CreateDCompStack();
    if (!Internal->DCompDevice || !Internal->DCompTarget || !Internal->RootVisual)
    {
        UE_LOG(LogInoWebUI, Error, TEXT("DirectComposition stack setup failed."));
        return;
    }

    // Hand our DComp visual to the WebView — it'll render its content INTO
    // this visual rather than into a child HWND. That's the whole trick.
    if (FAILED(CompCtrl->put_RootVisualTarget(Internal->RootVisual.Get())))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("put_RootVisualTarget failed."));
        return;
    }

    // Commit the DComp tree. From here on, the overlay HWND displays the
    // WebView content via DWM's compositor (zero GDI, zero child HWND).
    Internal->DCompDevice->Commit();

    // ── Cursor routing — propagate the WebView's requested cursor to overlay ─
    {
        TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
        CompCtrl->add_CursorChanged(
            Callback<ICoreWebView2CursorChangedEventHandler>(
                [this, WeakLifetime](ICoreWebView2CompositionController* Sender, IUnknown*) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Sender) return S_OK;
                    HCURSOR Cursor = nullptr;
                    if (SUCCEEDED(Sender->get_Cursor(&Cursor)) && Cursor)
                    {
                        // Install for our overlay class so WM_SETCURSOR picks it up.
                        SetClassLongPtr(Internal->OverlayHwnd, GCLP_HCURSOR,
                            reinterpret_cast<LONG_PTR>(Cursor));
                    }
                    return S_OK;
                }).Get(),
            &Internal->CursorChangedToken);
    }

    // ── Inject bridge script (always) ───────────────────────────────────────
    Internal->WebView->AddScriptToExecuteOnDocumentCreated(
        GInoWebUIBridgeScript_Composition, nullptr);

    // ── Inject dev overlay (opt-in) ─────────────────────────────────────────
    if (Internal->Config.bEnableDevTools)
    {
        Internal->WebView->AddScriptToExecuteOnDocumentCreated(
            GInoWebUIDevToolsOverlayScript_Composition, nullptr);
    }

    // ── Navigation events ───────────────────────────────────────────────────
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

                    if (!IsURIAllowed(URI, Internal->Config))
                    {
                        Args->put_Cancel(1);
                        UE_LOG(LogInoWebUI, Warning,
                            TEXT("Navigation blocked by lockdown: %s"), *URI);
                    }
                    if (OnNavigationStartingCallback) OnNavigationStartingCallback(URI);
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

                    FString URI;
                    LPWSTR SourceRaw = nullptr;
                    if (SUCCEEDED(Internal->WebView->get_Source(&SourceRaw)) && SourceRaw)
                    {
                        URI = SourceRaw;
                        CoTaskMemFree(SourceRaw);
                    }
                    if (OnNavigationCompletedCallback) OnNavigationCompletedCallback(bSuccess != 0, URI);
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
                    if (OnDocumentTitleChangedCallback) OnDocumentTitleChangedCallback(Title);
                    return S_OK;
                }).Get(),
            &Internal->DocumentTitleChangedToken);

        // JS dialog suppression (default-deny; Accept only if user allowed).
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

                    if (Internal->Config.bAllowScriptDialogs) { Args->Accept(); }
                    else { UE_LOG(LogInoWebUI, Verbose,
                           TEXT("Suppressed JS dialog (%d): %s"),
                           static_cast<int32>(Kind), *Message); }

                    if (OnScriptDialogCallback) OnScriptDialogCallback(Kind, Message);
                    return S_OK;
                }).Get(),
            &Internal->ScriptDialogOpeningToken);

        // Controller-level focus events.
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

        Internal->WebView->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;

                    COREWEBVIEW2_PROCESS_FAILED_KIND KindRaw =
                        COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                    Args->get_ProcessFailedKind(&KindRaw);

                    const FString Description = FString::Printf(TEXT("Kind=%d"),
                        static_cast<int32>(KindRaw));
                    UE_LOG(LogInoWebUI, Error,
                        TEXT("WebView2 subprocess failed (composition): %s"), *Description);
                    if (OnProcessFailedCallback) OnProcessFailedCallback(Description);
                    return S_OK;
                }).Get(),
            &Internal->ProcessFailedToken);

        // window.open / target="_blank" — block by default.
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
                            TEXT("Blocked window.open: %s"), *URI);
                    }
                    if (OnNewWindowRequestedCallback) OnNewWindowRequestedCallback(URI);
                    return S_OK;
                }).Get(),
            &Internal->NewWindowRequestedToken);
    }

    // ── JS -> UE messaging ──────────────────────────────────────────────────
    {
        TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
        Internal->WebView->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, WeakLifetime](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* Args) -> HRESULT
                {
                    if (!WeakLifetime.IsValid() || !Args) return S_OK;
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
                        if (OnMessageReceivedJson) OnMessageReceivedJson(Msg);
                    }
                    return S_OK;
                }).Get(),
            &Internal->MessageReceivedToken);
    }

    // ── Transparent background ──────────────────────────────────────────────
    if (Internal->Config.bTransparentBackground)
    {
        ComPtr<ICoreWebView2Controller2> Ctrl2;
        if (SUCCEEDED(Internal->Controller.As(&Ctrl2)))
        {
            const COREWEBVIEW2_COLOR Transparent = { 0, 0, 0, 0 };
            Ctrl2->put_DefaultBackgroundColor(Transparent);
        }
    }

    // ── Settings (context menus, dev tools, accelerator keys, UA override) ─
    {
        ComPtr<ICoreWebView2Settings> Settings;
        if (SUCCEEDED(Internal->WebView->get_Settings(&Settings)))
        {
            Settings->put_AreDefaultContextMenusEnabled(Internal->Config.bEnableContextMenus ? 1 : 0);
            Settings->put_AreDevToolsEnabled          (Internal->Config.bEnableDevTools      ? 1 : 0);

            ComPtr<ICoreWebView2Settings3> Settings3;
            if (SUCCEEDED(Settings.As(&Settings3)))
            {
                Settings3->put_AreBrowserAcceleratorKeysEnabled(
                    Internal->Config.bEnableAcceleratorKeys ? 1 : 0);
            }

            if (!Internal->Config.UserAgentOverride.IsEmpty())
            {
                ComPtr<ICoreWebView2Settings2> Settings2;
                if (SUCCEEDED(Settings.As(&Settings2)))
                {
                    Settings2->put_UserAgent(*Internal->Config.UserAgentOverride);
                }
            }
        }

        if (Internal->Config.bStartMuted)
        {
            ComPtr<ICoreWebView2_8> WebView8;
            if (SUCCEEDED(Internal->WebView.As(&WebView8)))
            {
                WebView8->put_IsMuted(1);
            }
        }
    }

    // ── Virtual-host mapping ────────────────────────────────────────────────
    if (!Internal->Config.VirtualHostName.IsEmpty() &&
        !Internal->Config.VirtualHostFolder.IsEmpty())
    {
        ComPtr<ICoreWebView2_3> WebView3;
        if (SUCCEEDED(Internal->WebView.As(&WebView3)))
        {
            const FString& FolderIn = Internal->Config.VirtualHostFolder;
            const FString AbsoluteFolder = FPaths::IsRelative(FolderIn)
                ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / FolderIn)
                : FPaths::ConvertRelativePathToFull(FolderIn);

            if (!IFileManager::Get().DirectoryExists(*AbsoluteFolder))
            {
                UE_LOG(LogInoWebUI, Warning,
                    TEXT("VirtualHostFolder does not exist: %s"), *AbsoluteFolder);
            }

            WebView3->SetVirtualHostNameToFolderMapping(
                *Internal->Config.VirtualHostName,
                *AbsoluteFolder,
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

            UE_LOG(LogInoWebUI, Log,
                TEXT("Virtual host mapped (composition):  https://%s/  ->  %s"),
                *Internal->Config.VirtualHostName, *AbsoluteFolder);
        }
    }

    // Initial bounds — for composition mode, put_Bounds covers the WebView's
    // rect within the overlay HWND. Since our overlay matches the WebView
    // rect exactly (see SyncBounds), the internal bounds are always (0,0,W,H).
    if (!Internal->PendingBounds.IsSet())
    {
        RECT Client;
        ::GetClientRect(Internal->OverlayHwnd, &Client);
        Internal->Controller->put_Bounds(Client);
    }

    // Kick off the UE-window tracking timer (16ms ≈ 60 Hz).
    SetTimer(Internal->OverlayHwnd, kOverlayTrackTimerId, 16, nullptr);

    bReady = true;
    UE_LOG(LogInoWebUI, Log,
        TEXT("WebView2 composition controller ready; DComp visual mounted."));

    ApplyPendingOperations();

    if (OnReadyCallback) OnReadyCallback();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ApplyPendingOperations — flush queue exactly as the sibling impl does.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::ApplyPendingOperations()
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
    if (Internal->PendingOutboundMessages.Num() > 0)
    {
        TArray<FString> Replay = MoveTemp(Internal->PendingOutboundMessages);
        for (const FString& Msg : Replay)
        {
            Internal->WebView->PostWebMessageAsString(*Msg);
        }
    }
    if (Internal->PendingScripts.Num() > 0)
    {
        TArray<FString> Replay = MoveTemp(Internal->PendingScripts);
        for (const FString& Code : Replay) ExecuteJavaScript(Code);
    }
    if (Internal->PendingMute.IsSet())
    {
        SetMuted(Internal->PendingMute.GetValue());
        Internal->PendingMute.Reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public operations
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::Navigate(const FString& URL)
{
    check(IsInGameThread());
    if (!bReady) { Internal->PendingNavigate = URL; return; }

    Internal->WebView->Navigate(*URL);
}

void FInoWebViewImpl_Windows_Composition::Reload()
{
    check(IsInGameThread());
    if (!bReady) { Internal->bPendingReload = true; return; }

    Internal->WebView->Reload();
}

void FInoWebViewImpl_Windows_Composition::SetVisible(bool bVisible)
{
    check(IsInGameThread());

    Internal->bUserVisible = bVisible;

    if (!bReady)
    {
        Internal->PendingVisible = bVisible;
        return;
    }

    // Two things to toggle: the WebView2 controller's own visibility, and
    // our overlay HWND. Both must match or we'll either miss input (overlay
    // hidden) or burn GPU rendering a hidden WebView.
    Internal->Controller->put_IsVisible(bVisible ? 1 : 0);

    if (Internal->OverlayHwnd)
    {
        ::ShowWindow(Internal->OverlayHwnd, bVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void FInoWebViewImpl_Windows_Composition::SyncBounds(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingBounds = FInternal::FRect{ ScreenX, ScreenY, Width, Height };
        return;
    }

    UpdateOverlayToScreenRect(ScreenX, ScreenY, Width, Height);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 3 runtime polish (DevTools, ExecuteJS, mute, focus, zoom, cookies)
//  — identical signatures and behaviour to the sibling impl.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::OpenDevTools()
{
    check(IsInGameThread());
    if (!bReady) return;

    if (!Internal->Config.bEnableDevTools)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("OpenDevTools called but bEnableDevTools was false at construction."));
        return;
    }
    Internal->WebView->OpenDevToolsWindow();
}

void FInoWebViewImpl_Windows_Composition::ExecuteJavaScript(const FString& Code)
{
    check(IsInGameThread());
    if (!bReady) { Internal->PendingScripts.Add(Code); return; }

    Internal->WebView->ExecuteScript(*Code,
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, PCWSTR) -> HRESULT { return S_OK; }).Get());
}

void FInoWebViewImpl_Windows_Composition::SetMuted(bool bMuted)
{
    check(IsInGameThread());
    if (!bReady) { Internal->PendingMute = bMuted; return; }

    ComPtr<ICoreWebView2_8> WebView8;
    if (SUCCEEDED(Internal->WebView.As(&WebView8)))
    {
        WebView8->put_IsMuted(bMuted ? 1 : 0);
    }
}

void FInoWebViewImpl_Windows_Composition::FocusWebView()
{
    check(IsInGameThread());
    if (!bReady || !Internal->Controller) return;

    // Activate the overlay so the thread's focus is here, then hand logical
    // focus to the WebView via the controller.
    if (Internal->OverlayHwnd)
    {
        ::SetForegroundWindow(Internal->OverlayHwnd);
        ::SetFocus(Internal->OverlayHwnd);
    }
    Internal->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void FInoWebViewImpl_Windows_Composition::SetZoomFactor(float Factor)
{
    check(IsInGameThread());
    if (!bReady || !Internal->Controller) return;
    Internal->Controller->put_ZoomFactor(static_cast<double>(Factor));
}

float FInoWebViewImpl_Windows_Composition::GetZoomFactor() const
{
    if (!bReady || !Internal->Controller) return 1.0f;
    double Current = 1.0;
    if (FAILED(Internal->Controller->get_ZoomFactor(&Current))) return 1.0f;
    return static_cast<float>(Current);
}

void FInoWebViewImpl_Windows_Composition::ClearAllCookies()
{
    check(IsInGameThread());
    if (!bReady) return;

    ComPtr<ICoreWebView2_2> WebView2;
    if (FAILED(Internal->WebView.As(&WebView2))) return;

    ComPtr<ICoreWebView2CookieManager> CookieMgr;
    if (SUCCEEDED(WebView2->get_CookieManager(&CookieMgr)) && CookieMgr)
    {
        CookieMgr->DeleteAllCookies();
    }
}

void FInoWebViewImpl_Windows_Composition::SetBackgroundOpaque(bool bOpaque)
{
    check(IsInGameThread());
    if (!bReady) return;

    ComPtr<ICoreWebView2Controller2> Ctrl2;
    if (FAILED(Internal->Controller.As(&Ctrl2))) return;

    COREWEBVIEW2_COLOR Color = bOpaque
        ? COREWEBVIEW2_COLOR{ 255, 255, 255, 255 }
        : COREWEBVIEW2_COLOR{ 0, 0, 0, 0 };
    Ctrl2->put_DefaultBackgroundColor(Color);
}

void FInoWebViewImpl_Windows_Composition::PostMessageJson(const FString& Json)
{
    check(IsInGameThread());
    if (!bReady) { Internal->PendingOutboundMessages.Add(Json); return; }

    Internal->WebView->PostWebMessageAsString(*Json);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Overlay window creation / destruction
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::CreateOverlayWindow()
{
    if (!EnsureOverlayClassRegistered()) return;

    // WS_EX_NOREDIRECTIONBITMAP is the key flag — tells DWM not to allocate a
    // GDI redirection surface for this HWND. DirectComposition paints the
    // window directly; the redirection surface would be wasted memory AND
    // would force DWM into a different (slower) compositing path.
    //
    // WS_EX_TOOLWINDOW keeps the overlay out of the taskbar and Alt+Tab.
    // No WS_EX_LAYERED — that's for GDI alpha, not DComp.
    const DWORD ExStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW;
    const DWORD Style   = WS_POPUP;

    HWND Hwnd = CreateWindowEx(
        ExStyle,
        kOverlayClassName,
        TEXT("InoWebUI Overlay"),
        Style,
        0, 0, 1, 1,                      // final geometry comes from SyncBounds
        Internal->ParentHwnd,            // owner (NOT parent — WS_POPUP)
        nullptr,
        GetModuleHandle(nullptr),
        this);                           // CreateStruct::lpCreateParams

    if (!Hwnd)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWindowEx for overlay failed: 0x%08X"),
            static_cast<uint32>(GetLastError()));
        return;
    }

    // Stash the impl pointer so the static WndProc can route to this instance.
    // WM_NCCREATE handling would also work but this is simpler given we
    // don't need pre-create routing.
    SetWindowLongPtr(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    Internal->OverlayHwnd = Hwnd;
    UE_LOG(LogInoWebUI, Verbose,
        TEXT("Composition overlay HWND created: 0x%p"), Hwnd);
}

void FInoWebViewImpl_Windows_Composition::DestroyOverlayWindow()
{
    if (!Internal->OverlayHwnd) return;

    KillTimer(Internal->OverlayHwnd, kOverlayTrackTimerId);
    DestroyWindow(Internal->OverlayHwnd);
    Internal->OverlayHwnd = nullptr;
}

void FInoWebViewImpl_Windows_Composition::UpdateOverlayToScreenRect(
    int32 ScreenX, int32 ScreenY, int32 Width, int32 Height)
{
    if (!Internal->OverlayHwnd) return;

    Internal->DesiredX = ScreenX;
    Internal->DesiredY = ScreenY;
    Internal->DesiredW = Width;
    Internal->DesiredH = Height;

    // SWP_NOACTIVATE: moving the overlay shouldn't steal foreground activation
    // from the UE window (that would cause constant focus flicker on drag).
    // SWP_NOZORDER: preserve current stacking; the tracking timer will enforce
    // proper z-order (overlay above UE window but not above other apps).
    SetWindowPos(Internal->OverlayHwnd, nullptr,
        ScreenX, ScreenY, Width, Height,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW * (Internal->bUserVisible ? 1 : 0));

    // Update the WebView's internal bounds to match the new overlay size.
    // Bounds are in the target window's client coords → (0,0,W,H) for us.
    if (Internal->Controller)
    {
        RECT Client = { 0, 0, Width, Height };
        Internal->Controller->put_Bounds(Client);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DirectComposition setup
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::CreateDCompStack()
{
    // DCompositionCreateDevice2 accepts a null rendering device — which is
    // what we want. We don't draw anything ourselves; WebView2 renders into
    // the visual via the composition controller.
    ComPtr<IDCompositionDesktopDevice> Device;
    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&Device))))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("DCompositionCreateDevice2 failed."));
        return;
    }
    Internal->DCompDevice = Device;

    // Bind to our overlay HWND. TRUE = topmost within the target's tree.
    ComPtr<IDCompositionTarget> Target;
    if (FAILED(Device->CreateTargetForHwnd(Internal->OverlayHwnd, TRUE, &Target)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("IDCompositionDesktopDevice::CreateTargetForHwnd failed."));
        return;
    }
    Internal->DCompTarget = Target;

    // Create the root visual — this is what we hand WebView2 via
    // put_RootVisualTarget. WebView2 will attach its own sub-visuals to it.
    ComPtr<IDCompositionVisual2> Root;
    if (FAILED(Device->CreateVisual(&Root)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("IDCompositionDesktopDevice::CreateVisual failed."));
        return;
    }
    Internal->RootVisual = Root;

    Target->SetRoot(Root.Get());
}

void FInoWebViewImpl_Windows_Composition::DestroyDCompStack()
{
    // Order matters a little: visual first, then target (unbinds HWND),
    // then device.
    if (Internal->RootVisual)  Internal->RootVisual.Reset();
    if (Internal->DCompTarget) Internal->DCompTarget.Reset();
    if (Internal->DCompDevice) Internal->DCompDevice.Reset();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Overlay WndProc — mouse/pointer forwarding + tracking timer
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK FInoWebViewImpl_Windows_Composition::OverlayWndProcStatic(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* Self = reinterpret_cast<FInoWebViewImpl_Windows_Composition*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!Self)
    {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return Self->OverlayWndProc(hwnd, msg, wParam, lParam);
}

LRESULT FInoWebViewImpl_Windows_Composition::OverlayWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        // Mouse events — forward every one into the composition controller.
        // The COREWEBVIEW2_MOUSE_EVENT_KIND_* enum values equal the Win32
        // WM_* message IDs by design, so we don't need a translation table.
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:   case WM_LBUTTONUP:   case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:   case WM_MBUTTONUP:   case WM_MBUTTONDBLCLK:
        case WM_RBUTTONDOWN:   case WM_RBUTTONUP:   case WM_RBUTTONDBLCLK:
        case WM_XBUTTONDOWN:   case WM_XBUTTONUP:   case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:    case WM_MOUSEHWHEEL:
            ForwardMouseMessage(msg, wParam, lParam);
            return 0;

        case WM_MOUSELEAVE:
            if (Internal && Internal->CompositionController)
            {
                // No coordinates for leave; zero point is fine.
                Internal->CompositionController->SendMouseInput(
                    COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
                    static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(0),
                    0, POINT{ 0, 0 });
            }
            return 0;

        // Cursor — serve whatever WebView2 requested via add_CursorChanged.
        case WM_SETCURSOR:
        {
            const LRESULT Handled = DefWindowProc(hwnd, msg, wParam, lParam);
            return Handled;
        }

        // Tracking timer — re-sync overlay to UE window in case it moved
        // (drag title bar, Windows snap, multi-monitor move). We don't get
        // an event for pure window moves; polling is cheap and reliable.
        case WM_TIMER:
            if (wParam == kOverlayTrackTimerId && Internal && Internal->ParentHwnd)
            {
                RECT ParentRect;
                if (::GetWindowRect(Internal->ParentHwnd, &ParentRect))
                {
                    if (ParentRect.left   != Internal->LastParentRect.left ||
                        ParentRect.top    != Internal->LastParentRect.top  ||
                        ParentRect.right  != Internal->LastParentRect.right ||
                        ParentRect.bottom != Internal->LastParentRect.bottom)
                    {
                        // Parent moved or resized — the subsystem will fire
                        // SyncBounds on resize, but pure moves we handle
                        // ourselves. Shift the overlay by the same delta.
                        const int32 DX = ParentRect.left - Internal->LastParentRect.left;
                        const int32 DY = ParentRect.top  - Internal->LastParentRect.top;

                        if (Internal->LastParentRect.right != 0)  // skip first-call initialisation
                        {
                            UpdateOverlayToScreenRect(
                                Internal->DesiredX + DX,
                                Internal->DesiredY + DY,
                                Internal->DesiredW,
                                Internal->DesiredH);
                        }
                        Internal->LastParentRect = ParentRect;
                    }
                }

                // Also check for UE window minimise/restore — sync overlay visibility.
                const bool bParentVisible = ::IsWindowVisible(Internal->ParentHwnd)
                                         && !::IsIconic(Internal->ParentHwnd);
                const bool bOverlayVisible = ::IsWindowVisible(hwnd);
                const bool bShouldShow = Internal->bUserVisible && bParentVisible;
                if (bShouldShow != bOverlayVisible)
                {
                    ::ShowWindow(hwnd, bShouldShow ? SW_SHOWNOACTIVATE : SW_HIDE);
                }
            }
            return 0;

        // Don't let the overlay steal activation on click — the user's click
        // should hit the overlay (forwarded to WebView) without focus-flashing
        // UE's window. WS_EX_NOACTIVATE is too strong (disables keyboard);
        // WM_MOUSEACTIVATE is the right surgical answer.
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        // Nothing to paint — DComp renders everything.
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_DESTROY:
            // Don't reach into the impl here — Shutdown() drives teardown;
            // we're just finalising the Win32 side.
            return 0;

        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void FInoWebViewImpl_Windows_Composition::ForwardMouseMessage(
    UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!Internal || !Internal->CompositionController) return;

    // Mouse event kind: enum values equal WM_* IDs.
    const auto EventKind = static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg);

    // Modifier keys. Mouse messages pack MK_* flags in wParam's low word
    // EXCEPT for the wheel messages, which also pack delta in the high word
    // (we pull it out separately).
    UINT32 VirtualKeys = 0;
    const WORD KeyState = (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
        ? GET_KEYSTATE_WPARAM(wParam)
        : LOWORD(wParam);

    if (KeyState & MK_LBUTTON)  VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if (KeyState & MK_MBUTTON)  VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if (KeyState & MK_RBUTTON)  VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (KeyState & MK_SHIFT)    VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if (KeyState & MK_CONTROL)  VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if (KeyState & MK_XBUTTON1) VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if (KeyState & MK_XBUTTON2) VirtualKeys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;

    // mouseData: wheel delta, or x-button number, else 0.
    UINT32 MouseData = 0;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        MouseData = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wParam));
    }
    else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK)
    {
        MouseData = GET_XBUTTON_WPARAM(wParam);
    }

    // Point. For plain mouse messages, lParam is CLIENT coords of our overlay,
    // which is what SendMouseInput expects. For wheel messages, lParam is
    // SCREEN coords (Win32 quirk) — convert to client for the WebView.
    POINT Pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        ::ScreenToClient(Internal->OverlayHwnd, &Pt);
    }

    Internal->CompositionController->SendMouseInput(
        EventKind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(VirtualKeys),
        MouseData, Pt);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown — teardown order is more involved than the sibling impl:
//    1. Invalidate lifetime token → async callbacks no-op
//    2. Close composition controller → WebView stops writing to our visual
//    3. Release DComp (visual → target → device)
//    4. Destroy overlay HWND (must outlive the controller close)
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::Shutdown()
{
    check(IsInGameThread());
    if (!Internal) return;

    if (!Internal->LifetimeToken.IsValid()) return;
    Internal->LifetimeToken.Reset();

    if (Internal->Controller)
    {
        Internal->Controller->Close();
    }
    Internal->WebView.Reset();
    Internal->Controller.Reset();
    Internal->CompositionController.Reset();
    Internal->Environment.Reset();

    DestroyDCompStack();
    DestroyOverlayWindow();

    bReady = false;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 composition shutdown complete."));
}

#endif // PLATFORM_WINDOWS
