// Copyright Inoland. All Rights Reserved.

#include "InoWebUISubsystem.h"

#include "InoWebUILog.h"
#include "InoWebView.h"
#include "InoWebBundle.h"
#include "IInoWebViewImpl.h"    // now a public header (see commit 16140a0)
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
#if !PLATFORM_ANDROID
    // Android doesn't need a handle — the Java helper locates the current
    // GameActivity itself. On every other platform, failing to resolve a
    // parent window is a fatal precondition for CreateWebView.
    if (!NativeHandle)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWebView('%s'): no game viewport window available yet. "
                 "Call this from BeginPlay or later, not from PreInit."),
            *Name.ToString());
        return nullptr;
    }
#endif

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

    // GetClientRectInScreen respects whatever chrome Slate draws on top of
    // the HWND — crucial for PIE "New Editor Window" mode, where the HWND's
    // own client area includes the Slate-drawn title bar and we must NOT
    // overlap it. For a standalone OS-chromed game window this just returns
    // the OS client area as expected.
    const FSlateRect ClientRect = Window->GetClientRectInScreen();

    // CeilToInt32 (explicit int32) — plain CeilToInt overloads to int64 for
    // double input, which would narrow when assigned to int32.
    const int32 ScreenX = FMath::FloorToInt32(ClientRect.Left);
    const int32 ScreenY = FMath::FloorToInt32(ClientRect.Top);
    const int32 Width   = FMath::Max(0, FMath::CeilToInt32(ClientRect.Right  - ClientRect.Left));
    const int32 Height  = FMath::Max(0, FMath::CeilToInt32(ClientRect.Bottom - ClientRect.Top));

    // Screen-space coords — the impl converts to parent-HWND coords.
    for (const auto& Pair : WebViews)
    {
        if (UInoWebView* View = Pair.Value)
        {
            // Manual-bounds WebViews opt out of auto-resize. Their rect is
            // owned by the caller via SetBounds; SetBoundsAuto re-engages
            // them.
            if (View->IsManualBounds()) continue;
            View->OnParentResized(ScreenX, ScreenY, Width, Height);
        }
    }
}

void UInoWebUISubsystem::BroadcastClientRectToOne(UInoWebView* View)
{
    check(IsInGameThread());
    if (!View) return;

    TSharedPtr<SWindow> Window = GetParentWindow();
    if (!Window.IsValid()) return;

    const FSlateRect ClientRect = Window->GetClientRectInScreen();
    const int32 ScreenX = FMath::FloorToInt32(ClientRect.Left);
    const int32 ScreenY = FMath::FloorToInt32(ClientRect.Top);
    const int32 Width   = FMath::Max(0, FMath::CeilToInt32(ClientRect.Right  - ClientRect.Left));
    const int32 Height  = FMath::Max(0, FMath::CeilToInt32(ClientRect.Bottom - ClientRect.Top));

    View->OnParentResized(ScreenX, ScreenY, Width, Height);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CreateWebViewFromAsset — bundle-driven creation.
//
//  Two paths:
//
//    Dev path (editor / uncooked AND Bundle->DevInitialURL is non-empty):
//      Point the WebView straight at the dev URL (Vite, webpack-dev-server,
//      etc.). Skip the virtual-host setup entirely — the dev server owns its
//      own origin. Disable lockdown so the dev URL isn't blocked.
//
//    Packaged / default path:
//      Use Bundle->Config.InitialURL with a virtual-host mapping onto the
//      bundle's content folder (SourceFolder in editor, extracted Files[]
//      in cooked).
// ─────────────────────────────────────────────────────────────────────────────
UInoWebView* UInoWebUISubsystem::CreateWebViewFromAsset(FName Name, UInoWebBundle* Bundle)
{
    check(IsInGameThread());

    if (!Bundle)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWebViewFromAsset('%s'): Bundle is null."), *Name.ToString());
        return nullptr;
    }

    FInoWebViewConfig Config = Bundle->Config;

#if WITH_EDITOR
    // Dev-server shortcut: if the user set DevInitialURL, prefer it over the
    // packaged flow. Virtual host becomes irrelevant (dev server serves its
    // own URLs) and lockdown would otherwise block the dev URL since it
    // doesn't match VirtualHostName.
    if (!Bundle->DevInitialURL.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Log,
            TEXT("CreateWebViewFromAsset('%s'): editor build — using DevInitialURL '%s' "
                 "(virtual host + lockdown disabled for dev)."),
            *Name.ToString(), *Bundle->DevInitialURL);

        Config.InitialURL        = Bundle->DevInitialURL;
        Config.VirtualHostName   = FString();
        Config.VirtualHostFolder = FString();
        Config.bLockToVirtualHost = false;

        return CreateWebView(Name, Config);
    }
