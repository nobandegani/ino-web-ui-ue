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
//
//  These live in a uniquely-named namespace (NOT anonymous) because UE uses
//  unity builds: anonymous-namespace helpers in multiple .cpp files collide
//  when the translation units are concatenated. Anonymous-namespace helpers
//  work fine in isolated compilation; the named wrapper makes unity builds
//  also work.
// ─────────────────────────────────────────────────────────────────────────────
namespace InoWebUICompositionPriv
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
} // namespace InoWebUICompositionPriv

// ─────────────────────────────────────────────────────────────────────────────
//  Bridge + dev-overlay JS
//
//  Identical to the copies in InoWebViewImpl_Windows.cpp. Kept separately so
//  the two Windows impls have zero compile-time coupling. If you modify
//  either script, also update:
//    • Source/InoWebUI/Private/Impl/Windows/InoWebViewImpl_Windows.cpp
//    • Source/InoWebUI/Java/src/net/inoland/webui/InoWebViewAndroid.java
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
//  FInternal — ALL Win32 / DComp / WebView2 state and helpers.
//
//  Everything that touches Windows.h or WebView2.h lives here, keeping those
//  headers out of the class header (header hygiene rule).
// ─────────────────────────────────────────────────────────────────────────────
struct FInoWebViewImpl_Windows_Composition::FInternal
{
    // UE's game window — read-only reference, not a parent.
    HWND ParentHwnd = nullptr;

    // Our top-level overlay HWND. Owner = ParentHwnd (for z-order / cleanup).
    HWND OverlayHwnd = nullptr;

    // Back-pointer to the owning impl so we can reach the BP-facing callbacks
    // (OnGotFocusCallback etc.) from within WndProc / input forwarding if
    // needed. Raw pointer; Internal is owned by the impl so lifetime tracks.
    FInoWebViewImpl_Windows_Composition* Self = nullptr;

    // Desired WebView rect in SCREEN coords, as last reported by SyncBounds.
    int32 DesiredX = 0;
    int32 DesiredY = 0;
    int32 DesiredW = 0;
    int32 DesiredH = 0;

    // Last observed parent-window rect (screen coords), for move tracking.
    RECT LastParentRect { 0, 0, 0, 0 };

    // User-level visibility preference — overlay is also hidden when the
    // parent is minimised, even if bUserVisible is true.
    bool bUserVisible = true;

    FInoWebViewConfig Config;

    // COM — WebView2 side.
    ComPtr<ICoreWebView2Environment>             Environment;
    ComPtr<ICoreWebView2CompositionController>   CompositionController;
    ComPtr<ICoreWebView2Controller>              Controller;
    ComPtr<ICoreWebView2>                        WebView;

    // COM — DirectComposition side.
    ComPtr<IDCompositionDesktopDevice> DCompDevice;
    ComPtr<IDCompositionTarget>        DCompTarget;
    ComPtr<IDCompositionVisual2>       RootVisual;

    // Pending-ops queue (same contract as the sibling impl).
    TOptional<FString> PendingNavigate;
    TOptional<bool>    PendingVisible;

    struct FRect { int32 X = 0; int32 Y = 0; int32 W = 0; int32 H = 0; };
    TOptional<FRect>   PendingBounds;
    bool               bPendingReload = false;

    TArray<FString>    PendingOutboundMessages;
    TArray<FString>    PendingScripts;
    TOptional<bool>    PendingMute;

    // Event registration tokens.
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

    // Lifetime token — see sibling impl for the weak-ptr pattern.
    TSharedPtr<int> LifetimeToken = MakeShared<int>(0);

