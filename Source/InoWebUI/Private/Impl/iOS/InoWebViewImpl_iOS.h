// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IInoWebViewImpl.h"

#if PLATFORM_IOS

/**
 * FInoWebViewImpl_iOS — iOS implementation of IInoWebViewImpl backed by WKWebView.
 *
 * Threading model
 * ───────────────
 * UE's game thread is NOT the iOS main thread. WKWebView is main-thread-only.
 *  • Public C++ entry points run on the game thread (asserted via
 *    check(IsInGameThread())).
 *  • Each entry point marshals its real work onto the iOS main thread via
 *    dispatch_async(dispatch_get_main_queue(), ^{ ... }) inside the .mm.
 *  • WKNavigationDelegate / WKUIDelegate / WKScriptMessageHandler callbacks
 *    fire on main; the .mm marshals back to the game thread via
 *    AsyncTask(ENamedThreads::GameThread, ...) before invoking the
 *    OnXxxCallback slots set up by UInoWebView.
 *
 * Ready signal
 * ────────────
 * Same shape as Android: WKWebView's [alloc init] returns synchronously on
 * main, so we can mark ourselves ready as soon as Initialize finishes its
 * main-thread block. The OnReady BP delegate is deferred to the next game
 * tick by the UInoWebView wrapper, so binding right after CreateWebView
 * still works.
 *
 * Pimpl boundary
 * ──────────────
 * THIS HEADER MUST NOT INCLUDE WEBKIT/UIKIT. All Objective-C types live in
 * InoWebViewImpl_iOS.mm, hidden behind a void* InternalPtr to the FInternal
 * struct (analogous to the Windows impl's Internal). Any consumer of this
 * header — the factory, the UObject layer — only sees a clean C++ class.
 *
 * Virtual host
 * ────────────
 * WKWebView has no SetVirtualHostNameToFolderMapping equivalent. We use a
 * WKURLSchemeHandler registered on the custom scheme "inoweb". The user's
 * Config.VirtualHostName becomes the host of inoweb://<host>/<path>. See
 * the .mm header doc for the full divergence note.
 */
class FInoWebViewImpl_iOS : public IInoWebViewImpl
{
public:
    FInoWebViewImpl_iOS();
    virtual ~FInoWebViewImpl_iOS();

    //~ IInoWebViewImpl — implemented
    virtual bool Initialize(void* ParentNativeHandle, const FInoWebViewConfig& Config) override;
    virtual bool IsReady() const override { return bReady; }
    virtual void Navigate(const FString& URL) override;
    virtual void Reload() override;
    virtual void SetVisible(bool bVisible) override;
    virtual void SyncBounds(int32 ScreenX, int32 ScreenY, int32 Width, int32 Height) override;
    virtual void Shutdown() override;

    //~ IInoWebViewImpl — Phase 2+
    virtual void  PostMessageJson(const FString& Json) override;
    virtual void  OpenDevTools() override;
    virtual void  ExecuteJavaScript(const FString& Code) override;
    virtual void  SetMuted(bool bMuted) override;
    virtual void  FocusWebView() override;
    virtual void  SetZoomFactor(float Factor) override;
    virtual float GetZoomFactor() const override { return CachedZoomFactor; }
    virtual void  ClearAllCookies() override;
    virtual void  SetBackgroundOpaque(bool bOpaque) override;

    // ── Browser-style nav / state / capture / data wipe / headers ───────────
    virtual void GoBack() override;
    virtual void GoForward() override;
    virtual bool CanGoBack() const override;
    virtual bool CanGoForward() const override;
    virtual void StopLoading() override;
    virtual void LoadHTMLString(const FString& HTML, const FString& BaseURI) override;
    virtual void SetCookie(const FString& URL, const FString& Cookie) override;
    virtual void ClearAllData() override;
    virtual bool CapturePreview(EInoImageFormat Format, const FString& OutFilePath) override;
    virtual void LoadURLWithHeaders(const FString& URL, const TMap<FString, FString>& Headers) override;
    virtual void SetBoundsMode(bool bManual) override;

    // ── Friends-of-impl mutators for delegate callbacks ────────────────────
    // The Objective-C delegate classes need to write the protected base-class
    // cache fields. Public setters scoped to the impl keep that path narrow
    // without making the cache fields fully public — same shape as Android.
    void SetCachedURL    (const FString& URL)         { CachedURL = URL; }
    void SetCachedTitle  (const FString& Title)       { CachedTitle = Title; }
    void SetCachedLoading(bool bLoading)              { bCachedLoading = bLoading; }
    void SetCachedNavState(bool bBack, bool bForward)
    {
        bCachedCanGoBack    = bBack;
        bCachedCanGoForward = bForward;
    }
    void SetCachedZoomFactor(float Factor)            { CachedZoomFactor = Factor; }

    // ── Lockdown helpers used by the Obj-C navigation delegate ─────────────
    bool ShouldAllowURI(const FString& URI) const;

    /**
     * If URL is `http(s)://<VirtualHostName>[...]`, rewrite to
     * `inoweb://<VirtualHostName>[...]` so the configured custom-scheme handler
     * can serve it. WKWebView refuses to intercept https, so the user's
     * cross-platform `https://<vhost>/index.html` config gets transparently
     * mapped to the iOS-only scheme. URLs whose host doesn't match the
     * configured VirtualHostName pass through unchanged.
     */
    FString RewriteForVHost(const FString& URL) const;

    // ── Hardening flags read by the Obj-C UI delegate ──────────────────────
    // Public because the Obj-C bridge class lives in the same .mm and acts
    // as an extension of this impl. Set once during Initialize, read on
    // dialog/popup callbacks.
    bool bAllowScriptDialogs = false;
    bool bAllowNewWindows    = false;
    /** Snapshotted from FInoWebViewConfig::bAllowJavaScript. Read by the
     *  navigation delegate's decidePolicyForNavigationAction:preferences:
     *  variant (iOS 13+) and pushed into WKWebpagePreferences.allowsContentJavaScript
     *  on every navigation. True = JS runs (default); false = scripts inert. */
    bool bAllowJavaScript    = true;

private:
    /** Process-unique identifier — keys the Objective-C side's instance map
     *  so delegate callbacks can find us. Mirrors the Android InstanceId. */
    int32 InstanceId = 0;

    /** Opaque pointer to the FInternal struct defined inside the .mm. Holds
     *  WKWebView*, delegate objects, bounds-mode flag, etc. */
    void* InternalPtr = nullptr;

    /** Lockdown config copied from FInoWebViewConfig at Initialize time.
     *  Read by ShouldAllowURI on every nav decision. */
    bool            bLockToVirtualHost = true;
    FString         VirtualHostName;
    TArray<FString> AllowedURIPatterns;

    bool  bReady     = false;
    bool  bDestroyed = false;
    bool  bManualBounds = false;

    /** WKWebView has no native getter for zoom; we cache whatever the most
     *  recent SetZoomFactor pushed via JS (defaults to 1.0). */
    float CachedZoomFactor = 1.0f;
};

#endif // PLATFORM_IOS
