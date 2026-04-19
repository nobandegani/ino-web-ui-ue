// Copyright Inoksan. All Rights Reserved.

#include "InoWebView.h"
#include "InoWebUILog.h"
#include "Misc/Paths.h"

// IInoWebViewImpl is already pulled in via InoWebView.h — no extra include here.

UInoWebView::UInoWebView() = default;

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization (called by the subsystem only)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::Init(FName InName, TUniquePtr<IInoWebViewImpl>&& InImpl,
                       void* ParentNativeHandle, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    WebViewName = InName;
    Impl        = MoveTemp(InImpl);

    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: no platform implementation; operations will no-op."),
            *WebViewName.ToString());
        return;
    }

    const bool bOk = Impl->Initialize(ParentNativeHandle, Config);
    if (!bOk)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: native Initialize() failed."), *WebViewName.ToString());
        Impl.Reset();
        return;
    }

    UE_LOG(LogInoWebUI, Log, TEXT("UInoWebView[%s] created."), *WebViewName.ToString());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Navigation
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::LoadURL(const FString& URL)
{
    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]::LoadURL: no implementation."), *WebViewName.ToString());
        return;
    }
    Impl->Navigate(URL);
}

void UInoWebView::LoadLocalFile(const FString& RelativeContentPath)
{
    const FString Absolute = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectContentDir() / RelativeContentPath);

    // file:/// URIs use forward slashes.
    FString URI = FString::Printf(TEXT("file:///%s"), *Absolute);
    URI.ReplaceInline(TEXT("\\"), TEXT("/"));

    LoadURL(URI);
}

void UInoWebView::Reload()
{
    if (Impl.IsValid()) Impl->Reload();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Visibility
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::Show()
{
    if (Impl.IsValid()) Impl->SetVisible(true);
}

void UInoWebView::Hide()
{
    if (Impl.IsValid()) Impl->SetVisible(false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Queries
// ─────────────────────────────────────────────────────────────────────────────
bool UInoWebView::IsReady() const
{
    return Impl.IsValid() && Impl->IsReady();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem-driven lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::OnParentResized(int32 X, int32 Y, int32 Width, int32 Height)
{
    if (Impl.IsValid()) Impl->SyncBounds(X, Y, Width, Height);
}

void UInoWebView::ShutdownImpl()
{
    if (Impl.IsValid())
    {
        Impl->Shutdown();
        Impl.Reset();
        UE_LOG(LogInoWebUI, Log, TEXT("UInoWebView[%s] destroyed."), *WebViewName.ToString());
    }
}

void UInoWebView::BeginDestroy()
{
    // GC is tearing us down — make sure the native WebView is closed while
    // the parent HWND still exists (this mostly matters when the object
    // outlives the subsystem teardown, e.g. in editor reloads).
    ShutdownImpl();
    Super::BeginDestroy();
}
