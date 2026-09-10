// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "InoWebUITypes.generated.h"

/**
 * Which flavor of JS dialog fired (alert/confirm/prompt/beforeunload).
 * Maps 1:1 to COREWEBVIEW2_SCRIPT_DIALOG_KIND.
 */
UENUM(BlueprintType)
enum class EInoScriptDialogKind : uint8
{
    Alert         UMETA(DisplayName = "alert()"),
    Confirm       UMETA(DisplayName = "confirm()"),
    Prompt        UMETA(DisplayName = "prompt()"),
    BeforeUnload  UMETA(DisplayName = "onbeforeunload"),
};

/**
 * Severity of a console.log()/.info()/.warn()/.error()/.debug() call from
 * the page. Values match Android's ConsoleMessage.MessageLevel ordinals
 * exactly so the JNI bridge can pass through as a plain jint with no
 * mapping table:
 *
 *   0 = Tip      (rare; browser-emitted hints)
 *   1 = Log      (console.log, console.info)
 *   2 = Warning  (console.warn)
 *   3 = Error    (console.error)
 *   4 = Debug    (console.debug)
 *
 * Windows / iOS impls don't currently emit OnConsoleMessage — Chromium's
 * WebView2 doesn't expose a console-message event, and WKWebView routes
 * console output through Safari's Web Inspector only.
 */
UENUM(BlueprintType)
enum class EInoConsoleMessageLevel : uint8
{
    Tip      = 0   UMETA(DisplayName = "Tip"),
    Log      = 1   UMETA(DisplayName = "Log"),
    Warning  = 2   UMETA(DisplayName = "Warning"),
    Error    = 3   UMETA(DisplayName = "Error"),
    Debug    = 4   UMETA(DisplayName = "Debug"),
};

/**
 * One cookie to seed into a WebView's cookie store before its first
 * navigation. `Cookie` is the standard HTTP cookie syntax:
 *     "name=value; Path=/; Expires=Wed, 09 Jun 2026 10:18:14 GMT; Secure"
 * `URL` selects the cookie store the cookie is written into (typically
 * the origin you're about to navigate to, e.g. "https://api.foo.com").
 */
USTRUCT(BlueprintType)
struct INOWEBUI_API FInoInitialCookie
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString URL;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString Cookie;
};

/**
 * Image format selector for UInoWebView::CapturePreview. PNG is lossless and
 * the default; JPEG is lossy but smaller for screenshots without alpha.
 */
UENUM(BlueprintType)
enum class EInoImageFormat : uint8
{
    PNG   UMETA(DisplayName = "PNG"),
    JPEG  UMETA(DisplayName = "JPEG"),
};

/**
 * Severity / styling category for UInoWebView::ShowNotification. Drives the
 * toast's accent colour and glyph in the injected notify_overlay.js. The
 * enum names map to the lowercase JS category strings ('info' / 'warning' /
 * 'error') the overlay expects.
 */
UENUM(BlueprintType)
enum class EInoNotifyCategory : uint8
{
    Info     UMETA(DisplayName = "Info"),
    Warning  UMETA(DisplayName = "Warning"),
    Error    UMETA(DisplayName = "Error"),
};

