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
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 async initialization started."));
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

    // Start filling the parent's client area; the subsystem will push updates on resize.
    RECT ClientRect;
    ::GetClientRect(Internal->ParentHwnd, &ClientRect);
    Internal->Controller->put_Bounds(ClientRect);

    bReady = true;
    UE_LOG(LogInoWebUI, Log, TEXT("WebView2 controller ready (%dx%d)."),
        ClientRect.right - ClientRect.left, ClientRect.bottom - ClientRect.top);

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

    Internal->Controller->put_IsVisible(bVisible ? TRUE : FALSE);
}

void FInoWebViewImpl_Windows::SyncBounds(int32 X, int32 Y, int32 Width, int32 Height)
{
    check(IsInGameThread());

    if (!bReady)
    {
        Internal->PendingBounds = FInternal::FRect{ X, Y, Width, Height };
        return;
    }

    RECT Bounds;
    Bounds.left   = X;
    Bounds.top    = Y;
    Bounds.right  = X + Width;
    Bounds.bottom = Y + Height;
    Internal->Controller->put_Bounds(Bounds);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown — MUST be called before the parent HWND dies.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows::Shutdown()
{
    check(IsInGameThread());
    if (!Internal) return;

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
