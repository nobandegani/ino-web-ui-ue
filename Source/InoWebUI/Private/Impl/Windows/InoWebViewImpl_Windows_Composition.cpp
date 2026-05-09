// Copyright Inoland. All Rights Reserved.

#include "InoWebViewImpl_Windows_Composition.h"

#if PLATFORM_WINDOWS

#include "InoWebUILog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
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
#include <shlwapi.h>            // SHCreateMemStream
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

// Injected JS — bridge + dev-tools overlay (generated from Source/InoWebUI/JS/*.js)
// Re-run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1 if you edit the .js files.
#include "Generated/InoWebUIScripts.generated.h"

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

    // ── New pending operations (parity with sibling impl) ──────────────────
    struct FPendingHTML { FString HTML; FString BaseURI; };
    TOptional<FPendingHTML> PendingHTMLLoad;

    struct FPendingHeadered { FString URL; TMap<FString, FString> Headers; };
    TOptional<FPendingHeadered> PendingHeaderedLoad;

    struct FPendingCookie { FString URL; FString Cookie; };
    TArray<FPendingCookie> PendingCookies;

    bool bPendingClearAllData = false;

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

    // Seed cookies before navigation so InitialCookies are in the store
    // before the initial request. ApplyPendingOperations replays cookies
    // before any navigation queue.
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
        CachedURL = Config.InitialURL;
    }
    Internal->PendingVisible = Config.View.bVisibleOnCreate;
    Internal->bUserVisible   = Config.View.bVisibleOnCreate;

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
        GInoWebUIBridgeScript, nullptr);

    if (Internal->Config.View.bEnableDevTools)
    {
        Internal->WebView->AddScriptToExecuteOnDocumentCreated(
            GInoWebUIDevToolsOverlayScript, nullptr);
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
                    else
                    {
                        bCachedLoading = true;
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
                    CachedURL      = URI;
                    bCachedLoading = false;
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
                    CachedTitle = Title;
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
    if (Internal->Config.View.bTransparentBackground)
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
            Settings->put_AreDefaultContextMenusEnabled(Internal->Config.View.bEnableContextMenus ? 1 : 0);
            Settings->put_AreDevToolsEnabled          (Internal->Config.View.bEnableDevTools      ? 1 : 0);
            Settings->put_IsStatusBarEnabled          (Internal->Config.View.bShowStatusBar       ? 1 : 0);

            ComPtr<ICoreWebView2Settings3> Settings3;
            if (SUCCEEDED(Settings.As(&Settings3)))
            {
                Settings3->put_AreBrowserAcceleratorKeysEnabled(
                    Internal->Config.View.bEnableAcceleratorKeys ? 1 : 0);
            }

            // Settings5 — pinch zoom. Runtime 91+. Silent no-op on older
            // runtimes.
            ComPtr<ICoreWebView2Settings5> Settings5;
            if (SUCCEEDED(Settings.As(&Settings5)))
            {
                Settings5->put_IsPinchZoomEnabled(
                    Internal->Config.View.bAllowZoom ? 1 : 0);
            }

            if (!Internal->Config.View.UserAgentOverride.IsEmpty())
            {
                ComPtr<ICoreWebView2Settings2> Settings2;
                if (SUCCEEDED(Settings.As(&Settings2)))
                {
                    Settings2->put_UserAgent(*Internal->Config.View.UserAgentOverride);
                }
            }
        }

        if (Internal->Config.View.bStartMuted)
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

    // Cookies replay FIRST — same rationale as the sibling impl. Initial
    // cookies need to be in the cookie store before any navigation goes
    // out so the first request carries them.
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

    if (Internal->PendingHTMLLoad.IsSet())
    {
        const auto Snap = Internal->PendingHTMLLoad.GetValue();
        Internal->PendingHTMLLoad.Reset();
        LoadHTMLString(Snap.HTML, Snap.BaseURI);
    }
    if (Internal->PendingHeaderedLoad.IsSet())
    {
        const auto Snap = Internal->PendingHeaderedLoad.GetValue();
        Internal->PendingHeaderedLoad.Reset();
        LoadURLWithHeaders(Snap.URL, Snap.Headers);
    }
    // PendingCookies were replayed at the top of this function.
    if (Internal->bPendingClearAllData)
    {
        Internal->bPendingClearAllData = false;
        ClearAllData();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public operations (thin wrappers; helpers live on FInternal)
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::Navigate(const FString& URL)
{
    check(IsInGameThread());
    if (!bReady)
    {
        // Most-recent navigation intent wins.
        Internal->PendingNavigate = URL;
        Internal->PendingHeaderedLoad.Reset();
        Internal->PendingHTMLLoad.Reset();
        return;
    }
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

    if (!Internal->Config.View.bEnableDevTools)
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
//  Browser-style nav (composition impl)
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Windows_Composition::GoBack()
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->GoBack();
}

void FInoWebViewImpl_Windows_Composition::GoForward()
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->GoForward();
}

bool FInoWebViewImpl_Windows_Composition::CanGoBack() const
{
    if (!bReady || !Internal || !Internal->WebView) return false;
    BOOL b = 0;
    if (FAILED(Internal->WebView->get_CanGoBack(&b))) return false;
    return b != 0;
}

bool FInoWebViewImpl_Windows_Composition::CanGoForward() const
{
    if (!bReady || !Internal || !Internal->WebView) return false;
    BOOL b = 0;
    if (FAILED(Internal->WebView->get_CanGoForward(&b))) return false;
    return b != 0;
}

void FInoWebViewImpl_Windows_Composition::StopLoading()
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return;
    Internal->WebView->Stop();
}

