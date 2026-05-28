// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "InoWebUITypes.h"
#include "IInoWebViewImpl.h"     // complete type needed for TUniquePtr<> member
#include "JsonObjectWrapper.h"   // FJsonObjectWrapper — BP-friendly JSON
#include "InoWebView.generated.h"

/**
 * Fired when the loaded page posts a message via window.InoWebUI.send(...).
 *   Channel — the channel name the page passed to send()
 *   Payload — the payload as a BP-friendly JSON object wrapper. Non-object
 *             payloads (scalars, arrays, null) are auto-wrapped in
 *             { "value": <payload> } so BP always sees a JsonObject.
 *             Read fields with the JsonBlueprintUtilities nodes (GetField,
 *             HasField, GetFieldNames, Get Json String).
 *
 * Always broadcast on the game thread. Bind it as a Blueprint event pin or
 * via OnMessageReceived.AddDynamic() in C++.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoWebMessage,
    FName,                      Channel,
    const FJsonObjectWrapper&,  Payload);

/** Fired just before a navigation starts. Observation only — lockdown
 *  (FInoWebViewConfig::bLockToVirtualHost) handles cancellation internally. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoWebNavigationStarting,
    const FString&, URI);

/** Fired when navigation completes (successfully or not). bSuccess=false
 *  on broken links, network failures, navigation cancelled by lockdown. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoWebNavigationCompleted,
    bool,            bSuccess,
    const FString&,  URI);

/** Fired when the page's document.title changes. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoWebTitleChanged,
    const FString&, Title);

/** Fired when JS calls alert/confirm/prompt. Observation only — suppression
 *  happens when bAllowScriptDialogs is false. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoWebScriptDialog,
    EInoScriptDialogKind, Kind,
    const FString&,       Message);

/** Fired when JS calls window.open or clicks target="_blank". Observation only —
 *  the popup is blocked by default. BP can call LoadURL(URI) to redirect. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoWebNewWindowRequested,
    const FString&, URI);

/** Fired when focus enters or leaves the WebView. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoWebFocusChanged);

/** Fired when a Chromium subprocess fails (renderer crashed, OOM, etc.).
 *  After this the WebView may be in an unusable state — a common response
 *  is to Reload() or destroy and recreate. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoWebProcessFailed,
    const FString&, Description);

/** One-shot signal: the native WebView has finished async construction and
 *  can safely accept operations. Fires exactly once on the game thread, on
 *  a tick AFTER CreateWebView returned — so BP bind order like
 *      View = CreateWebView(...);
 *      View.OnReady.AddDynamic(...);
 *  works reliably on every platform (even platforms where native
 *  construction is synchronous, like Android). If you bind AFTER the
 *  WebView is already ready, use IsReady() to check — OnReady has fired
 *  and won't fire again. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoWebReady);

/** Fired when the "Custom" (last) button on the dev overlay is clicked.
 *  Project-specific dev action — bind to do whatever you want in-game. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoWebDevCallback);

/** Fired when CapturePreview finishes (success or failure).
 *  bSuccess=true means the image was written to FilePath. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoWebCapturePreviewComplete,
    bool,           bSuccess,
    const FString&, FilePath);

/**
 * UInoWebView — Blueprint-visible handle to a single native WebView overlay.
 *
 * You never construct this directly. Ask UInoWebUISubsystem::CreateWebView()
 * for one. The subsystem owns the object lifetime via UPROPERTY, so the
 * garbage collector only collects a UInoWebView after the subsystem releases
 * it (usually on GameInstance teardown or explicit DestroyWebView).
 *
 * The public API is intentionally tiny — everything you need in Phase 1:
 *
 *   LoadURL / LoadLocalFile     — point the WebView at content
 *   Show / Hide                 — toggle visibility
 *   Reload                      — refresh current page
 *   IsReady                     — has the underlying native WebView finished
 *                                 its async initialization?
 *
 * All methods are safe to call immediately after creation, even before the
 * underlying WebView has asynchronously initialized. Operations issued before
 * ready are queued and replayed once ready.
 */
UCLASS(BlueprintType)
class INOWEBUI_API UInoWebView : public UObject
{
    GENERATED_BODY()

public:
    UInoWebView();

    // ── Navigation ──────────────────────────────────────────────────────────

    /** Navigate to an absolute URL (http, https, file, about, etc.). */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void LoadURL(const FString& URL);

