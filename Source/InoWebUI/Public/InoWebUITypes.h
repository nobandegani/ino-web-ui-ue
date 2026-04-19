// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "InoWebUITypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
//  FInoWebViewConfig
//
//  Passed to UInoWebUISubsystem::CreateWebView. Describes how a single WebView
//  should be constructed. Designed as a struct (not N function arguments) so
//  that adding options in future phases never breaks existing call sites.
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
     * If true (default), the WebView renders with a fully transparent
     * background so the Unreal 3D scene shows through the HTML's empty areas.
     * If false, the WebView paints an opaque white background (standard web).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bTransparentBackground = true;

    /**
     * Whether the WebView is visible immediately on creation.
     * You can flip this later via UInoWebView::Show / Hide.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bVisibleOnCreate = true;

    /**
     * Subfolder under Saved/ where WebView2 persists user data
     * (cookies, localStorage, cache). One folder per WebView keeps state
     * isolated; shared folders let WebViews share a login session.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString UserDataSubfolder = TEXT("WebViewData");

    // ── Phase 3 — runtime polish & debugging ────────────────────────────────

    /**
     * Enable the Chromium DevTools panel. Open programmatically with
     * UInoWebView::OpenDevTools(), or with F12 if bEnableAcceleratorKeys
     * is also true. Usually enabled in dev builds, disabled in shipping.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bEnableDevTools = false;

    /**
     * If false, right-clicking inside the WebView does nothing (the browser
     * "Save image as…/Inspect element" menu is suppressed). Recommended
     * for game UI. Default false.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bEnableContextMenus = false;

    /**
     * If false, browser-level accelerator keys like F5 (reload), F12
     * (devtools), Ctrl+F (find in page), Ctrl+P (print) are suppressed.
     * Recommended for game UI so those keys remain available to the game.
     * Default false. NOTE: disabling accelerators also disables F12 as a
     * way to open DevTools — use OpenDevTools() programmatically instead.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bEnableAcceleratorKeys = false;

    /** Audio output from the page is muted on creation when true. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    bool bStartMuted = false;

    /**
     * When non-empty, overrides navigator.userAgent inside the WebView.
     * Useful for Unreal-specific page branches, e.g.:
     *   if (navigator.userAgent.includes('Unreal')) { ... }
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    FString UserAgentOverride;

    // ── Phase 4 — local content serving ─────────────────────────────────────

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

    // ── Phase 5 — hardening / lockdown ──────────────────────────────────────

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

    /**
     * Additional URIs allowed past the lockdown. Each entry is a UE wildcard
     * pattern (`*` = any chars, `?` = single char) matched against the full
     * navigation URI. Examples:
     *
     *   "https://*.api.company.com/*"    — any subdomain of api.company.com
     *   "http://localhost:*\/*"           — any localhost port (dev)
     *   "https://cdn.example.com/*"      — specific CDN
     *
     * Empty list + bLockToVirtualHost=true = only VirtualHostName is allowed.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoWebUI")
    TArray<FString> AllowedURIPatterns;
};
