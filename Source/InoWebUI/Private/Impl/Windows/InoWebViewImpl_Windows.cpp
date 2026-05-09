// Copyright Inoland. All Rights Reserved.

#include "InoWebViewImpl_Windows.h"

#if PLATFORM_WINDOWS

#include "InoWebUILog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

// ── Windows + WebView2 headers ─────────────────────────────────────────────
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"

THIRD_PARTY_INCLUDES_START
#include <Windows.h>
#include <wrl.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <shlwapi.h>            // SHCreateMemStream
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

// Injected JS — bridge + dev-tools overlay (generated from Source/InoWebUI/JS/*.js)
// Re-run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1 if you edit the .js files.
#include "Generated/InoWebUIScripts.generated.h"

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

    /** Pending LoadHTMLString — queued before ready. */
    struct FPendingHTML { FString HTML; FString BaseURI; };
    TOptional<FPendingHTML> PendingHTMLLoad;

    /** Pending LoadURLWithHeaders — queued before ready. */
    struct FPendingHeadered { FString URL; TMap<FString, FString> Headers; };
    TOptional<FPendingHeadered> PendingHeaderedLoad;

    /** Pending SetCookie calls — queued before ready. */
    struct FPendingCookie { FString URL; FString Cookie; };
    TArray<FPendingCookie> PendingCookies;

    /** Pending ClearAllData call — queued before ready. */
    bool bPendingClearAllData = false;

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
    //
    // Order matters: SetCookie calls go into PendingCookies, navigation goes
    // into PendingNavigate or PendingHeaderedLoad. ApplyPendingOperations
    // replays cookies BEFORE any navigation, so Config.InitialCookies are
    // guaranteed to be in the cookie store by the time the initial request
    // fires.
    for (const FInoInitialCookie& InitCookie : Config.InitialCookies)
    {
        SetCookie(InitCookie.URL, InitCookie.Cookie);
    }
    if (!Config.InitialURL.IsEmpty())
    {
        if (Config.InitialHeaders.Num() > 0)
        {
            LoadURLWithHeaders(Config.InitialURL, Config.InitialHeaders);
        }
        else
        {
            Navigate(Config.InitialURL);
        }
        // Seed cached URL so GetURL() is meaningful before the first
        // NavigationCompleted fires.
        CachedURL = Config.InitialURL;
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

    // ── Outer-scroll lock — game-UI default. Inject CSS that pins
    //    html/body to overflow:hidden so vertical drag (touch on a
    //    Surface, mouse-wheel on a desktop) doesn't drift the whole
    //    page. Long content uses an inner overflow:auto container. ────────
    if (!Internal->Config.bAllowOuterScroll)
    {
        Internal->WebView->AddScriptToExecuteOnDocumentCreated(
            GInoWebUILockScrollScript,
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
                    else
                    {
                        // Real navigation begins; flip cached state.
                        bCachedLoading = true;
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

                    // Cache state — readers expect O(1) URL/IsLoading getters.
                    CachedURL      = URI;
                    bCachedLoading = false;

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

                    CachedTitle = Title;

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
            Settings->put_IsStatusBarEnabled          (Internal->Config.bShowStatusBar          ? 1 : 0);

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

    // Cookies replay FIRST so they're in the cookie store before any
    // navigation request goes out. Config::InitialCookies relies on this
    // ordering to seed SSO / auth cookies for the very first request.
    if (Internal->PendingCookies.Num() > 0)
    {
        TArray<FInternal::FPendingCookie> Replay = MoveTemp(Internal->PendingCookies);
        for (const FInternal::FPendingCookie& C : Replay)
        {
            SetCookie(C.URL, C.Cookie);
        }
    }

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

    // Replay any queued LoadHTMLString. PendingNavigate already replayed
    // above; if both are set, PendingHTMLLoad wins because LoadHTMLString
    // ought to be the most recently requested action.
    if (Internal->PendingHTMLLoad.IsSet())
    {
        const auto Snap = Internal->PendingHTMLLoad.GetValue();
        Internal->PendingHTMLLoad.Reset();
        LoadHTMLString(Snap.HTML, Snap.BaseURI);
    }

    // Replay queued LoadURLWithHeaders. Note: PendingCookies were already
    // replayed at the top of this function so they apply to this request.
    if (Internal->PendingHeaderedLoad.IsSet())
    {
        const auto Snap = Internal->PendingHeaderedLoad.GetValue();
        Internal->PendingHeaderedLoad.Reset();
        LoadURLWithHeaders(Snap.URL, Snap.Headers);
    }

    // Replay a queued ClearAllData.
    if (Internal->bPendingClearAllData)
    {
        Internal->bPendingClearAllData = false;
        ClearAllData();
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
        // Most-recent navigation intent wins. Without these resets, an
        // earlier headered/HTML load queued (e.g. from Initialize using
        // Config.InitialHeaders) could replay AFTER this Navigate and
        // override it.
        Internal->PendingNavigate = URL;
        Internal->PendingHeaderedLoad.Reset();
        Internal->PendingHTMLLoad.Reset();
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
// ─────────────────────────────────────────────────────────────────────────────
//  Browser-style nav
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::GoBack()
{
    check(IsInGameThread());
    // Going back before any navigation has happened is meaningless — don't queue.
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->GoBack();
}

void FInoWebViewImpl_Windows::GoForward()
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->GoForward();
}

bool FInoWebViewImpl_Windows::CanGoBack() const
{
    if (!bReady || !Internal || !Internal->WebView) return false;
    BOOL b = 0;
    if (FAILED(Internal->WebView->get_CanGoBack(&b))) return false;
    return b != 0;
}

bool FInoWebViewImpl_Windows::CanGoForward() const
{
    if (!bReady || !Internal || !Internal->WebView) return false;
    BOOL b = 0;
    if (FAILED(Internal->WebView->get_CanGoForward(&b))) return false;
    return b != 0;
}

void FInoWebViewImpl_Windows::StopLoading()
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->Stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  LoadHTMLString — WebView2 has NavigateToString, which doesn't accept a base
//  URI. Document the BaseURI ignore at runtime to avoid silent surprises.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::LoadHTMLString(const FString& HTML, const FString& BaseURI)
{
    check(IsInGameThread());
    if (!bReady)
    {
        // Most-recent navigation intent wins; clear sibling queues.
        Internal->PendingHTMLLoad = FInternal::FPendingHTML{ HTML, BaseURI };
        Internal->PendingNavigate.Reset();
        Internal->PendingHeaderedLoad.Reset();
        return;
    }

    if (!BaseURI.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Verbose,
            TEXT("LoadHTMLString: BaseURI ('%s') is ignored on Windows — WebView2's "
                 "NavigateToString uses about:blank as origin. Use a virtual-host "
                 "mapping to serve HTML through a real https://<host>/ origin."),
            *BaseURI);
    }

    const HRESULT Hr = Internal->WebView->NavigateToString(*HTML);
    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("NavigateToString failed: 0x%08X"), static_cast<uint32>(Hr));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cookie parser — accepts the standard HTTP cookie syntax:
//    name=value; Path=/; Domain=...; Expires=...; HttpOnly; Secure; SameSite=...
//
//  ICoreWebView2Cookie's Path/Domain are READ-ONLY post-construction — they
//  must be supplied to CookieManager::CreateCookie. Everything else (Expires,
//  IsHttpOnly, IsSecure, SameSite) has a setter and we apply those after.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    bool ParseHttpDateToUnixSeconds(const FString& Date, double& OutSeconds)
    {
        FDateTime DT;
        if (FDateTime::ParseHttpDate(Date, DT))
        {
            OutSeconds = static_cast<double>(DT.ToUnixTimestamp());
            return true;
        }
        return false;
    }

    /**
     * Parsed cookie pieces. Path / Domain are created up-front via the
     * cookie manager; the remaining fields are applied via setters.
     */
    struct FParsedCookie
    {
        FString  Name;
        FString  Value;
        FString  Path   = TEXT("/");
        FString  Domain;
        TOptional<double> Expires;     // Unix seconds
        bool     bSecure   = false;
        bool     bHttpOnly = false;
        TOptional<COREWEBVIEW2_COOKIE_SAME_SITE_KIND> SameSite;
        bool     bValid = false;
    };

    FParsedCookie ParseHttpCookie(const FString& Raw)
    {
        FParsedCookie Out;

        TArray<FString> Parts;
        Raw.ParseIntoArray(Parts, TEXT(";"), /*bCullEmpty=*/ true);
        if (Parts.Num() == 0) return Out;

        FString First = Parts[0].TrimStartAndEnd();
        int32 Eq = INDEX_NONE;
        if (!First.FindChar(TEXT('='), Eq)) return Out;

        Out.Name  = First.Left(Eq).TrimStartAndEnd();
        Out.Value = First.Mid(Eq + 1).TrimStartAndEnd();
        Out.bValid = true;

        for (int32 i = 1; i < Parts.Num(); ++i)
        {
            const FString Trim = Parts[i].TrimStartAndEnd();
            int32 EqIdx = INDEX_NONE;
            FString K, V;
            if (Trim.FindChar(TEXT('='), EqIdx))
            {
                K = Trim.Left(EqIdx).TrimStartAndEnd();
                V = Trim.Mid(EqIdx + 1).TrimStartAndEnd();
            }
            else
            {
                K = Trim;
            }

            if      (K.Equals(TEXT("Path"),    ESearchCase::IgnoreCase)) Out.Path   = V;
            else if (K.Equals(TEXT("Domain"),  ESearchCase::IgnoreCase)) Out.Domain = V;
            else if (K.Equals(TEXT("Expires"), ESearchCase::IgnoreCase))
            {
                double Sec = 0.0;
                if (ParseHttpDateToUnixSeconds(V, Sec)) Out.Expires = Sec;
            }
            else if (K.Equals(TEXT("Max-Age"), ESearchCase::IgnoreCase))
            {
                const int64 Secs = FCString::Atoi64(*V);
                Out.Expires = static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp() + Secs);
            }
            else if (K.Equals(TEXT("HttpOnly"), ESearchCase::IgnoreCase)) Out.bHttpOnly = true;
            else if (K.Equals(TEXT("Secure"),   ESearchCase::IgnoreCase)) Out.bSecure   = true;
            else if (K.Equals(TEXT("SameSite"), ESearchCase::IgnoreCase))
            {
                COREWEBVIEW2_COOKIE_SAME_SITE_KIND Kind = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX;
                if      (V.Equals(TEXT("None"),   ESearchCase::IgnoreCase)) Kind = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
                else if (V.Equals(TEXT("Strict"), ESearchCase::IgnoreCase)) Kind = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT;
                Out.SameSite = Kind;
            }
            // Unknown attributes silently ignored — be liberal in what you accept.
        }

        return Out;
    }
}

void FInoWebViewImpl_Windows::SetCookie(const FString& URL, const FString& Cookie)
{
    check(IsInGameThread());
    if (!bReady)
    {
        Internal->PendingCookies.Add(FInternal::FPendingCookie{ URL, Cookie });
        return;
    }

    ComPtr<ICoreWebView2_2> WebView2;
    if (FAILED(Internal->WebView.As(&WebView2)) || !WebView2)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetCookie: ICoreWebView2_2 unavailable."));
        return;
    }
    ComPtr<ICoreWebView2CookieManager> CookieMgr;
    if (FAILED(WebView2->get_CookieManager(&CookieMgr)) || !CookieMgr)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetCookie: could not obtain CookieManager."));
        return;
    }

    FParsedCookie P = ParseHttpCookie(Cookie);
    if (!P.bValid)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetCookie: malformed cookie string: '%s'"), *Cookie);
        return;
    }

    // Domain default — infer from URL host if the cookie didn't specify.
    FString Domain = P.Domain;
    if (Domain.IsEmpty())
    {
        Domain = ExtractHost(URL);
    }

    ComPtr<ICoreWebView2Cookie> NewCookie;
    if (FAILED(CookieMgr->CreateCookie(*P.Name, *P.Value, *Domain, *P.Path, &NewCookie))
        || !NewCookie)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetCookie: CreateCookie failed for '%s'"), *P.Name);
        return;
    }

    if (P.Expires.IsSet())  NewCookie->put_Expires(P.Expires.GetValue());
    if (P.bHttpOnly)        NewCookie->put_IsHttpOnly(1);
    if (P.bSecure)          NewCookie->put_IsSecure(1);
    if (P.SameSite.IsSet()) NewCookie->put_SameSite(P.SameSite.GetValue());

    if (FAILED(CookieMgr->AddOrUpdateCookie(NewCookie.Get())))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("SetCookie: AddOrUpdateCookie failed for '%s'"), *P.Name);
        return;
    }
    UE_LOG(LogInoWebUI, Verbose, TEXT("SetCookie applied: %s = %s"), *P.Name, *P.Value);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ClearAllData — modern path: ICoreWebView2_13->Profile->ClearBrowsingData