void FInoWebViewImpl_Windows_Composition::LoadHTMLString(const FString& HTML, const FString& BaseURI)
{
    check(IsInGameThread());
    if (!bReady)
    {
        // Most-recent navigation intent wins.
        Internal->PendingHTMLLoad = FInternal::FPendingHTML{ HTML, BaseURI };
        Internal->PendingNavigate.Reset();
        Internal->PendingHeaderedLoad.Reset();
        return;
    }
    if (!BaseURI.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Verbose,
            TEXT("LoadHTMLString (composition): BaseURI ('%s') ignored — "
                 "WebView2 NavigateToString uses about:blank as origin."),
            *BaseURI);
    }
    Internal->WebView->NavigateToString(*HTML);
}

// Cookie parsing helper (unique-named namespace because of unity builds —
// matches the IsURIAllowed helper above). ICoreWebView2Cookie's Path/Domain
// are read-only post-construction; we parse the full string first, then call
// CreateCookie with name/value/domain/path baked in.
namespace InoWebUICompositionPriv
{
    bool ParseHttpDateToUnixSecondsC(const FString& Date, double& OutSeconds)
    {
        FDateTime DT;
        if (FDateTime::ParseHttpDate(Date, DT))
        {
            OutSeconds = static_cast<double>(DT.ToUnixTimestamp());
            return true;
        }
        return false;
    }

    struct FParsedCookieC
    {
        FString  Name;
        FString  Value;
        FString  Path = TEXT("/");
        FString  Domain;
        TOptional<double> Expires;
        bool     bSecure   = false;
        bool     bHttpOnly = false;
        TOptional<COREWEBVIEW2_COOKIE_SAME_SITE_KIND> SameSite;
        bool     bValid = false;
    };

    FParsedCookieC ParseHttpCookieC(const FString& Raw)
    {
        FParsedCookieC Out;
        TArray<FString> Parts;
        Raw.ParseIntoArray(Parts, TEXT(";"), true);
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
                if (ParseHttpDateToUnixSecondsC(V, Sec)) Out.Expires = Sec;
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
        }
        return Out;
    }
}

void FInoWebViewImpl_Windows_Composition::SetCookie(const FString& URL, const FString& Cookie)
{
    check(IsInGameThread());
    if (!bReady)
    {
        Internal->PendingCookies.Add(FInternal::FPendingCookie{ URL, Cookie });
        return;
    }

    ComPtr<ICoreWebView2_2> WebView2;
    if (FAILED(Internal->WebView.As(&WebView2)) || !WebView2) return;

    ComPtr<ICoreWebView2CookieManager> CookieMgr;
    if (FAILED(WebView2->get_CookieManager(&CookieMgr)) || !CookieMgr) return;

    InoWebUICompositionPriv::FParsedCookieC P =
        InoWebUICompositionPriv::ParseHttpCookieC(Cookie);
    if (!P.bValid) return;

    FString Domain = P.Domain;
    if (Domain.IsEmpty())
    {
        Domain = InoWebUICompositionPriv::ExtractHost(URL);
    }

    ComPtr<ICoreWebView2Cookie> NewCookie;
    if (FAILED(CookieMgr->CreateCookie(*P.Name, *P.Value, *Domain, *P.Path, &NewCookie))
        || !NewCookie)
    {
        return;
    }

    if (P.Expires.IsSet())  NewCookie->put_Expires(P.Expires.GetValue());
    if (P.bHttpOnly)        NewCookie->put_IsHttpOnly(1);
    if (P.bSecure)          NewCookie->put_IsSecure(1);
    if (P.SameSite.IsSet()) NewCookie->put_SameSite(P.SameSite.GetValue());

    CookieMgr->AddOrUpdateCookie(NewCookie.Get());
}

