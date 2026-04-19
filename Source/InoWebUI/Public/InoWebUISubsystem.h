// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "InoWebUITypes.h"
#include "InoWebUISubsystem.generated.h"

class UInoWebView;

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

    /** Returns nullptr if no WebView with that name exists. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ino|WebUI")
    UInoWebView* GetWebView(FName Name) const;

    /** Tears down and forgets the WebView. No-op if the name isn't found. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void DestroyWebView(FName Name);

    /** Tears down every WebView this subsystem owns. */
    UFUNCTION(BlueprintCallable, Category = "Ino|WebUI")
    void DestroyAllWebViews();

private:
    /** All live WebViews keyed by the name passed to CreateWebView. */
    UPROPERTY()
    TMap<FName, TObjectPtr<UInoWebView>> WebViews;

    /**
     * Walks GameInstance → World → GameViewport → SWindow → GenericWindow
     * to resolve the OS-level window handle (HWND on Windows). Returns
     * nullptr if any step fails (e.g., viewport not created yet).
     */
    void* AcquireParentNativeHandle() const;
};
