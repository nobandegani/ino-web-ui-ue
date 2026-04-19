// Copyright Inoksan. All Rights Reserved.

#include "InoWebUISubsystem.h"

#include "InoWebUILog.h"
#include "InoWebView.h"
#include "Impl/IInoWebViewImpl.h"

#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"            // FViewport
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebUISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Global event — fires on any viewport resize, including fullscreen
    // toggle, DPI change, and standard window drag-resize. We filter to our
    // own viewport inside the handler.
    ViewportResizedHandle = FViewport::ViewportResizedEvent.AddUObject(
        this, &UInoWebUISubsystem::OnViewportResized);

    UE_LOG(LogInoWebUI, Log, TEXT("UInoWebUISubsystem initialized."));
}

void UInoWebUISubsystem::Deinitialize()
{
    UE_LOG(LogInoWebUI, Log,
        TEXT("UInoWebUISubsystem deinitializing (%d WebView(s) live)."), WebViews.Num());

    if (ViewportResizedHandle.IsValid())
    {
        FViewport::ViewportResizedEvent.Remove(ViewportResizedHandle);
        ViewportResizedHandle.Reset();
    }

    DestroyAllWebViews();

    Super::Deinitialize();
}

// ─────────────────────────────────────────────────────────────────────────────
//  CreateWebView
// ─────────────────────────────────────────────────────────────────────────────
UInoWebView* UInoWebUISubsystem::CreateWebView(FName Name, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    if (Name.IsNone())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("CreateWebView: Name is None; refusing to create."));
        return nullptr;
    }

    // Idempotent on duplicate name — warn and return the existing one.
    if (const TObjectPtr<UInoWebView>* Existing = WebViews.Find(Name))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("CreateWebView('%s'): a WebView with this name already exists; "
                 "returning the existing instance."),
            *Name.ToString());
        return Existing->Get();
    }

    // Resolve the OS-level parent window handle.
    void* NativeHandle = AcquireParentNativeHandle();
    if (!NativeHandle)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWebView('%s'): no game viewport window available yet. "
                 "Call this from BeginPlay or later, not from PreInit."),
            *Name.ToString());
        return nullptr;
    }

    // Build the platform implementation. Null on unsupported platforms.
    TUniquePtr<IInoWebViewImpl> Impl = CreateInoWebViewImpl();
    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWebView('%s'): no native WebView implementation on this platform."),
            *Name.ToString());
        return nullptr;
    }

    // Spawn the UObject and hand it the impl.
    UInoWebView* View = NewObject<UInoWebView>(this);
    View->Init(Name, MoveTemp(Impl), NativeHandle, Config);

    WebViews.Add(Name, View);
    UE_LOG(LogInoWebUI, Log, TEXT("CreateWebView('%s') succeeded."), *Name.ToString());

    // The impl seeds its own initial bounds from GetClientRect during async
    // construction, but we push an extra broadcast here to cover the case
    // where the window resizes while the WebView is still initializing —
    // SyncBounds pre-ready is queued and replayed when the controller arrives.
    BroadcastClientRectToAll();

    return View;
}

// ─────────────────────────────────────────────────────────────────────────────
//  GetWebView / DestroyWebView / DestroyAllWebViews
// ─────────────────────────────────────────────────────────────────────────────
UInoWebView* UInoWebUISubsystem::GetWebView(FName Name) const
{
    if (const TObjectPtr<UInoWebView>* Found = WebViews.Find(Name))
    {
        return Found->Get();
    }
    return nullptr;
}

void UInoWebUISubsystem::DestroyWebView(FName Name)
{
    check(IsInGameThread());

    TObjectPtr<UInoWebView> View;
    if (!WebViews.RemoveAndCopyValue(Name, View))
    {
        UE_LOG(LogInoWebUI, Verbose,
            TEXT("DestroyWebView('%s'): no such WebView."), *Name.ToString());
        return;
    }

    if (View)
    {
        View->ShutdownImpl();
        // UObject will be collected by GC once all references are gone.
    }
}

void UInoWebUISubsystem::DestroyAllWebViews()
{
    check(IsInGameThread());

    // Move the map aside so ShutdownImpl can't mutate what we iterate.
    TMap<FName, TObjectPtr<UInoWebView>> Snapshot = MoveTemp(WebViews);
    for (auto& Pair : Snapshot)
    {
        if (UInoWebView* View = Pair.Value)
        {
            View->ShutdownImpl();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  AcquireParentNativeHandle
//
//  GameInstance → World → GameViewport → SWindow → GenericWindow → OS handle
//
//  Works for both PIE (child editor window) and standalone (main game window)
//  because we go through this GameInstance's own viewport.
// ─────────────────────────────────────────────────────────────────────────────
void* UInoWebUISubsystem::AcquireParentNativeHandle()
{
    UGameInstance* GI = GetGameInstance();
    if (!GI)
    {
        return nullptr;
    }

    UWorld* World = GI->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameViewportClient* ViewportClient = World->GetGameViewport();
    if (!ViewportClient)
    {
        return nullptr;
    }

    const TSharedPtr<SWindow> Window = ViewportClient->GetWindow();
    if (!Window.IsValid())
    {
        return nullptr;
    }

    const TSharedPtr<FGenericWindow> NativeWindow = Window->GetNativeWindow();
    if (!NativeWindow.IsValid())
    {
        return nullptr;
    }

    return NativeWindow->GetOSWindowHandle();
}

TSharedPtr<SWindow> UInoWebUISubsystem::GetParentWindow()
{
    UGameInstance* GI = GetGameInstance();
    if (!GI) return nullptr;

    UWorld* World = GI->GetWorld();
    if (!World) return nullptr;

    UGameViewportClient* VC = World->GetGameViewport();
    if (!VC) return nullptr;

    return VC->GetWindow();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Resize pipeline
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebUISubsystem::OnViewportResized(FViewport* InViewport, uint32 /*Unused*/)
{
    // Filter: only react to resizes of OUR GameInstance's viewport. Other
    // viewports (a second PIE window, an editor preview viewport, the
    // thumbnail renderer...) pass through this same global event.
    const UGameInstance* GI = GetGameInstance();
    if (!GI) return;

    const UWorld* World = GI->GetWorld();
    if (!World) return;

    const UGameViewportClient* VC = World->GetGameViewport();
    if (!VC || VC->Viewport != InViewport) return;

    BroadcastClientRectToAll();
}

void UInoWebUISubsystem::BroadcastClientRectToAll()
{
    if (WebViews.Num() == 0) return;

    TSharedPtr<SWindow> Window = GetParentWindow();
    if (!Window.IsValid()) return;

    // GetClientSizeInScreen returns physical pixels on Windows, which is
    // what WebView2's put_Bounds expects — no DPI conversion needed.
    const FVector2D  ClientSize = Window->GetClientSizeInScreen();
    const int32      Width  = FMath::Max(0, FMath::CeilToInt(ClientSize.X));
    const int32      Height = FMath::Max(0, FMath::CeilToInt(ClientSize.Y));

    // Origin is always (0,0) because put_Bounds is in parent-client coords.
    for (const auto& Pair : WebViews)
    {
        if (UInoWebView* View = Pair.Value)
        {
            View->OnParentResized(0, 0, Width, Height);
        }
    }
}
