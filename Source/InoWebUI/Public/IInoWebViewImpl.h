// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InoWebUITypes.h"

/**
 * IInoWebViewImpl — platform-agnostic contract for a single native WebView.
 *
 * This header is public, but only because UInoWebView holds a
 * TUniquePtr<IInoWebViewImpl> member and UHT-generated code needs the
 * complete type at compile time (FVTableHelper ctor, exception unwinding
 * paths). It contains NO platform-specific includes — just CoreMinimal and
 * FInoWebViewConfig — so publishing it leaks nothing. Concrete implementations
 * (e.g., FInoWebViewImpl_Windows) stay in Private/Impl/<Platform>/.
 *
 * Normal consumer code should never need to touch this interface — the
 * UObject layer (UInoWebView / UInoWebUISubsystem) is the intended API.
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
     * Update the WebView's bounds. Inputs are **screen-space pixel coords**
     * (origin = top-left of the monitor). The implementation is responsible
     * for converting them to whatever parent-local coord system the native
     * WebView expects.
     *
     * Reason for using screen coords instead of parent-client coords:
     * Slate-chromed UE windows (e.g., PIE "New Editor Window") have no OS
     * chrome, so the HWND's "client area" covers the entire window including
     * the Slate-drawn title bar. Passing screen coords lets us express the
     * actual usable content area (excluding Slate chrome) portably, and the
     * impl does a single ScreenToClient pass to project into its HWND.
     */
    virtual void SyncBounds(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height) = 0;

    /**
     * Release all native resources. MUST be called before the parent window
     * is destroyed — on Windows the WebView2 controller must be Close()'d
     * while its parent HWND still exists, otherwise we leak COM objects.
     *
     * Safe to call more than once. After Shutdown(), all other methods are
     * no-ops.
     */
    virtual void Shutdown() = 0;

    // ── Messaging (Phase 2) ─────────────────────────────────────────────────

    /**
     * Send a raw JSON string to the loaded page. Delivered to JS as the
     * `event.data` payload of a 'message' event on window.chrome.webview
     * (or equivalent per-platform). The string is passed verbatim — the
     * caller (UInoWebView) is responsible for envelope construction.
     *
     * Safe to call before IsReady(); implementations queue pre-ready
     * messages and replay them once ready.
     */
    virtual void PostMessageJson(const FString& Json) = 0;

    /**
     * Owner-settable callback fired when JS posts a message to the native
     * side. The string is the raw message content as sent by JS (our
     * envelope JSON, by convention). Always invoked on the game thread.
     *
     * The owner (UInoWebView) sets this once before Initialize(); it is
     * never overwritten at runtime. Not a virtual method because there's
     * no per-platform behavior — it's just a callback slot.
     */
    TFunction<void(const FString&)> OnMessageReceivedJson;

    // ── Navigation events (Phase 5) ─────────────────────────────────────────
    // Owner-settable, fired on the game thread. Observation only — lockdown
    // decisions are made inside the implementation using FInoWebViewConfig.

    TFunction<void(const FString& URI)>                  OnNavigationStartingCallback;
    TFunction<void(bool bSuccess, const FString& URI)>   OnNavigationCompletedCallback;
    TFunction<void(const FString& Title)>                OnDocumentTitleChangedCallback;
    TFunction<void(EInoScriptDialogKind, const FString& Message)>  OnScriptDialogCallback;
    TFunction<void(const FString& URI)>                  OnNewWindowRequestedCallback;
    TFunction<void()>                                    OnGotFocusCallback;
    TFunction<void()>                                    OnLostFocusCallback;
    TFunction<void(const FString& Description)>          OnProcessFailedCallback;

    /** Renderer-process responsiveness callbacks. The renderer is alive but
     *  its main thread hasn't returned to the event loop in ~5s (Unresponsive)
     *  or recovered (Responsive). Distinct from OnProcessFailed (which fires
     *  only when the process actually dies). Android-only today; Windows /
     *  iOS impls leave these slots untouched. */
    TFunction<void()>                                    OnRenderProcessUnresponsiveCallback;
    TFunction<void()>                                    OnRenderProcessResponsiveCallback;

    /** Fired for every page-side console.log / .warn / .error / .debug.
     *  Android-only today; Windows / iOS impls don't currently surface
     *  console messages (WebView2 doesn't expose them as events; WKWebView
     *  routes them only to Safari's Web Inspector). */
    TFunction<void(EInoConsoleMessageLevel /*Level*/,
                   const FString& /*Message*/,
                   const FString& /*SourceID*/,
                   int32          /*Line*/)>             OnConsoleMessageCallback;

    /** One-shot: fires when the native WebView transitions to ready. Called
     *  exactly once per impl lifetime. Owner (UInoWebView) defers the BP
     *  delegate broadcast to the next game tick so sync-ready platforms
     *  (Android) don't fire before the caller can bind. */
    TFunction<void()>                                    OnReadyCallback;

    // ── Phase 3 — runtime polish ────────────────────────────────────────────

    /**
     * Open the Chromium DevTools window. The underlying native API has no
     * corresponding "close" call — the user closes DevTools themselves.
     * bEnableDevTools in FInoWebViewConfig must be true for this to work.
     */
    virtual void OpenDevTools() = 0;

    /**
     * Execute arbitrary JavaScript inside the WebView. Fire-and-forget —
     * use the messaging API (PostMessage / OnMessageReceived) if you need
     * to get a result back to UE. Safe to call before IsReady() — scripts
     * are queued and replayed once ready.
     */
    virtual void ExecuteJavaScript(const FString& Code) = 0;

    /**
     * Mute/unmute audio output from the page. Safe to call before IsReady();
     * the requested state is queued and applied once ready. FInoWebViewConfig::
     * bStartMuted handles the one-shot "mute at startup" case.
     */
    virtual void SetMuted(bool bMuted) = 0;

    /** Move keyboard focus to the WebView (COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC). */
    virtual void FocusWebView() = 0;

    /** Set page zoom factor. 1.0 = 100%. No-op if not ready. */
    virtual void SetZoomFactor(float Factor) = 0;

    /** Returns the current zoom factor, or 1.0 if not ready / unavailable. */
    virtual float GetZoomFactor() const = 0;

    /** Delete all cookies from this WebView's isolated profile. */
    virtual void ClearAllCookies() = 0;

    /**
     * Flip the WebView's background between transparent and opaque at
     * runtime. bOpaque=true paints a solid color (currently white on
     * Windows, Color.WHITE on Android) behind the HTML, useful for
     * debugging where the UI's boundaries sit against the 3D scene.
     */
    virtual void SetBackgroundOpaque(bool bOpaque) = 0;

    // ── Browser-style nav helpers ───────────────────────────────────────────
    // GoBack / GoForward are no-ops if the impl isn't ready; CanGo* return
    // false unless the underlying native view says otherwise.
    virtual void GoBack() = 0;
    virtual void GoForward() = 0;
    virtual bool CanGoBack() const = 0;
    virtual bool CanGoForward() const = 0;

    // ── O(1) cached state getters ───────────────────────────────────────────
    // These return values that the impls keep in sync via existing event
    // callbacks (NavigationStarting/Completed, OnDocumentTitleChanged) and
    // the initial config. No native round-trip — guaranteed cheap.
    FString GetURL()   const { return CachedURL;   }
    FString GetTitle() const { return CachedTitle; }
    bool    IsLoading() const { return bCachedLoading; }

    /** Stop loading the current navigation. No-op if not ready. */
    virtual void StopLoading() = 0;

    /**
     * Load arbitrary HTML directly. Queued like Navigate if not ready.
     *
     * BaseURI behavior diverges by platform:
     *   • Windows (WebView2 NavigateToString): BaseURI is IGNORED — the
     *     resulting page sees `about:blank` as origin. If you need a base
     *     URI, use a virtual-host mapping and serve the HTML through that.
     *   • Android (loadDataWithBaseURL): BaseURI is honoured natively.
     */
    virtual void LoadHTMLString(const FString& HTML, const FString& BaseURI) = 0;

    /**
     * Set a single cookie for URL. Cookie is the raw HTTP cookie syntax
     * ("name=value; Path=/; Expires=...; HttpOnly; Secure; SameSite=Lax").
     * Queued if not ready.
     */
    virtual void SetCookie(const FString& URL, const FString& Cookie) = 0;

    /** Clear cookies + cache + storage for this WebView's profile. */
    virtual void ClearAllData() = 0;

    /**
     * Capture the WebView's current visual state and write it to OutFilePath.
     * Best-effort: returns false (no queueing) if the impl isn't ready, or
     * if any of the platform calls fail synchronously. Completion is async
     * either way — the OnCapturePreviewCompleteCallback fires when done.
     */
    virtual bool CapturePreview(EInoImageFormat Format, const FString& OutFilePath) = 0;

    /** Owner-settable; fired when CapturePreview finishes (success or fail). */
    TFunction<void(bool bSuccess, const FString& FilePath)> OnCapturePreviewCompleteCallback;

    /**
     * Navigate to URL with extra HTTP headers attached to the top-level
     * request. Headers DO NOT propagate to subresource requests — only the
     * navigation itself. Queued if not ready.
     */
    virtual void LoadURLWithHeaders(const FString& URL, const TMap<FString, FString>& Headers) = 0;

    // ── Sub-region bounds (item 9) ──────────────────────────────────────────
    // Same parameter semantics as SyncBounds — screen-space pixel coords for
    // X/Y, then width/height. Same impl behavior, just keyed off the
    // "manual bounds" mode the caller selected through UInoWebView.

    /**
     * Set whether this WebView should size itself manually (per SetBounds)
     * or automatically follow the parent's client rect (default).
     *
     * On Windows the impls don't actually need to know — they just put
     * whatever they're told. On Android, the Java side needs to know whether
     * to use MATCH_PARENT (auto) or explicit pixel sizing with margins
     * (manual). Default no-op so platforms that don't care can ignore it.
     */
    virtual void SetBoundsMode(bool bManual) { (void)bManual; }

protected:
    // Cached state — written by impls from existing event callbacks. Public
    // accessors above. Access from implementations is fine; access from
    // anywhere else should go through GetURL/GetTitle/IsLoading.
    FString CachedURL;
    FString CachedTitle;
    bool    bCachedLoading   = false;
    // Cached on Android via a Java->C++ callback after each nav event;
    // Windows reads them directly from WebView2 in CanGoBack/CanGoForward
    // and ignores these fields.
    bool    bCachedCanGoBack    = false;
    bool    bCachedCanGoForward = false;
};

/**
 * Factory — returns a platform-appropriate implementation, or nullptr on
 * unsupported platforms (dedicated servers, headless commandlets, etc.).
 *
 * Defined in InoWebViewFactory.cpp with platform #ifdefs.
 */
TUniquePtr<IInoWebViewImpl> CreateInoWebViewImpl();