    /**
     * Navigate to a file on disk, relative to the project's Content directory.
     * Example:  LoadLocalFile(TEXT("WebUI/dist/index.html"))
     *       →  navigates to  file:///Absolute/.../Content/WebUI/dist/index.html
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void LoadLocalFile(const FString& RelativeContentPath);

    /** Reload the current page. No-op if nothing loaded yet. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void Reload();

    // ── Visibility ──────────────────────────────────────────────────────────

    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void Show();

    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void Hide();

    // ── Appearance ──────────────────────────────────────────────────────────

    /**
     * Switch the WebView background between transparent (the 3D scene shows
     * through the page's empty areas) and opaque white, at runtime. This is
     * the runtime equivalent of FInoWebViewConfig's bTransparentBackground.
     *
     * Also feeds the subsystem's auto engine-idle: going opaque while
     * visible can idle Unreal; going transparent resumes it (only if
     * SetAutoEngineIdle is on). Safe before the WebView is ready (applied
     * once the native side is up). Game-thread only.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetBackgroundTransparent(bool bTransparent);

    /** Current background mode. True = transparent, false = opaque white. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    bool IsBackgroundTransparent() const { return !bBackgroundCurrentlyOpaque; }

    // ── Messaging (Phase 2) ─────────────────────────────────────────────────

    /**
     * Push a message to the loaded page. On the JS side, handlers subscribed
     * via window.InoWebUI.on(Channel, ...) will fire with the parsed payload.
     *
     * @param Channel  Short identifier the JS side listens on.
     * @param Payload  BP-friendly JSON object. Build it with the
     *                 JsonBlueprintUtilities nodes (SetField / Load Json from
     *                 String) and the fields will arrive as a JS object on
     *                 the other side. An empty / invalid wrapper sends null.
     *
     * Safe to call before the WebView finishes loading; messages are queued
     * and delivered once the underlying native WebView is ready.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI",
              meta = (AutoCreateRefTerm = "Payload"))
    void PostMessage(FName Channel, const FJsonObjectWrapper& Payload);

    /**
     * Fires whenever the page calls window.InoWebUI.send(Channel, payload).
     * Bind as a Blueprint event pin or with AddDynamic() in C++.
     */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebMessage OnMessageReceived;

    // ── Navigation events (Phase 5) ─────────────────────────────────────────

    /** Fires before each navigation. Observation only. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebNavigationStarting OnNavigationStarting;

    /** Fires when a navigation finishes (success or failure). */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebNavigationCompleted OnNavigationCompleted;

    /** Fires when document.title changes. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebTitleChanged OnDocumentTitleChanged;

    /** Fires when a JS dialog opens (alert/confirm/prompt). Observation only. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebScriptDialog OnScriptDialog;

    /** Fires when JS requests a new window (window.open/_blank). Observation only. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebNewWindowRequested OnNewWindowRequested;

    /** Fires when the WebView receives keyboard focus. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebFocusChanged OnGotFocus;

    /** Fires when the WebView loses keyboard focus. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebFocusChanged OnLostFocus;

    /** Fires when a Chromium subprocess fails (renderer crash, OOM, …). */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebProcessFailed OnProcessFailed;

    /** Fires once when the underlying native WebView has finished async
     *  construction and is ready for operations. See FOnInoWebReady docs. */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebReady OnReady;

    /** Fires when the "Custom" button on the dev overlay is clicked. Use
     *  this as a project-specific dev hook — bind to do whatever you
     *  need (dump game state, trigger a debug menu, etc.). */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI|DevTools")
    FOnInoWebDevCallback OnDevCallback;

    // ── Runtime polish (Phase 3) ────────────────────────────────────────────

