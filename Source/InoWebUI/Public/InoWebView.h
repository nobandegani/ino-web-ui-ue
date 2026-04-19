// Copyright Inoksan. All Rights Reserved.

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

    /**
     * Parse the raw envelope JSON pushed by the impl's OnMessageReceivedJson
     * callback and broadcast OnMessageReceived to Blueprint subscribers.
     * Always runs on the game thread.
     */
    void DispatchIncomingEnvelope(const FString& EnvelopeJson);
};