    // ── Win32 / DComp helpers — all live in the .cpp to keep Win32 types
    //    out of the class header. ───────────────────────────────────────
    bool    CreateOverlayWindow();
    void    DestroyOverlayWindow();
    bool    CreateDCompStack();
    void    DestroyDCompStack();
    void    UpdateOverlayToScreenRect(int32 X, int32 Y, int32 W, int32 H);
    void    ForwardMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OverlayWndProc   (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Overlay window class + WndProc trampoline
//
//  File-scope (not static class members) so the WNDCLASSEX registration can
//  wire directly to a C-style function pointer without leaking Win32 types
//  through the class header.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    const TCHAR* kOverlayClassName = TEXT("InoWebUIComposition_Overlay");
    bool         bOverlayClassRegistered = false;

    /** Timer id for the UE-window tracking poll (see FInternal::OverlayWndProc WM_TIMER). */
    constexpr UINT_PTR kOverlayTrackTimerId = 0x494E4F01; // "INO\x01"

    /**
     * File-scope trampoline. Looks up the FInternal* we stashed in
     * GWLP_USERDATA and forwards to its OverlayWndProc method. Messages
     * that arrive before the USERDATA slot is populated (WM_NCCREATE etc.)
     * fall through to DefWindowProc.
     */
    LRESULT CALLBACK OverlayWndProcTrampoline(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* Internal = reinterpret_cast<FInoWebViewImpl_Windows_Composition::FInternal*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!Internal)
        {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        return Internal->OverlayWndProc(hwnd, msg, wParam, lParam);
    }

    bool EnsureOverlayClassRegistered()
    {
        if (bOverlayClassRegistered) return true;

        WNDCLASSEX Wc = {};
        Wc.cbSize        = sizeof(WNDCLASSEX);
        Wc.style         = CS_HREDRAW | CS_VREDRAW;
        Wc.lpfnWndProc   = &OverlayWndProcTrampoline;
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
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  FInternal method implementations
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Windows_Composition::FInternal::CreateOverlayWindow()
{
    if (!EnsureOverlayClassRegistered()) return false;

    // WS_EX_NOREDIRECTIONBITMAP: tell DWM not to allocate a GDI redirection
    // surface — DComp paints the window directly. Required for the
    // "zero-copy composition" path we're using here.
    //
    // WS_EX_TOOLWINDOW: keeps the overlay out of the taskbar / Alt+Tab.
    // No WS_EX_LAYERED — that's for GDI alpha, not DComp.
    const DWORD ExStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW;
    const DWORD Style   = WS_POPUP;

    HWND Hwnd = CreateWindowEx(
        ExStyle,
        kOverlayClassName,
        TEXT("InoWebUI Overlay"),
        Style,
        0, 0, 1, 1,                      // final geometry comes from SyncBounds
        ParentHwnd,                      // owner (NOT parent — WS_POPUP)
        nullptr,
        GetModuleHandle(nullptr),
        nullptr);

    if (!Hwnd)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWindowEx for overlay failed: 0x%08X"),
            static_cast<uint32>(GetLastError()));
        return false;
    }

    // Stash the FInternal* so the trampoline can route to this instance.
    SetWindowLongPtr(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    OverlayHwnd = Hwnd;
    UE_LOG(LogInoWebUI, Verbose,
        TEXT("Composition overlay HWND created: 0x%p"), Hwnd);
    return true;
}

void FInoWebViewImpl_Windows_Composition::FInternal::DestroyOverlayWindow()
{
    if (!OverlayHwnd) return;
    KillTimer(OverlayHwnd, kOverlayTrackTimerId);
    DestroyWindow(OverlayHwnd);
    OverlayHwnd = nullptr;
}

bool FInoWebViewImpl_Windows_Composition::FInternal::CreateDCompStack()
{
    // DCompositionCreateDevice2 accepts a null rendering device — we don't
    // draw anything ourselves; WebView2 renders INTO the visual.
    ComPtr<IDCompositionDesktopDevice> Device;
    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&Device))))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("DCompositionCreateDevice2 failed."));
        return false;
    }
    DCompDevice = Device;

    ComPtr<IDCompositionTarget> Target;
    // Use 1 not TRUE — HideWindowsPlatformTypes.h un-#defines TRUE/FALSE
    // macros above; BOOL is still a valid typedef for int, so 1 fits.
    if (FAILED(Device->CreateTargetForHwnd(OverlayHwnd, 1, &Target)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("CreateTargetForHwnd failed."));
        return false;
    }
    DCompTarget = Target;

    ComPtr<IDCompositionVisual2> Root;
    if (FAILED(Device->CreateVisual(&Root)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("CreateVisual failed."));
        return false;
    }
    RootVisual = Root;

    Target->SetRoot(Root.Get());
    return true;
}

void FInoWebViewImpl_Windows_Composition::FInternal::DestroyDCompStack()
{
    // Order: visual first, then target (unbinds HWND), then device.
    RootVisual.Reset();
    DCompTarget.Reset();
    DCompDevice.Reset();
}

void FInoWebViewImpl_Windows_Composition::FInternal::UpdateOverlayToScreenRect(
    int32 ScreenX, int32 ScreenY, int32 Width, int32 Height)
{
    if (!OverlayHwnd) return;

    DesiredX = ScreenX;
    DesiredY = ScreenY;
    DesiredW = Width;
    DesiredH = Height;

    // SWP_NOACTIVATE: moving the overlay must not steal foreground activation
    // from UE (would cause focus flicker on drag).
    // SWP_NOZORDER: preserve current stacking; tracking timer enforces proper
    // order.
    UINT Flags = SWP_NOACTIVATE | SWP_NOZORDER;
    if (bUserVisible) Flags |= SWP_SHOWWINDOW;

    SetWindowPos(OverlayHwnd, nullptr, ScreenX, ScreenY, Width, Height, Flags);

    // Update WebView's own bounds to match. In composition mode, Bounds are
    // in the target HWND's client coords → (0,0,W,H) since our overlay is
    // exactly the WebView rect.
    if (Controller)
    {
        RECT Client = { 0, 0, Width, Height };
        Controller->put_Bounds(Client);
    }
}

