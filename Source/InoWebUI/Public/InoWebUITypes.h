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
};
