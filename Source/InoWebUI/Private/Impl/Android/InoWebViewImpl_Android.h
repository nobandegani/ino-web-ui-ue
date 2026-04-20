// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IInoWebViewImpl.h"

#if PLATFORM_ANDROID

/**
 * FInoWebViewImpl_Android — MVP implementation of IInoWebViewImpl backed by
 * android.webkit.WebView via the Java helper at
 *   com.inoksan.webui.InoWebViewAndroid
 *
 * All public methods run on the game thread; the JNI calls they make into
 * Java dispatch their real work onto the Android UI thread via
 * Activity.runOnUiThread (see the Java helper).
 *
 * MVP scope (Phase 6):
 *   • Create / Destroy
 *   • LoadURL / Reload
 *   • Show / Hide
 *   • SyncBounds
 *
 * Everything else on IInoWebViewImpl (messaging, dev tools, focus, zoom,
 * cookies, etc.) is present as a no-op override with a diagnostic log —
 * filled in as needed in later phases.
 */
class FInoWebViewImpl_Android : public IInoWebViewImpl
{
public:
    FInoWebViewImpl_Android();
    virtual ~FInoWebViewImpl_Android();

    //~ IInoWebViewImpl — implemented
    virtual bool Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config) override;
    virtual bool IsReady() const override { return bReady; }
    virtual void Navigate(const FString& URL) override;
    virtual void Reload() override;
    virtual void SetVisible(bool bVisible) override;
    virtual void SyncBounds(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height) override;
    virtual void Shutdown() override;

    //~ IInoWebViewImpl — MVP stubs (log-and-return)
    virtual void  PostMessageJson(const FString& Json) override;
    virtual void  OpenDevTools() override;
    virtual void  ExecuteJavaScript(const FString& Code) override;
    virtual void  SetMuted(bool bMuted) override;
    virtual void  FocusWebView() override;
    virtual void  SetZoomFactor(float Factor) override;
    virtual float GetZoomFactor() const override { return 1.0f; }
    virtual void  ClearAllCookies() override;
    virtual void  SetBackgroundOpaque(bool bOpaque) override;

private:
    /** Process-unique identifier passed across JNI; the Java side keeps a
     *  SparseArray<WebView> keyed by this int so primitives are all that
     *  cross the JNI boundary — no per-WebView jobject bookkeeping. */
    int32 InstanceId = 0;

    bool  bReady     = false;
    bool  bDestroyed = false;
};

#endif // PLATFORM_ANDROID