void FInoWebViewImpl_Windows_Composition::FInternal::ForwardMouseMessage(
    UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!CompositionController) return;

    // Mouse event kind: the COREWEBVIEW2_MOUSE_EVENT_KIND_* enum values equal
    // the Win32 WM_* message IDs by design, so a direct cast is legitimate.
    const auto EventKind = static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg);

    // Modifiers. Wheel messages pack the delta in the high word of wParam,
    // so we pull the keystate out via the dedicated macro in that case.
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

    UINT32 MouseData = 0;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        MouseData = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wParam));
    }
    else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK)
    {
        MouseData = GET_XBUTTON_WPARAM(wParam);
    }

    // Coordinates. Mouse messages use CLIENT coords of the receiving HWND;
    // wheel messages use SCREEN coords (Win32 quirk) so we convert.
    POINT Pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        ::ScreenToClient(OverlayHwnd, &Pt);
    }

    CompositionController->SendMouseInput(
        EventKind,
        static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(VirtualKeys),
        MouseData, Pt);
}

LRESULT FInoWebViewImpl_Windows_Composition::FInternal::OverlayWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:   case WM_LBUTTONUP:   case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:   case WM_MBUTTONUP:   case WM_MBUTTONDBLCLK:
        case WM_RBUTTONDOWN:   case WM_RBUTTONUP:   case WM_RBUTTONDBLCLK:
        case WM_XBUTTONDOWN:   case WM_XBUTTONUP:   case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:    case WM_MOUSEHWHEEL:
            ForwardMouseMessage(msg, wParam, lParam);
            return 0;

        case WM_MOUSELEAVE:
            if (CompositionController)
            {
                CompositionController->SendMouseInput(
                    COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
                    static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(0),
                    0, POINT{ 0, 0 });
            }
            return 0;

        case WM_SETCURSOR:
            // Fall through to DefWindowProc — it honours the class cursor,
            // which we update from add_CursorChanged.
            break;

        case WM_TIMER:
            if (wParam == kOverlayTrackTimerId && ParentHwnd)
            {
                RECT ParentRect;
                if (::GetWindowRect(ParentHwnd, &ParentRect))
                {
                    if (ParentRect.left   != LastParentRect.left  ||
                        ParentRect.top    != LastParentRect.top   ||
                        ParentRect.right  != LastParentRect.right ||
                        ParentRect.bottom != LastParentRect.bottom)
                    {
                        // UE window moved/resized. Shift overlay by the
                        // same delta. The subsystem fires SyncBounds on
                        // resize; pure moves we handle here.
                        const int32 DX = ParentRect.left - LastParentRect.left;
                        const int32 DY = ParentRect.top  - LastParentRect.top;

                        if (LastParentRect.right != 0)  // skip first-call init
                        {
                            UpdateOverlayToScreenRect(
                                DesiredX + DX, DesiredY + DY,
                                DesiredW, DesiredH);
                        }
                        LastParentRect = ParentRect;
                    }
                }

                // Sync overlay visibility with parent minimise/restore.
                const bool bParentVisible = ::IsWindowVisible(ParentHwnd)
                                         && !::IsIconic(ParentHwnd);
                const bool bOverlayVisible = ::IsWindowVisible(hwnd) != 0;
                const bool bShouldShow = bUserVisible && bParentVisible;
                if (bShouldShow != bOverlayVisible)
                {
                    ::ShowWindow(hwnd, bShouldShow ? SW_SHOWNOACTIVATE : SW_HIDE);
                }
            }
            return 0;

        case WM_MOUSEACTIVATE:
            // Don't let clicks on the overlay steal activation from UE.
            return MA_NOACTIVATE;

        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;

        case WM_DESTROY:
            return 0;

        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Outer class — construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
FInoWebViewImpl_Windows_Composition::FInoWebViewImpl_Windows_Composition()
    : Internal(MakeUnique<FInternal>())
{
    Internal->Self = this;
}

