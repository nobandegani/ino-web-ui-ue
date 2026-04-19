// Copyright Inoksan. All Rights Reserved.

#include "InoWebUISubsystem.h"

#include "InoWebUILog.h"
#include "InoWebView.h"
#include "Impl/IInoWebViewImpl.h"

#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebUISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogInoWebUI, Log, TEXT("UInoWebUISubsystem initialized."));
}

void UInoWebUISubsystem::Deinitialize()
{
    UE_LOG(LogInoWebUI, Log,
        TEXT("UInoWebUISubsystem deinitializing (%d WebView(s) live)."), WebViews.Num());

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
void* UInoWebUISubsystem::AcquireParentNativeHandle() const
{
    const UGameInstance* GI = GetGameInstance();
    if (!GI)
    {
        return nullptr;
    }

    const UWorld* World = GI->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    const UGameViewportClient* ViewportClient = World->GetGameViewport();
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
