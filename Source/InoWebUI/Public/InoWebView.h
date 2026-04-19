// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "InoWebUITypes.h"
#include "InoWebView.generated.h"

class IInoWebViewImpl;

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
UCLASS(BlueprintType, Transient, NotBlueprintable)
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

    // ── State queries ───────────────────────────────────────────────────────

    /** True once the native WebView has finished its async construction. */
    UFUNCTION(BlueprintPure, Category = "Ino|WebUI")
    bool IsReady() const;

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
};