#endif

    // Packaged path (or editor with no DevInitialURL): resolve the content
    // folder and wire up virtual-host mapping.
    const FString Folder = ResolveBundleContentFolder(Bundle);
    if (Folder.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("CreateWebViewFromAsset('%s'): could not resolve a content folder for bundle '%s'."),
            *Name.ToString(), *Bundle->GetName());
        return nullptr;
    }

    // VirtualHostFolder on the asset is always overridden: the bundle owns
    // that — via SourceFolder in editor or extracted Files[] in packaged.
    Config.VirtualHostFolder = Folder;

    return CreateWebView(Name, Config);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ResolveBundleContentFolder
//
//  Editor / non-cooked: prefer the bundle's SourceFolder if it exists on disk.
//                       Lets dev iterate on React with loose files.
//  Packaged / cooked:   extract baked Files[] to ProjectSavedDir. Uses a
//                       content-hash sidecar so extraction is only re-done
//                       when the asset's bytes actually change.
// ─────────────────────────────────────────────────────────────────────────────
FString UInoWebUISubsystem::ResolveBundleContentFolder(UInoWebBundle* Bundle)
{
#if WITH_EDITOR
    {
        const FString SourceFolder = Bundle->GetAbsoluteSourceFolder();
        if (!SourceFolder.IsEmpty() && IFileManager::Get().DirectoryExists(*SourceFolder))
        {
            UE_LOG(LogInoWebUI, Log,
                TEXT("Bundle '%s': serving from SourceFolder  %s"),
                *Bundle->GetName(), *SourceFolder);
            return SourceFolder;
        }
        UE_LOG(LogInoWebUI, Verbose,
            TEXT("Bundle '%s': SourceFolder unavailable, falling through to extraction."),
            *Bundle->GetName());
    }
#endif

    // Packaged path (or editor fallback).
    if (Bundle->Files.Num() == 0)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("Bundle '%s': no baked Files — did you Reimport before packaging?"),
            *Bundle->GetName());
        return FString();
    }

    // CRUCIAL: get an absolute OS-level path that platform-native code
    // (WebView2 on Windows, Java new File(...) on Android) can open directly.
    //
    // FPaths::ConvertRelativePathToFull on Android does NOT fully resolve
    // UE's "../../../ProjectName/..." relative form — UE's BaseDir concept
    // on Android isn't a real filesystem CWD, and Android's JNI doesn't
    // reinterpret those '..' segments the way UE's IFileManager does.
    //
    // ConvertToAbsolutePathForExternalAppForRead is the UE-blessed API for
    // exactly this purpose: get a path external apps can use. On Windows
    // it's equivalent to ConvertRelativePathToFull; on Android it resolves
    // to the real on-disk location under the app's external-files dir.
    const FString ExtractDir = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(
        *(FPaths::ProjectSavedDir() / TEXT("InoWebBundles") / Bundle->GetName()));
    const FString HashFile   = ExtractDir / TEXT(".inowebbundle.hash");

    // Fast path: we've already extracted this exact content hash — reuse it.
    FString OnDiskHash;
    if (FFileHelper::LoadFileToString(OnDiskHash, *HashFile))
    {
        OnDiskHash.TrimStartAndEndInline();
        if (OnDiskHash == Bundle->ContentHash && !Bundle->ContentHash.IsEmpty())
        {
            UE_LOG(LogInoWebUI, Verbose,
                TEXT("Bundle '%s': extraction at %s is up-to-date (hash=%s)."),
                *Bundle->GetName(), *ExtractDir, *OnDiskHash);
            return ExtractDir;
        }
    }

    // Stale or first extraction — write fresh.
    if (!Bundle->ExtractToDirectory(ExtractDir))
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("Bundle '%s': extraction to %s failed."),
            *Bundle->GetName(), *ExtractDir);
        return FString();
    }

    // Write the hash sidecar so future sessions can skip re-extraction.
    if (!FFileHelper::SaveStringToFile(Bundle->ContentHash, *HashFile))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("Bundle '%s': could not write hash sidecar — will re-extract next run."),
            *Bundle->GetName());
    }

    UE_LOG(LogInoWebUI, Log,
        TEXT("Bundle '%s': extracted %d file(s) to %s"),
        *Bundle->GetName(), Bundle->Files.Num(), *ExtractDir);
    return ExtractDir;
}
