// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "IInoWebViewImpl.h"

#if PLATFORM_WINDOWS

/**
 * FInoWebViewImpl_Windows_Composition — DirectComposition-hosted WebView2 impl.
 *
 * Purpose: fixes the PIE transparency problem on Windows. Slate sets flags on
 * the PIE editor window that confuse DWM's child-HWND compositing, causing
 * transparent WebView pixels to blend against the desktop instead of against
 * UE's swap chain. The child-HWND controller (FInoWebViewImpl_Windows) hits
 * this limitation.
 *
 * This impl takes a different approach:
 *   • Creates a top-level layered overlay HWND (WS_EX_NOREDIRECTIONBITMAP),
 *     not a child of UE's window.
 *   • Creates its own DirectComposition device / target / visual tree on
 *     that overlay.
 *   • Uses CreateCoreWebView2CompositionController instead of the normal
 *     Controller. The composition controller exposes a visual (via
 *     put_RootVisualTarget) that we stick into our DComp tree.
 *   • Tracks UE's window position/size/visibility so the overlay follows it.
 *   • Forwards mouse/pointer input from the overlay HWND's WndProc to the
 *     composition controller via SendMouseInput / SendPointerInput.
 *
 * The factory (InoWebViewFactory.cpp) dispatches to this impl only when
 * GIsEditor is true (→ running inside a PIE session). Standalone Game and
 * packaged builds continue to use the simpler, battle-tested
 * FInoWebViewImpl_Windows child-HWND path.
 *
 * All methods run on the game thread, same as the sibling impl.
 *
 * ──────────────────────────────────────────────────────────────────────
 * Header hygiene rule (same as the sibling impl): NEVER expose Win32 or
 * WebView2 types in this header. Any helper that takes HWND / LRESULT /
 * WPARAM / LPARAM / COM interfaces lives on FInternal inside the .cpp.
 * ──────────────────────────────────────────────────────────────────────
 */
class FInoWebViewImpl_Windows_Composition : public IInoWebViewImpl
{
public:
    FInoWebViewImpl_Windows_Composition();
    virtual ~FInoWebViewImpl_Windows_Composition();

    //~ IInoWebViewImpl
    virtual bool Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config) override;
    virtual bool IsReady()  const override { return bReady; }
    virtual void Navigate(const FString& URL) override;
    virtual void Reload() override;
    virtual void SetVisible(bool bVisible) override;
    virtual void SyncBounds(int32 X, int32 Y, int32 Width, int32 Height) override;
    virtual void Shutdown() override;
    virtual void PostMessageJson(const FString& Json) override;
    virtual void OpenDevTools() override;
    virtual void ExecuteJavaScript(const FString& Code) override;
    virtual void SetMuted(bool bMuted) override;
    virtual void FocusWebView() override;
    virtual void SetZoomFactor(float Factor) override;
    virtual float GetZoomFactor() const override;
    virtual void ClearAllCookies() override;
    virtual void SetBackgroundOpaque(bool bOpaque) override;

    // ── New ops ────────────────────────────────────────────────────────────
    virtual void GoBack() override;
    virtual void GoForward() override;
    virtual bool CanGoBack() const override;
    virtual bool CanGoForward() const override;
    virtual void StopLoading() override;
    virtual void LoadHTMLString(const FString& HTML, const FString& BaseURI) override;
    virtual void SetCookie(const FString& URL, const FString& Cookie) override;
    virtual void ClearAllData() override;
    virtual bool CapturePreview(EInoImageFormat Format, const FString& OutFilePath) override;
    virtual void LoadURLWithHeaders(const FString& URL, const TMap<FString, FString>& Headers) override;

    /**
     * Opaque state — full definition lives in the .cpp so Windows.h and
     * WebView2.h stay out of this header. Declared public ONLY so the
     * file-scope WndProc trampoline in the .cpp can name the type; all
     * state is still effectively private (only Internal has a valid
     * pointer, Internal is private).
     */
    struct FInternal;

private:
    TUniquePtr<FInternal> Internal;

    bool bReady = false;
    bool bInitStarted = false;

    // Async-completion callbacks, invoked from WRL lambdas. Plain void*
    // parameter types — the .cpp reinterprets to the correct COM type.
    void OnEnvironmentReady(int32 HResult, void* EnvironmentPtr);
    void OnCompositionControllerReady(int32 HResult, void* CompositionControllerPtr);
    void ApplyPendingOperations();
};

#endif // PLATFORM_WINDOWS