    /**
     * Open the Chromium DevTools window. Requires bEnableDevTools=true on
     * the config passed to CreateWebView. No-op otherwise (with a warning).
     * The user closes DevTools themselves — there's no programmatic close
     * in the WebView2 API.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void OpenDevTools();

    /**
     * Execute arbitrary JavaScript. Fire-and-forget — use the messaging
     * API (PostMessage / OnMessageReceived) if you need the result back.
     * Safe to call before IsReady(); scripts are queued.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void ExecuteJavaScript(const FString& Code);

    /** Mute / unmute audio output from the page. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetMuted(bool bMuted);

    /**
     * Give keyboard focus to the WebView so HTML <input> / <textarea>
     * elements receive typing. Without this, the game might still have
     * focus while the cursor is in a text field.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void FocusWebView();

    /** Set page zoom factor. 1.0 = 100%, 1.5 = 150%, etc. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetZoomFactor(float Factor);

    /** Current zoom factor. Returns 1.0 if unavailable. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    float GetZoomFactor() const;

    /**
     * Clear every cookie in this WebView's isolated profile. Useful for
     * "log out" flows. Async internally — no completion event is surfaced.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void ClearAllCookies();

    // ── Browser-style nav (history) ─────────────────────────────────────────

    /** Step back in history. No-op if not ready or no back history. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void GoBack();

    /** Step forward in history. No-op if not ready or no forward history. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void GoForward();

    /** Whether GoBack will do anything. Returns false if not ready. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    bool CanGoBack() const;

    /** Whether GoForward will do anything. Returns false if not ready. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    bool CanGoForward() const;

    /** Stop the current navigation. No-op if nothing is loading. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void StopLoading();

    /**
     * Load arbitrary HTML into the WebView. BaseURI is honoured on Android
     * (loadDataWithBaseURL) but ignored on Windows (WebView2 NavigateToString
     * has no equivalent — use a virtual-host mapping if you need a real
     * origin). Queued if not ready.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void LoadHTMLString(const FString& HTML, const FString& BaseURI);

    /**
     * Set a single cookie in HTTP cookie syntax for URL.
     *   "name=value; Path=/; Expires=Wed, 09 Jun 2027 10:18:14 GMT; Secure"
     * Queued if not ready.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetCookie(const FString& URL, const FString& Cookie);

    /**
     * Wipe all browsing data for this WebView's profile — cookies, cache,
     * Web Storage (localStorage/sessionStorage), history. Async internally.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void ClearAllData();

    /**
     * Snapshot the WebView's current visual state to OutFilePath in the
     * requested format. Async — bind OnCapturePreviewComplete to know when
     * the file is written. Returns false synchronously if the WebView isn't
     * ready or the platform call failed; otherwise true (async path took
     * over).
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    bool CapturePreview(EInoImageFormat Format, const FString& OutFilePath);

    /** Fires when CapturePreview completes (success or fail). */
    UPROPERTY(BlueprintAssignable, Category = "Ino|WebUI")
    FOnInoWebCapturePreviewComplete OnCapturePreviewComplete;

    /**
     * Navigate to URL with extra HTTP headers attached to the top-level
     * request. Headers are NOT applied to subresource requests — Chromium
     * fetches assets with its normal header set. Queued if not ready.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void LoadURLWithHeaders(const FString& URL, const TMap<FString, FString>& Headers);

    // ── Sub-region bounds (manual/auto modes) ───────────────────────────────

    /**
     * Switch this WebView to MANUAL bounds mode and immediately apply
     * the supplied rect (X/Y/W/H, screen-space pixels). The subsystem's
     * automatic resize broadcasts will skip this WebView until you call
     * SetBoundsAuto() to re-enable them.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetBounds(int32 X, int32 Y, int32 W, int32 H);

    /**
     * Switch this WebView back to AUTO bounds mode (follow the parent
     * client rect on every viewport resize) and immediately push the
     * current rect to apply right now.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void SetBoundsAuto();

    /** Whether SetBounds was called and SetBoundsAuto hasn't been called since. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    bool IsManualBounds() const { return bManualBounds; }

    // ── State queries ───────────────────────────────────────────────────────

    /** True once the native WebView has finished its async construction. */
    UFUNCTION(BlueprintPure, Category = "Ino|WebUI")
    bool IsReady() const;

    /** Cached current page URL — set on each NavigationCompleted. O(1). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    FString GetURL() const;

    /** Cached current page title — set on each OnDocumentTitleChanged. O(1). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    FString GetTitle() const;

    /** True between NavigationStarting and NavigationCompleted. O(1). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    bool IsLoading() const;

    /** The Name originally passed to CreateWebView — useful for lookups/logs. */
    UFUNCTION(BlueprintPure, Category = "Ino|WebUI")
    FName GetWebViewName() const { return WebViewName; }

    //~ UObject
    virtual void BeginDestroy() override;

    // ── Internal (called by the subsystem only) ─────────────────────────────

    /**
     * Called once by the subsystem immediately after NewObject<UInoWebView>.
     * Takes the platform implementation, the HWND, and the config, then kicks
     * off async initialization. Not a UFUNCTION — Blueprints shouldn't call this.
     */
    void Init(FName InName, TUniquePtr<IInoWebViewImpl>&& InImpl,
              void* ParentNativeHandle, const FInoWebViewConfig& Config);

    /**
     * Called by the subsystem on viewport resize. Parameters are in the
     * parent window's client-area pixel coordinates.
     */
    void OnParentResized(int32 X, int32 Y, int32 Width, int32 Height);

    /**
     * Tear down the underlying native WebView. Called by the subsystem
     * during DestroyWebView() or Deinitialize(). Safe to call more than once.
     */
    void ShutdownImpl();

private:
    /** Stable name used for subsystem map lookups and log tagging. */
    UPROPERTY()
    FName WebViewName;

    /** Platform implementation. Null on unsupported platforms / after shutdown. */
    TUniquePtr<IInoWebViewImpl> Impl;

    /** Tracks the current background opacity. Initialized from
     *  Config.View.bTransparentBackground in Init and flipped by the dev
     *  overlay's "Toggle transparency". Part of the "covering" state. */
    bool bBackgroundCurrentlyOpaque = false;