//                 fallback:    ClearAllCookies + ExecuteScript("…clear()")
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::ClearAllData()
{
    check(IsInGameThread());
    if (!bReady)
    {
        Internal->bPendingClearAllData = true;
        return;
    }

    // Try the modern profile-level wipe first.
    ComPtr<ICoreWebView2_13> WebView13;
    if (SUCCEEDED(Internal->WebView.As(&WebView13)) && WebView13)
    {
        ComPtr<ICoreWebView2Profile> Profile;
        if (SUCCEEDED(WebView13->get_Profile(&Profile)) && Profile)
        {
            ComPtr<ICoreWebView2Profile2> Profile2;
            if (SUCCEEDED(Profile.As(&Profile2)) && Profile2)
            {
                TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
                const HRESULT Hr = Profile2->ClearBrowsingData(
                    COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE,
                    Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                        [WeakLifetime](HRESULT) -> HRESULT
                        {
                            // Lifetime guard — even though we don't touch state,
                            // keep the pattern consistent for future expansion.
                            (void)WeakLifetime;
                            return S_OK;
                        }).Get());
                if (SUCCEEDED(Hr))
                {
                    UE_LOG(LogInoWebUI, Log, TEXT("ClearAllData via Profile2."));
                    return;
                }
                UE_LOG(LogInoWebUI, Verbose,
                    TEXT("ClearBrowsingData failed: 0x%08X — falling back."),
                    static_cast<uint32>(Hr));
            }
        }
    }

    // Fallback for older runtimes.
    ClearAllCookies();
    ExecuteJavaScript(TEXT("try{localStorage.clear();sessionStorage.clear();}catch(e){}"));
    UE_LOG(LogInoWebUI, Log, TEXT("ClearAllData via fallback (cookies + storage clear)."));
}

