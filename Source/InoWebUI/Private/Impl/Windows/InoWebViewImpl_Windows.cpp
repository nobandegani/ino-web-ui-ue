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

    /** Token for the add_WebMessageReceived registration. */
    EventRegistrationToken MessageReceivedToken{};

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