    /** Tracks Show()/Hide() visibility. Initialized from
     *  Config.View.bVisibleOnCreate in Init. Part of the "covering" state. */
    bool bViewVisible = true;

    /**
     * Report this view's "covering" state (opaque AND visible — i.e. it
     * fully hides the 3D scene) to the owning subsystem. The subsystem
     * decides whether that idles the engine (only when its auto mode is
     * on). Recomputed on Show/Hide and on every transparency switch.
     * Cheap; safe to call repeatedly.
     */
    void RefreshCoveringState();

    /** True when SetBounds is in effect. Subsystem skips this WebView when
     *  broadcasting the parent client rect. SetBoundsAuto resets it. */
    bool bManualBounds = false;

    /** Last manually-set rect (only meaningful while bManualBounds). */
    int32 ManualX = 0;
    int32 ManualY = 0;
    int32 ManualW = 0;
    int32 ManualH = 0;

    /**
     * Parse the raw envelope JSON pushed by the impl's OnMessageReceivedJson
     * callback and broadcast OnMessageReceived to Blueprint subscribers.
     * Always runs on the game thread.
     */
    void DispatchIncomingEnvelope(const FString& EnvelopeJson);

    /**
     * Handle a "_devtools.*" channel from the injected dev overlay. Returns
     * true if the channel was consumed (and should NOT be forwarded to the
     * user's OnMessageReceived delegate); false otherwise.
     */
    bool HandleDevToolsAction(const FString& Channel);

    // ── Auto-recover on renderer process failure ────────────────────────────
    //
    // When the underlying native renderer dies (Chromium subprocess OOM-killed
    // or crashed) the platform impl's OnProcessFailed fires. Per the Android
    // WebView lifecycle docs the dead native WebView is unusable after that
    // point — you must detach + destroy it. Auto-recover takes the next step
    // and re-initializes a fresh native impl with the same Config so the
    // overlay comes back without the caller having to manually handle it.
    //
    // Reliability rationale:
    //   • Persistent state (cookies, localStorage, IndexedDB) lives in the
    //     WebView's data dir, NOT the renderer process, so it survives.
    //   • The UInoWebView UObject (this) and all Blueprint delegate bindings
    //     persist across the recreate — only the Impl swap happens under us.
    //   • Pages must already handle a normal reload (same as F5), so the
    //     recreate looks identical to that from the page's perspective.
    //
    // Bounded by a recovery budget (max 3 attempts within 60s) to prevent an
    // infinite recreate loop on a page that consistently crashes the
    // renderer — after the budget is exhausted we leave the WebView dead
    // and the user code can decide what to do next.

    /** Copy of the Config originally passed to Init; replayed by RecreateImpl. */
    UPROPERTY()
    FInoWebViewConfig SavedConfig;

    /** Parent OS-level window handle (HWND on Windows) captured at Init. Plain
     *  pointer — not a UPROPERTY because it's an OS handle, not a UObject.
     *  Valid for the lifetime of the GameInstance / parent window. */
    void* SavedParentNativeHandle = nullptr;

    /** FPlatformTime::Seconds() timestamps of recent auto-recovery attempts.
     *  Trimmed in TryConsumeRecoveryBudget to within the rolling window. */
    TArray<double> RecentRecoveryAttempts;

    /** Set when HandleProcessFailed schedules a deferred recreate; cleared
     *  once the recreate runs or on ShutdownImpl. Prevents a recreate from
     *  firing after the caller has destroyed the WebView in their
     *  OnProcessFailed handler. */
    bool bAwaitingRecreate = false;

    /** Wire every IInoWebViewImpl callback slot to a lambda that broadcasts
     *  the matching BP delegate. Extracted from Init() so RecreateImpl can
     *  reuse it on the fresh impl. */
    void WireImplCallbacks();

    /** Called by the OnProcessFailedCallback lambda. Fires the BP delegate
     *  first, then — if SavedConfig.bAutoRecoverOnProcessFailed and the
     *  recovery budget allows — schedules a deferred RecreateImpl on the
     *  next game tick. */
    void HandleProcessFailed(const FString& Description);

    /** Returns true if we still have budget for one more auto-recover (and
     *  records the attempt). Sliding window of MaxRecoveriesPerWindow=3
     *  attempts per RecoveryWindowSec=60 seconds. */
    bool TryConsumeRecoveryBudget();

    /** Tear down the current Impl, allocate a new one via the factory, wire
     *  callbacks, re-Initialize with SavedConfig + SavedParentNativeHandle,
     *  and restore current bounds / visibility / transparency. Logs at every
     *  step. */
    void RecreateImpl();
};