// ─────────────────────────────────────────────────────────────────────────────
//  FInoWebViewSettings
//
//  View-level appearance / behavior toggles. Embedded inside FInoWebViewConfig
//  as the `View` field (shown flat in the Details panel via
//  ShowOnlyInnerProperties). Defaults are tuned for "locked-down game UI"
//  rather than browser-like behaviour — most flags ship false / off so a
//  fresh CreateWebView produces a focused overlay rather than a Safari tab.
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct INOWEBUI_API FInoWebViewSettings
{
    GENERATED_BODY()

    /**
     * If true (default), the WebView renders with a fully transparent
     * background so the Unreal 3D scene shows through the HTML's empty areas.
     * If false, the WebView paints an opaque white background (standard web).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bTransparentBackground = true;

    /**
     * Whether the WebView is visible immediately on creation.
     * You can flip this later via UInoWebView::Show / Hide.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bVisibleOnCreate = true;

    // ── Dev / debug ─────────────────────────────────────────────────────────

    /**
     * Enable the Chromium DevTools panel (Windows) / chrome://inspect remote
     * debugging (Android) / Safari Web Inspector hint (iOS). Open
     * programmatically with UInoWebView::OpenDevTools(), or with F12 if
     * bEnableAcceleratorKeys is also true on Windows. Usually enabled in
     * dev builds, disabled in shipping.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bEnableDevTools = false;

    /**
     * If false (default), the WebView's status bar — the small floating
     * label that shows a link's URL when you hover over it — is hidden.
     * Game UI almost never wants this; flip true for browser-style flows
     * where seeing destinations on hover is helpful (e.g., dev menus).
     * Windows-only — Android / iOS WebViews have no equivalent.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Show Status Bar (Windows)"))
    bool bShowStatusBar = false;

    // ── Interaction ─────────────────────────────────────────────────────────

    /**
     * If false, right-clicking inside the WebView does nothing (the browser
     * "Save image as…/Inspect element" menu is suppressed). Recommended
     * for game UI. Default false.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bEnableContextMenus = false;

    /**
     * If false, browser-level accelerator keys like F5 (reload), F12
     * (devtools), Ctrl+F (find in page), Ctrl+P (print) are suppressed.
     * Recommended for game UI so those keys remain available to the game.
     * Default false. NOTE: disabling accelerators also disables F12 as a
     * way to open DevTools — use OpenDevTools() programmatically instead.
     * Windows-only concept.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Enable Accelerator Keys (Windows)"))
    bool bEnableAcceleratorKeys = false;

    /**
     * If false (default), pinch zoom and double-tap zoom are suppressed —
     * the page renders at 100% and stays there. Game UI almost always wants
     * this off; flip true for a docs / browser-style WebView where users
     * may want to zoom in.
     *
     * Applied via pure native APIs on every platform — no JS / CSS injection:
     *   • iOS: scrollView.min/maxZoomScale = 1 + pinchGestureRecognizer.enabled = NO
     *   • Android: WebSettings.setSupportZoom + setBuiltInZoomControls
     *   • Windows: ICoreWebView2Settings5.IsPinchZoomEnabled
     *
     * iOS gotcha — input auto-zoom is NOT covered. iOS focuses inputs by
     * scaling the WebKit viewport (a separate mechanism from the scrollView
     * zoom this flag controls), and there's no native API to disable it.
     * If the page focuses an `<input>` whose CSS font-size is below 16px,
     * iOS will still auto-zoom in. Two HTML-side fixes:
     *   • <meta name="viewport" content="width=device-width, initial-scale=1,
     *     maximum-scale=1, user-scalable=no"> in the page <head>, OR
     *   • CSS `input, textarea, select { font-size: 16px; }`.
     * Either of those, plus this flag, fully kills zoom on iOS.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bAllowZoom = false;

    /**
     * If false (default), the WebView's vertical / horizontal scroll
     * indicators are hidden. Scrolling itself still works — only the
     * gutter-style scrollbar visual is suppressed. Game UI almost always
     * wants the cleaner no-scrollbar look.
     *
     * Applied natively on iOS / Android (UIScrollView / WebView properties).
     * Windows / WebView2 has no native API to hide scrollbars; the flag is
     * a no-op there, and HTML can hide them with CSS
     * `::-webkit-scrollbar { display: none }` if needed.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Show Scroll Bars (iOS, Android)"))
    bool bShowScrollBars = false;

    /**
     * If false (default), page JavaScript cannot request fullscreen via
     * `element.requestFullscreen()`. WebKit ships fullscreen disabled by
     * default in WKWebView; flip true when the page needs e.g. fullscreen
     * HTML5 video or a fullscreen WebGL canvas. Maps to
     * `WKPreferences.isElementFullscreenEnabled` (iOS 16+).
     *
     * iOS-only today — Windows / Android have different fullscreen models
     * and ignore this flag.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Allow Element Fullscreen (iOS)"))
    bool bAllowElementFullscreen = false;

    /**
     * If true (default), the user can select / long-press / use the system
     * text-interaction callout inside the WebView. Game UI rarely needs text
     * selection; flip false to suppress the selection UI entirely (taps
     * still work, but the long-press callout / loupe never appears).
     * Maps to `WKPreferences.isTextInteractionEnabled` (iOS 15+).
     *
     * iOS-only — Windows / Android have their own selection mechanics and
     * ignore this flag.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Allow Text Interaction (iOS)"))
    bool bAllowTextInteraction = true;

    // ── Media ──────────────────────────────────────────────────────────────

    /** Audio output from the page is muted on creation when true. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    bool bStartMuted = false;

    /**
     * If true (default), HTML5 video plays inline within the page instead
     * of being forced into the system fullscreen player. Game UI almost
     * always wants inline. iOS-specific — Android and Windows always play
     * inline regardless. Maps to WKWebViewConfiguration.allowsInlineMediaPlayback.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Allow Inline Media Playback (iOS)"))
    bool bAllowInlineMediaPlayback = true;

    /**
     * If true (default), HTML5 audio / video can start playing without a
     * user gesture (tap / click). Game UI typically wants autoplay enabled
     * — the user opening the menu IS the gesture. iOS + Android.
     *   • iOS: WKWebViewConfiguration.mediaTypesRequiringUserActionForPlayback
     *   • Android: WebSettings.setMediaPlaybackRequiresUserGesture
     * Windows / Edge respects its own browser-level autoplay policy and
     * doesn't expose this as a per-WebView setting; flag is no-op there.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Allow Media Autoplay (iOS, Android)"))
    bool bAllowMediaAutoplay = true;

    // ── Layout (iOS-specific) ──────────────────────────────────────────────

    /**
     * If true (default), the WebView extends edge-to-edge under the iOS
     * notch / home indicator. The page can use the standard CSS env vars
     * (env(safe-area-inset-top) etc.) to keep tappable elements clear of
     * those areas — same model native iOS apps use.
     *
     * If false, the WebView is inset to respect the safe area; nothing
     * draws under the notch.
     *
     * iOS-only. Android handles display cutouts at the activity / manifest
     * level (UE's default GameActivity already extends under cutouts);
     * the flag is no-op on Android. Windows has no notch concept.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Extend Under Safe Area (iOS)"))
    bool bExtendUnderSafeArea = true;

    /**
     * If false (default), the iOS rubber-band / elastic scroll effect at
     * the top and bottom of the page is suppressed — when the user scrolls
     * past the content edge, the page stops instead of bouncing the whole
     * UI. Scrolling itself still works normally; only the over-scroll
     * bounce is disabled.
     *
     * If true, the WebView bounces like a regular Safari page.
     *
     * iOS-only. Android does not bounce by default (it shows an edge-glow
     * effect that is unrelated). Windows has no equivalent. Maps to
     * scrollView.bounces on iOS.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Allow Bounce On Scroll (iOS)"))
    bool bAllowBounceOnScroll = false;

    // ── Identity ───────────────────────────────────────────────────────────

    /**
     * When non-empty, appends to the default WebKit user-agent string —
     * "Mozilla/5.0 (...) AppleWebKit/... (KHTML, like Gecko) <ApplicationName>".
     * Keeps the WebKit version and platform info that compatibility-detecting
     * sites depend on, just tagged with your app name for analytics or
     * Unreal-specific page branches. Maps to
     * `WKWebViewConfiguration.applicationNameForUserAgent`.
     *
     * Prefer this over `UserAgentOverride` for tagging — the override below
     * REPLACES the entire UA, which can break sites that sniff for the
     * WebKit version. Both apply to iOS only; Windows / Android use their
     * own paths.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View",
              meta = (DisplayName = "Application Name (iOS, additive UA)"))
    FString ApplicationName;

    /**
     * When non-empty, overrides navigator.userAgent inside the WebView.
     * Useful for Unreal-specific page branches, e.g.:
     *   if (navigator.userAgent.includes('Unreal')) { ... }
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI|View")
    FString UserAgentOverride;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FInoWebViewConfig
//
//  Passed to UInoWebUISubsystem::CreateWebView. Top-level container for
//  everything a WebView needs to start: what to load, where it loads from,
//  what's allowed, what initial state to seed. View-level appearance /
//  behavior toggles live in the embedded FInoWebViewSettings (see View).
// ─────────────────────────────────────────────────────────────────────────────

USTRUCT(BlueprintType)
struct INOWEBUI_API FInoWebViewConfig
{
    GENERATED_BODY()

    /**
     * URL to load as soon as the WebView is ready.
     * Leave empty to create a blank WebView and navigate later via LoadURL.
     *
     * Accepted forms:
     *   http://…  https://…  file:///…   (and anything else Chromium accepts)
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString InitialURL;

    /**
     * Subfolder under Saved/ where WebView2 persists user data
     * (cookies, localStorage, cache). One folder per WebView keeps state
     * isolated; shared folders let WebViews share a login session.
     * Windows-only — Android / iOS use the system-default WebView storage.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString UserDataSubfolder = TEXT("WebViewData");

    /**
     * View-level appearance and behavior toggles (transparency, dev tools,
     * context menus, media, safe-area, scroll bounce, user agent, ...).
     * Shown flat in the Details panel — fields appear at the same level as
     * the parent's siblings here.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (ShowOnlyInnerProperties))
    FInoWebViewSettings View;

    // ── Local content serving (virtual host) ────────────────────────────────

    /**
     * Virtual host name that will be mapped to VirtualHostFolder. When both
     * are set, requests to  https://<VirtualHostName>/<path>  are served
     * from disk by WebView2's built-in loopback (SetVirtualHostNameToFolderMapping).
     * This is the RECOMMENDED way to serve production web UI — it fixes all
     * the file:// pitfalls (relative imports, React Router, fetch CORS,
     * service workers, ES modules).
     *
     * Convention: a short dotted name that won't collide with real DNS,
     * like "inoweb.local" or "ui.local". The ".local" suffix is RFC 6762
     * link-local — browsers treat it as always-local for security purposes.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString VirtualHostName;

    /**
     * Folder on disk that VirtualHostName maps to. Treated as relative to
     * the project's Content/ directory unless absolute (contains ':' or
     * starts with '/').
     *
     * Example:
     *   VirtualHostName   = "inoweb.local"
     *   VirtualHostFolder = "WebUI/dist"
     *   InitialURL        = "https://inoweb.local/index.html"
     *   → serves  <project>/Content/WebUI/dist/index.html  and every
     *     relative import/asset reference inside it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString VirtualHostFolder;

    // ── Hardening / lockdown ────────────────────────────────────────────────

    /**
     * If true (default), the WebView refuses to navigate anywhere except:
     *   • the configured VirtualHostName (whole host),
     *   • internal schemes  (about:, data:, blob:),
     *   • any entry in AllowedURIPatterns (UE wildcard match).
     * Everything else is cancelled and logged.
     *
     * Turn off only for debug/dev flows — it's the primary defense against
     * accidental navigation away from your game UI (bad links, injected
     * content, third-party JS redirects).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bLockToVirtualHost = true;

    // Additional URIs allowed past the lockdown. Each entry is a UE wildcard
    // pattern (* = any chars, ? = single char) matched against the full
    // navigation URI. Examples (note: these lines use // comments instead of
    // /** */ because Clang -Wcomment rejects nested /* inside a block comment,
    // and wildcard-heavy patterns like "foo/*" would trip it):
    //
    //   "https://*.api.company.com/*"    — any subdomain of api.company.com
    //   "http://localhost:*/*"           — any localhost port (dev)
    //   "https://cdn.example.com/*"      — specific CDN
    //
    // Empty list + bLockToVirtualHost=true = only VirtualHostName is allowed.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    TArray<FString> AllowedURIPatterns;

    /**
     * If false (default), JS alert() / confirm() / prompt() / onbeforeunload
     * dialogs are SUPPRESSED — alert() returns normally, confirm()/prompt()
     * see "cancelled", beforeunload is skipped. The OnScriptDialog delegate
     * still fires so you can observe and/or present your own in-game UI.
     *
     * Turn on only for debug/dev when you actually want the native dialogs.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bAllowScriptDialogs = false;

    /**
     * If false (default), JavaScript window.open() / target="_blank" links
     * are BLOCKED — no popup Chromium window pops up over your game. The
     * OnNewWindowRequested delegate fires so BP can listen and, e.g., call
     * LoadURL() to redirect to the same frame instead.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bAllowNewWindows = false;

    /**
     * If true (default), page JavaScript runs. Flip false to load pages as
     * inert documents (CSS still renders, but no scripts execute, no
     * fetch / XHR, no event handlers). Useful for displaying untrusted
     * HTML or rendering offline help pages where you want zero attack
     * surface.
     *
     * Applied per-navigation on iOS via
     * `WKWebpagePreferences.allowsContentJavaScript` (iOS 14+) — the new
     * Apple-recommended replacement for deprecated
     * `WKPreferences.javaScriptEnabled`. iOS-only today; Windows / Android
     * ignore this flag (their JS-enable knobs aren't wired yet).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Allow JavaScript (iOS)"))
    bool bAllowJavaScript = true;

    /**
     * If true, the WebView automatically retries plain-HTTP loads as HTTPS
     * when the destination host is known to support TLS. Behaves like an
     * inline HSTS upgrade for those requests. Defaults to false to match
     * WebKit's own default (and so existing http:// dev flows aren't
     * silently transformed). Maps to
     * `WKWebViewConfiguration.upgradeKnownHostsToHTTPS` (iOS 15+).
     *
     * iOS-only today — Windows / Android use different upgrade paths.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Upgrade HTTP→HTTPS for Known Hosts (iOS)"))
    bool bUpgradeHTTPToHTTPS = false;

    /**
     * If false (default), mixed-content (an https:// page fetching http://
     * subresources) is blocked. Mirrors the API 21+ Android default but is
     * set explicitly to be defensive against future framework changes.
     * Turn on only for dev flows where the page legitimately needs to mix
     * schemes (e.g. an https:// app loading from a plain-HTTP dev API).
     *
     * Android: WebSettings.setMixedContentMode(MIXED_CONTENT_NEVER_ALLOW
     * vs MIXED_CONTENT_ALWAYS_ALLOW). Windows / iOS have their own
     * mixed-content handling; this flag is currently Android-only.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Allow Mixed Content (http on https, Android)"))
    bool bAllowMixedContent = false;

    /**
     * If false (default), the WebView refuses to load file:// URLs. Game UI
     * served from a virtual host (https://<vhost>/...) never needs file://
     * — the vhost mapping serves bundled content via a real https origin.
     *
     * Defense-in-depth: even if lockdown is misconfigured or a redirect
     * slips through, the WebView won't open a local file. Matters because
     * file:// has historically had more permissive same-origin semantics
     * than http(s), so a malicious page reaching it gets broader access
     * than from any other scheme.
     *
     * Default behavior changed from API 30 (true → false) — we now set
     * it explicitly so the behavior is the same on every supported
     * Android version. Android-only flag.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Allow file:// URLs (Android)"))
    bool bAllowFileURLs = false;

    /**
     * If false (default), the WebView refuses to load content:// URIs.
     * content:// is Android's cross-app file-sharing scheme (images from
     * the gallery, files from other apps). Game UI doesn't need it; turn
     * on only if you intentionally embed media from a ContentProvider.
     *
     * Same defense-in-depth motivation as bAllowFileURLs — Android-only.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Allow content:// URIs (Android)"))
    bool bAllowContentURIs = false;

    /**
     * If true, the Android WebView is promoted to its own offscreen hardware
     * layer via `View.setLayerType(LAYER_TYPE_HARDWARE, null)`. Default false.
     *
     * This is a targeted workaround for a specific class of GPU-driver bug —
     * seen on the Pixel 10 series (Tensor G5 / Imagination PowerVR GPU,
     * Android 16) — where a TRANSPARENT WebView composited over the 3D scene
     * renders perfectly while static but corrupts on SCROLL: parts of the
     * page fail to recomposite and go transparent, letting the game scene
     * bleed through where content should be.
     *
     * Why it helps: by default the WebView tile-composites incrementally —
     * on scroll it shifts existing tiles and only re-blends the moved /
     * newly-exposed strips. The affected driver mishandles that incremental
     * transparent re-blend. Promoting to a hardware layer makes the WebView
     * re-render into one full-screen texture and blend the WHOLE thing each
     * frame — the same full-frame blend that already works when the page is
     * static — sidestepping the broken incremental path.
     *
     * Cost: one extra full-screen GPU texture (~view-size × 4 bytes, e.g.
     * ~16 MB at 1440p) and a per-frame re-render of that texture while the
     * page is animating. Negligible for a mostly-static menu / chat / HUD
     * overlay; measurable on a constantly-animating page. The texture is
     * VIEW-sized (not document-sized), so long scrollable pages don't blow
     * the GPU's max-texture cap.
     *
     * Leave false on devices that render correctly (Adreno / Mali, etc.) —
     * it's pure overhead there. Android-only; no-op on Windows / iOS.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Force Hardware Layer (Pixel/PowerVR scroll fix, Android)"))
    bool bForceHardwareLayer = false;

    /**
     * Auto-terminate the renderer when it has been unresponsive for this
     * many milliseconds. 0 disables auto-terminate (observe-only — the
     * OnRenderProcessUnresponsive / Responsive delegates still fire).
     *
     * "Unresponsive" means the renderer's main thread hasn't returned to
     * its event loop for ~5 seconds — typically an infinite JS loop, a
     * pathologically long synchronous task, or a wedged page. Different
     * from "renderer process died" (covered by bAutoRecoverOnProcessFailed
     * + the OnProcessFailed delegate); a hung renderer is alive but
     * frozen, dead to the user either way.
     *
     * Default 10000 ms: the page gets 10 seconds of stuck time before the
     * plugin kills the renderer, which then triggers the
     * OnRenderProcessGone path, which (if bAutoRecoverOnProcessFailed is
     * also true) reloads the page from scratch. End result: a hung page
     * self-heals after 10 seconds, looking identical to the user as a
     * normal F5 reload.
     *
     * Combined behavior:
     *   bAutoRecoverOnProcessFailed=true  + UnresponsiveTimeoutMs>0
     *     → hung pages auto-recover (recommended for shipping games)
     *   bAutoRecoverOnProcessFailed=true  + UnresponsiveTimeoutMs=0
     *     → only crashes auto-recover; hangs are observable but persist
     *   bAutoRecoverOnProcessFailed=false + UnresponsiveTimeoutMs>0
     *     → hangs get killed but no automatic re-create; the BP handler
     *       is responsible for recreating from the OnProcessFailed event
     *   bAutoRecoverOnProcessFailed=false + UnresponsiveTimeoutMs=0
     *     → full manual control via the BP delegates
     *
     * Android-only feature today. WebView2 (Windows) and WKWebView (iOS)
     * have no equivalent API; this field is a no-op on those platforms.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI",
              meta = (DisplayName = "Unresponsive Timeout (ms, Android)"))
    int32 UnresponsiveTimeoutMs = 10000;

    /**
     * If true (default), the plugin automatically recreates the native
     * WebView when its renderer process dies (out-of-memory kill, renderer
     * crash, etc.). The OnProcessFailed delegate still fires first so you
     * can react (telemetry, "reconnecting" toast), then a fresh native
     * WebView is initialized with the same Config and the page is reloaded
     * to the URL the user was last on (snapshotted from CachedURL just
     * before the old impl is destroyed — NOT InitialURL, so SPA deep
     * links survive the recreate).
     *
     * Reliable for typical web UI:
     *   • localStorage / cookies / IndexedDB survive (they live in the
     *     WebView's data dir, not the renderer process).
     *   • Your C++ / Blueprint delegate bindings on the UInoWebView are
     *     untouched (the UObject persists across the recreate).
     *   • The page reloads exactly like the user pressed F5.
     *
     * Lossy for:
     *   • Any state the page kept only in JavaScript variables (re-fetch
     *     on DOMContentLoaded — same pattern as a normal reload).
     *   • Live WebSocket / EventSource connections — the page must
     *     reconnect on load.
     *
     * Protected by a retry budget: at most 3 automatic recoveries within
     * any 60-second window. If the renderer keeps dying (a page that
     * consistently crashes the GPU, for example), the plugin stops
     * recreating and leaves the WebView dead so your code can decide what
     * to do — preventing an infinite recreate loop.
     *
     * Turn off only when you want full manual control of process-failure
     * recovery via your OnProcessFailed handler.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bAutoRecoverOnProcessFailed = true;

    // ── Initial-load state (headers / cookies) ──────────────────────────────

    /**
     * Extra HTTP headers attached to the INITIAL navigation only. Sub-resource
     * requests (images, scripts, fetch/XHR from the page) carry their normal
     * browser-built headers — same caveat as UInoWebView::LoadURLWithHeaders.
     *
     * Saves the create-then-call-LoadURLWithHeaders dance for SSO / auth
     * flows where you want the very first request to carry an
     * Authorization / X-Tenant / etc. header.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    TMap<FString, FString> InitialHeaders;

    /**
     * Cookies seeded into the WebView's cookie store BEFORE the initial
     * navigation. Useful for SSO and pre-authenticated sessions where the
     * server expects a session cookie on the very first request.
     *
     * Each entry is applied through the same path as
     * UInoWebView::SetCookie — see FInoInitialCookie for the field
     * semantics. Submission order is preserved.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    TArray<FInoInitialCookie> InitialCookies;
};
