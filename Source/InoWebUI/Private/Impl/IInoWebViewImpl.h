// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InoWebUITypes.h"

/**
 * IInoWebViewImpl — platform-agnostic contract for a single native WebView.
 *
 * This header is PRIVATE. It is never included by UObject-layer code
 * (UInoWebView, UInoWebUISubsystem) to keep platform headers (Windows.h,
 * WebView2.h) out of the public API surface.
 *
 * Implementations (one per platform):
 *   • Windows → FInoWebViewImpl_Windows  (WebView2)
 *   • macOS   → (future) WKWebView
 *   • Android → (future) android.webkit.WebView
 *
 * All methods MUST be called on the game thread. Implementations may defer
 * work internally but the entry points are not thread-safe.
 *
 * Lifetime contract:
 *   1. Construct via CreateInoWebViewImpl()  (factory in InoWebViewFactory.h)
 *   2. Call Initialize(ParentWindow, Config) exactly once
 *   3. Any sequence of Navigate / SetVisible / Reload / SyncBounds
 *   4. Call Shutdown() before destruction, BEFORE the parent window dies
 *   5. Destructor
 */
class IInoWebViewImpl
{
public:
    virtual ~IInoWebViewImpl() = default;

    /**
     * Create the underlying native WebView and attach it to ParentNativeHandle.
     * Returns false if the platform isn't supported or the handle is invalid.
     *
     * NOTE: The WebView may not be fully ready when this returns — native
     * WebView SDKs typically initialize asynchronously. Queries and operations
     * issued before the WebView is ready are queued and replayed on ready.
     * Poll IsReady() to know when the WebView can actually render content.
     */
    virtual bool Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config) = 0;

    /** True once the native WebView has finished its async initialization. */
    virtual bool IsReady() const = 0;

    /** Navigate the WebView to the given URL. Safe to call before IsReady(). */
    virtual void Navigate(const FString& URL) = 0;

    /** Reload the current page. No-op if nothing is loaded. */
    virtual void Reload() = 0;

    /** Show or hide the native WebView surface. */
    virtual void SetVisible(bool bVisible) = 0;

    /**
     * Update the WebView's bounds to match a rectangle in client-space of the
     * parent window (origin = parent's client top-left, not screen coords).
     * Called by the subsystem whenever the UE viewport resizes.
     */
    virtual void SyncBounds(int32 X, int32 Y, int32 Width, int32 Height) = 0;

    /**
     * Release all native resources. MUST be called before the parent window
     * is destroyed — on Windows the WebView2 controller must be Close()'d
     * while its parent HWND still exists, otherwise we leak COM objects.
     *
     * Safe to call more than once. After Shutdown(), all other methods are
     * no-ops.
     */
    virtual void Shutdown() = 0;
};

/**
 * Factory — returns a platform-appropriate implementation, or nullptr on
 * unsupported platforms (dedicated servers, headless commandlets, etc.).
 *
 * Defined in InoWebViewFactory.cpp with platform #ifdefs.
 */
TUniquePtr<IInoWebViewImpl> CreateInoWebViewImpl();