// ─────────────────────────────────────────────────────────────────────────────
//  CapturePreview — WebView2 calls back via IStream; we copy out and write
//  through UE FFileHelper. Best-effort — returns false on synchronous failure.
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Windows::CapturePreview(EInoImageFormat Format, const FString& OutFilePath)
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("CapturePreview: WebView not ready."));
        return false;
    }

    // SHCreateMemStream(nullptr, 0) gives an in-memory IStream we can read out
    // afterwards. WebView2 writes the image bytes into this stream.
    IStream* MemStream = SHCreateMemStream(nullptr, 0);
    if (!MemStream)
    {
        UE_LOG(LogInoWebUI, Warning, TEXT("CapturePreview: SHCreateMemStream failed."));
        return false;
    }

    const COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT Fmt =
        (Format == EInoImageFormat::JPEG)
        ? COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG
        : COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG;

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
    const FString OutPathCopy = OutFilePath;

    const HRESULT Hr = Internal->WebView->CapturePreview(
        Fmt,
        MemStream,
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, WeakLifetime, MemStream, OutPathCopy](HRESULT Result) -> HRESULT
            {
                // Always release the stream on completion, even if we no-op.
                if (!WeakLifetime.IsValid())
                {
                    if (MemStream) MemStream->Release();
                    return S_OK;
                }

                bool bSuccess = false;
                if (SUCCEEDED(Result) && MemStream)
                {
                    // Read all bytes from the stream — rewind first.
                    LARGE_INTEGER LZero{};
                    LZero.QuadPart = 0;
                    MemStream->Seek(LZero, STREAM_SEEK_SET, nullptr);

                    TArray<uint8> Bytes;
                    constexpr ULONG kChunk = 64 * 1024;
                    uint8 Tmp[kChunk];
                    while (true)
                    {
                        ULONG ReadCount = 0;
                        const HRESULT ReadHr = MemStream->Read(Tmp, kChunk, &ReadCount);
                        if (ReadCount > 0)
                        {
                            Bytes.Append(Tmp, ReadCount);
                        }
                        if (FAILED(ReadHr) || ReadCount < kChunk) break;
                    }

                    if (Bytes.Num() > 0)
                    {
                        bSuccess = FFileHelper::SaveArrayToFile(Bytes, *OutPathCopy);
                        if (!bSuccess)
                        {
                            UE_LOG(LogInoWebUI, Warning,
                                TEXT("CapturePreview: failed to write %d bytes to %s"),
                                Bytes.Num(), *OutPathCopy);
                        }
                    }
                }

                if (MemStream) MemStream->Release();

                // Fire completion callback (we're already on the game thread —
                // WebView2 invokes its completion handlers there).
                if (OnCapturePreviewCompleteCallback)
                {
                    OnCapturePreviewCompleteCallback(bSuccess, OutPathCopy);
                }
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("CapturePreview: WebView2 call failed: 0x%08X"),
            static_cast<uint32>(Hr));
        if (MemStream) MemStream->Release();
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LoadURLWithHeaders — only headers on the top-level navigation request.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::LoadURLWithHeaders(const FString& URL,
                                                  const TMap<FString, FString>& Headers)
{
    check(IsInGameThread());
    if (!bReady)
    {
        // Most-recent navigation intent wins; clear sibling queues.
        Internal->PendingHeaderedLoad = FInternal::FPendingHeadered{ URL, Headers };
        Internal->PendingNavigate.Reset();
        Internal->PendingHTMLLoad.Reset();
        return;
    }

    ComPtr<ICoreWebView2Environment2> Env2;
    if (!Internal->Environment || FAILED(Internal->Environment.As(&Env2)) || !Env2)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("LoadURLWithHeaders: ICoreWebView2Environment2 unavailable; "
                 "falling back to plain Navigate (headers will be dropped)."));
        Navigate(URL);
        return;
    }

    ComPtr<ICoreWebView2_2> WebView2;
    if (FAILED(Internal->WebView.As(&WebView2)) || !WebView2)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("LoadURLWithHeaders: ICoreWebView2_2 unavailable; falling back to Navigate."));
        Navigate(URL);
        return;
    }

    // Build CRLF-joined "Name: Value" header block.
    FString Joined;
    for (const TPair<FString, FString>& KV : Headers)
    {
        Joined.Appendf(TEXT("%s: %s\r\n"), *KV.Key, *KV.Value);
    }

    ComPtr<ICoreWebView2WebResourceRequest> Request;
    const HRESULT Hr = Env2->CreateWebResourceRequest(
        *URL, TEXT("GET"), /*PostData=*/ nullptr, *Joined, &Request);
    if (FAILED(Hr) || !Request)
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("LoadURLWithHeaders: CreateWebResourceRequest failed: 0x%08X"),
            static_cast<uint32>(Hr));
        Navigate(URL);
        return;
    }

    const HRESULT NavHr = WebView2->NavigateWithWebResourceRequest(Request.Get());
    if (FAILED(NavHr))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("NavigateWithWebResourceRequest failed: 0x%08X"),
            static_cast<uint32>(NavHr));
    }
}

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
