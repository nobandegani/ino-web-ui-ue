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

    // ── Room to grow in later phases (do NOT add here yet — placeholders): ──
    //   FString InitialUserAgent;
    //   bool    bEnableDevTools;
    //   bool    bEnableDefaultContextMenus;
    //   bool    bAreBrowserAcceleratorKeysEnabled;
};