FInoWebViewImpl_Windows_Composition::~FInoWebViewImpl_Windows_Composition()
{
    Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initialize
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Windows_Composition::Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    if (bInitStarted)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("FInoWebViewImpl_Windows_Composition::Initialize called twice."));
        return false;
    }
    if (!ParentNativeHandle)
    {
        UE_LOG(LogInoWebUI, Error, TEXT("Composition init: parent HWND is null."));
        return false;
    }

    Internal->ParentHwnd = static_cast<HWND>(ParentNativeHandle);
    Internal->Config     = Config;

    if (!Config.InitialURL.IsEmpty())
    {
        Internal->PendingNavigate = Config.InitialURL;
    }
    Internal->PendingVisible = Config.bVisibleOnCreate;
    Internal->bUserVisible   = Config.bVisibleOnCreate;

    // Verify WebView2 runtime.
    {
        LPWSTR VersionString = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &VersionString))
            || !VersionString)
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("WebView2 Runtime not found. Install the Evergreen runtime."));
            return false;
        }
        UE_LOG(LogInoWebUI, Log,
            TEXT("WebView2 Runtime version (composition): %s"), VersionString);
        CoTaskMemFree(VersionString);
    }

    // Create overlay HWND up front — needed as the parent arg to
    // CreateCoreWebView2CompositionController.
    if (!Internal->CreateOverlayWindow())
    {
        UE_LOG(LogInoWebUI, Error, TEXT("Failed to create overlay HWND."));
        return false;
    }

    const FString UserDataPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / Config.UserDataSubfolder);
    IFileManager::Get().MakeDirectory(*UserDataPath, /*Tree=*/true);

    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 composition init started."));

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
//  OnEnvironmentReady — spin up the COMPOSITION controller.
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

    ComPtr<ICoreWebView2Environment3> Env3;
    if (FAILED(Env->QueryInterface(IID_PPV_ARGS(&Env3))) || !Env3)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("ICoreWebView2Environment3 unavailable — composition-hosting requires a "
                 "modern WebView2 Runtime."));
        return;
    }

    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 env ready; creating composition controller..."));

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
//  hook events, flush pending operations. Big function, mirrors the
//  sibling impl's OnControllerReady with DComp additions.
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

    if (FAILED(CompCtrl->QueryInterface(IID_PPV_ARGS(&Internal->Controller))))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("QI ICoreWebView2Controller failed."));
        return;
    }
    if (FAILED(Internal->Controller->get_CoreWebView2(&Internal->WebView)))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("get_CoreWebView2 failed."));
        return;
    }

    // ── Build DComp stack and attach the WebView visual ─────────────────────
    if (!Internal->CreateDCompStack())
    {
        UE_LOG(LogInoWebUI, Error, TEXT("DirectComposition stack setup failed."));
        return;
    }

    if (FAILED(CompCtrl->put_RootVisualTarget(Internal->RootVisual.Get())))
    {
        UE_LOG(LogInoWebUI, Error, TEXT("put_RootVisualTarget failed."));
        return;
    }
    Internal->DCompDevice->Commit();

    // ── Cursor routing ──────────────────────────────────────────────────────
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
                        SetClassLongPtr(Internal->OverlayHwnd, GCLP_HCURSOR,
                            reinterpret_cast<LONG_PTR>(Cursor));
                    }
                    return S_OK;
                }).Get(),
            &Internal->CursorChangedToken);
    }

    // ── Inject scripts ──────────────────────────────────────────────────────
    Internal->WebView->AddScriptToExecuteOnDocumentCreated(
        GInoWebUIBridgeScript_Composition, nullptr);

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

                    if (!InoWebUICompositionPriv::IsURIAllowed(URI, Internal->Config))
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

    // ── Settings ────────────────────────────────────────────────────────────
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

    // Initial Bounds — overlay's client rect; our overlay == WebView rect.
    if (!Internal->PendingBounds.IsSet())
    {
        RECT Client;
        ::GetClientRect(Internal->OverlayHwnd, &Client);
        Internal->Controller->put_Bounds(Client);
    }

    // Start UE-window tracking timer (60 Hz).
    SetTimer(Internal->OverlayHwnd, kOverlayTrackTimerId, 16, nullptr);

    bReady = true;
    UE_LOG(LogInoWebUI, Log,
        TEXT("Composition controller ready; DComp visual mounted."));

    ApplyPendingOperations();

    if (OnReadyCallback) OnReadyCallback();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ApplyPendingOperations
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
//  Public operations (thin wrappers; helpers live on FInternal)
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

    if (!bReady) { Internal->PendingVisible = bVisible; return; }

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
    Internal->UpdateOverlayToScreenRect(ScreenX, ScreenY, Width, Height);
}

void FInoWebViewImpl_Windows_Composition::OpenDevTools()
{
    check(IsInGameThread());
    if (!bReady) return;

    if (!Internal->Config.bEnableDevTools)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("OpenDevTools called but bEnableDevTools was false."));
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
//  Shutdown — composition path needs a bigger teardown than the sibling impl:
//    1. Invalidate lifetime token → async callbacks no-op
//    2. Close controller → WebView stops rendering
//    3. Release DComp (visual → target → device)
//    4. Destroy overlay HWND
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

    Internal->DestroyDCompStack();
    Internal->DestroyOverlayWindow();

    bReady = false;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 composition shutdown complete."));
}

#endif // PLATFORM_WINDOWS
