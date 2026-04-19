// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IInoWebViewImpl.h"

#if PLATFORM_WINDOWS

/**
 * FInoWebViewImpl_Windows — WebView2-backed implementation of IInoWebViewImpl.
 *
 * The public interface of this class is completely free of Windows.h /
 * WebView2.h. All COM state lives in an opaque FInternal struct defined in
 * the .cpp file, so no translation unit outside InoWebViewImpl_Windows.cpp
 * pays the cost of parsing WebView2.h.
 *
 * All methods must be called on the game thread.
 */
class FInoWebViewImpl_Windows : public IInoWebViewImpl
{
public:
    FInoWebViewImpl_Windows();
    virtual ~FInoWebViewImpl_Windows();

    //~ IInoWebViewImpl
    virtual bool Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config) override;
    virtual bool IsReady()  const override { return bReady; }
    virtual void Navigate(const FString& URL) override;
    virtual void Reload() override;
    virtual void SetVisible(bool bVisible) override;
    virtual void SyncBounds(int32 X, int32 Y, int32 Width, int32 Height) override;
    virtual void Shutdown() override;

private:
    /** Opaque state — defined in the .cpp so WebView2.h stays out of headers. */
    struct FInternal;
    TUniquePtr<FInternal> Internal;

    /** True once the WebView2 controller has finished its async construction. */
    bool bReady = false;

    /** True after the first successful Initialize() call. */
    bool bInitStarted = false;

    // Callback entry points, called from WRL lambdas after async completion.
    void OnEnvironmentReady(int32 HResult, void* EnvironmentPtr);
    void OnControllerReady (int32 HResult, void* ControllerPtr);
    void ApplyPendingOperations();
};

#endif // PLATFORM_WINDOWS
