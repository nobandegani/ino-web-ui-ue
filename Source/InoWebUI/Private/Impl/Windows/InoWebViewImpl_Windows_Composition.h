// Copyright Inoland. All Rights Reserved.

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

private:
    /** Opaque state — defined in the .cpp so WebView2.h / dcomp.h stay out of headers. */
    struct FInternal;
    TUniquePtr<FInternal> Internal;

    bool bReady = false;
    bool bInitStarted = false;

    // Async-completion callbacks, invoked from WRL lambdas.
    void OnEnvironmentReady(int32 HResult, void* EnvironmentPtr);
    void OnCompositionControllerReady(int32 HResult, void* CompositionControllerPtr);
    void ApplyPendingOperations();

    // Composition-specific helpers.
    void CreateOverlayWindow();
    void DestroyOverlayWindow();
    void CreateDCompStack();
    void DestroyDCompStack();
    void UpdateOverlayToScreenRect(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height);

    // Overlay HWND WndProc — forwards mouse/pointer to the composition
    // controller and handles our own tracking/timer messages.
    static LRESULT CALLBACK OverlayWndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ForwardMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // PLATFORM_WINDOWS
