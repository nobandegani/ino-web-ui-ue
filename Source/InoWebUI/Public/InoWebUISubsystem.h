// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "InoWebUITypes.h"
#include "InoWebUISubsystem.generated.h"

class UInoWebView;
class UInoWebBundle;

/**
 * UInoWebUISubsystem — entry point for all WebView overlay management.
 *
 * Created automatically with the UGameInstance; destroyed when the
 * GameInstance is shut down. Accessible from Blueprint via
 * "Get Ino Web UI Subsystem".
 *
 * Owns all live UInoWebView instances keyed by FName. The subsystem keeps
 * strong UPROPERTY references, so WebViews you create stay alive until you
 * explicitly DestroyWebView() them or the GameInstance tears down.
 *
 * Phase 1 API (Blueprint-callable):
 *   • CreateWebView(Name, Config)   — spawn a new native overlay
 *   • GetWebView(Name)              — look up an existing overlay
 *   • DestroyWebView(Name)          — tear one down
 *
 * Later phases will add: PostMessage, OnMessageReceived delegates, etc.
 */
UCLASS()
class INOWEBUI_API UInoWebUISubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    //~ UGameInstanceSubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // ── Blueprint / C++ API ─────────────────────────────────────────────────

    /**
     * Create a new WebView overlay on the game viewport's window.
     *
     * @param Name   Identifier used for subsequent Get/Destroy lookups.
     *               Must be unique — creating a WebView with an existing
     *               name logs a warning and returns the existing instance.
     * @param Config Construction parameters (see FInoWebViewConfig).
     * @return       The new (or existing) WebView, or nullptr on failure.
     *
     * Failure cases: no game viewport yet, no top-level window, or an
     * unsupported platform. Check the LogInoWebUI output for details.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI",
              meta = (AutoCreateRefTerm = "Config"))
    UInoWebView* CreateWebView(FName Name, const FInoWebViewConfig& Config);

    /**
     * Create a WebView driven by a UInoWebBundle asset. Uses the asset's
     * InitialURL, VirtualHostName, and (depending on build):
     *
     *   • Editor / non-cooked: serves loose files directly from the asset's
     *     SourceFolder so React hot-iteration still works.
     *   • Packaged / cooked:   extracts Files[] to
     *     <ProjectSavedDir>/InoWebBundles/<AssetName>/ on first use (or
     *     when the content hash changes), then serves from there.
     *
     * Null Bundle → logs an error and returns nullptr.
     */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI",
              meta = (DisplayName = "Create Web View From Bundle"))
    UInoWebView* CreateWebViewFromAsset(FName Name, UInoWebBundle* Bundle);

    /** Returns nullptr if no WebView with that name exists. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    UInoWebView* GetWebView(FName Name) const;

    /** Tears down and forgets the WebView. No-op if the name isn't found. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void DestroyWebView(FName Name);

    /** Tears down every WebView this subsystem owns. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void DestroyAllWebViews();

    /**
     * Push the current parent client rect to a single WebView.
     * Called by UInoWebView::SetBoundsAuto so the auto-resize pipeline
     * is re-engaged for that WebView immediately, without rebroadcasting
     * to siblings.
     */
    void BroadcastClientRectToOne(UInoWebView* View);

private:
    /** All live WebViews keyed by the name passed to CreateWebView. */
    UPROPERTY()
    TMap<FName, TObjectPtr<UInoWebView>> WebViews;

    /** Handle for our subscription to FViewport::ViewportResizedEvent. */
    FDelegateHandle ViewportResizedHandle;

    /**
     * Walks GameInstance → World → GameViewport → SWindow → GenericWindow
     * to resolve the OS-level window handle (HWND on Windows). Returns
     * nullptr if any step fails (e.g., viewport not created yet).
     *
     * Not const because UGameViewportClient::GetWindow() is itself non-const.
     */
    void* AcquireParentNativeHandle();

    /**
     * Returns the parent SWindow for this GameInstance's viewport, or an
     * invalid TSharedPtr if none is currently available. Used by the resize
     * handler to compute client-area size in physical pixels.
     */
    TSharedPtr<class SWindow> GetParentWindow();

    /** Called by FViewport::ViewportResizedEvent — pushes new bounds to every WebView. */
    void OnViewportResized(class FViewport* InViewport, uint32 Unused);

    /**
     * Pick the folder to serve for a given bundle. In editor/dev we prefer
     * the bundle's SourceFolder (loose files — React dev loop friendly);
     * in packaged builds (or if the source folder is missing) we extract
     * baked bytes to a persistent folder in ProjectSavedDir.
     */
    FString ResolveBundleContentFolder(UInoWebBundle* Bundle);

    /**
     * Pushes the parent window's current client-area size to every live
     * WebView. Called both on ViewportResizedEvent and once at WebView
     * creation (covers the case where the window resizes during async init).
     */
    void BroadcastClientRectToAll();
};