void FInoWebViewImpl_Windows_Composition::ClearAllData()
{
    check(IsInGameThread());
    if (!bReady)
    {
        Internal->bPendingClearAllData = true;
        return;
    }

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
                        [WeakLifetime](HRESULT) -> HRESULT { (void)WeakLifetime; return S_OK; }).Get());
                if (SUCCEEDED(Hr)) return;
            }
        }
    }

    ClearAllCookies();
    ExecuteJavaScript(TEXT("try{localStorage.clear();sessionStorage.clear();}catch(e){}"));
}

bool FInoWebViewImpl_Windows_Composition::CapturePreview(EInoImageFormat Format,
                                                         const FString& OutFilePath)
{
    check(IsInGameThread());
    if (!bReady || !Internal->WebView) return false;

    IStream* MemStream = SHCreateMemStream(nullptr, 0);
    if (!MemStream) return false;

    const COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT Fmt =
        (Format == EInoImageFormat::JPEG)
        ? COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG
        : COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG;

    TWeakPtr<int> WeakLifetime = Internal->LifetimeToken;
    const FString OutPathCopy = OutFilePath;

    const HRESULT Hr = Internal->WebView->CapturePreview(
        Fmt, MemStream,
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, WeakLifetime, MemStream, OutPathCopy](HRESULT Result) -> HRESULT
            {
                if (!WeakLifetime.IsValid())
                {
                    if (MemStream) MemStream->Release();
                    return S_OK;
                }

                bool bSuccess = false;
                if (SUCCEEDED(Result) && MemStream)
                {
                    LARGE_INTEGER LZero{}; LZero.QuadPart = 0;
                    MemStream->Seek(LZero, STREAM_SEEK_SET, nullptr);

                    TArray<uint8> Bytes;
                    constexpr ULONG kChunk = 64 * 1024;
                    uint8 Tmp[kChunk];
                    while (true)
                    {
                        ULONG Got = 0;
                        const HRESULT R = MemStream->Read(Tmp, kChunk, &Got);
                        if (Got > 0) Bytes.Append(Tmp, Got);
                        if (FAILED(R) || Got < kChunk) break;
                    }

                    if (Bytes.Num() > 0)
                    {
                        bSuccess = FFileHelper::SaveArrayToFile(Bytes, *OutPathCopy);
                    }
                }

                if (MemStream) MemStream->Release();

                if (OnCapturePreviewCompleteCallback)
                {
                    OnCapturePreviewCompleteCallback(bSuccess, OutPathCopy);
                }
                return S_OK;
            }).Get());

    if (FAILED(Hr))
    {
        if (MemStream) MemStream->Release();
        return false;
    }
    return true;
}

void FInoWebViewImpl_Windows_Composition::LoadURLWithHeaders(const FString& URL,
                                                              const TMap<FString, FString>& Headers)
{
    check(IsInGameThread());
    if (!bReady)
    {
        // Most-recent navigation intent wins.
        Internal->PendingHeaderedLoad = FInternal::FPendingHeadered{ URL, Headers };
        Internal->PendingNavigate.Reset();
        Internal->PendingHTMLLoad.Reset();
        return;
    }

    ComPtr<ICoreWebView2Environment2> Env2;
    ComPtr<ICoreWebView2_2> WebView2;
    if (!Internal->Environment
        || FAILED(Internal->Environment.As(&Env2)) || !Env2
        || FAILED(Internal->WebView.As(&WebView2)) || !WebView2)
    {
        Navigate(URL);
        return;
    }

    FString Joined;
    for (const TPair<FString, FString>& KV : Headers)
    {
        Joined.Appendf(TEXT("%s: %s\r\n"), *KV.Key, *KV.Value);
    }

    ComPtr<ICoreWebView2WebResourceRequest> Request;
    if (FAILED(Env2->CreateWebResourceRequest(*URL, TEXT("GET"), nullptr, *Joined, &Request))
        || !Request)
    {
        Navigate(URL);
        return;
    }
    WebView2->NavigateWithWebResourceRequest(Request.Get());
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
