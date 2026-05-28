// Copyright Inoland. All Rights Reserved.

#include "InoWebViewImpl_Android.h"

#if PLATFORM_ANDROID

#include "InoWebUILog.h"
#include "Android/AndroidApplication.h"
#include "Android/AndroidJavaEnv.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/CriticalSection.h"
#include "Async/Async.h"
#include "Misc/Paths.h"

// Process-unique instance id generator. Crosses JNI as a plain jint and keys
// the Java-side SparseArray<WebView>. Atomic so CreateWebView is safe if ever
// called off the game thread in the future.
static FThreadSafeCounter GInstanceIdGenerator(0);

// ─────────────────────────────────────────────────────────────────────────────
//  Impl registry — lets JNI callbacks from arbitrary threads find the right
//  C++ impl by ID. Registered in Initialize, unregistered in Shutdown. Held
//  under a lock because JNI may call in from any thread.
// ─────────────────────────────────────────────────────────────────────────────
static FCriticalSection                           GRegistryLock;
static TMap<int32, class FInoWebViewImpl_Android*> GImplRegistry;

// ─────────────────────────────────────────────────────────────────────────────
//  Cached JNI handles (one-time init, process-wide).
// ─────────────────────────────────────────────────────────────────────────────
namespace InoWebUIJNI
{
    static bool      bInitAttempted = false;
    static jclass    JavaClass      = nullptr;    // global ref; freed on module unload
    static jmethodID MCreate         = nullptr;
    static jmethodID MDestroy        = nullptr;
    static jmethodID MLoadURL        = nullptr;
    static jmethodID MSetVisible     = nullptr;
    static jmethodID MReload         = nullptr;
    static jmethodID MSyncBounds     = nullptr;
    static jmethodID MSetVirtualHost     = nullptr;
    static jmethodID MSetupMessaging     = nullptr;
    static jmethodID MPostMessage        = nullptr;
    static jmethodID MConfigureLockdown  = nullptr;
    static jmethodID MConfigureDialogs   = nullptr;
    static jmethodID MFocusWebView       = nullptr;
    static jmethodID MSetZoomFactor      = nullptr;
    static jmethodID MClearAllCookies    = nullptr;
    static jmethodID MSetDevToolsEnabled = nullptr;
    static jmethodID MExecuteJavaScript  = nullptr;
    static jmethodID MSetUserAgent       = nullptr;
    static jmethodID MSetContextMenusEnabled = nullptr;
    static jmethodID MSetBackgroundOpaque    = nullptr;
    static jmethodID MSetAllowMediaAutoplay  = nullptr;
    static jmethodID MSetAllowZoom           = nullptr;
    static jmethodID MSetShowScrollBars      = nullptr;
    // ── New ops ───────────────────────────────────────────────────────────
    static jmethodID MGoBack             = nullptr;
    static jmethodID MGoForward          = nullptr;
    static jmethodID MStopLoading        = nullptr;
    static jmethodID MLoadHTMLString     = nullptr;
    static jmethodID MSetCookie          = nullptr;
    static jmethodID MClearAllData       = nullptr;
    static jmethodID MCapturePreview     = nullptr;
    static jmethodID MLoadURLWithHeaders = nullptr;
    static jmethodID MSetBoundsMode      = nullptr;
    static jmethodID MConfigureUnresponsiveTimeout = nullptr;
    static jmethodID MConfigureSecurity  = nullptr;

    /**
     * Look up the Java helper class and all the static methods we call.
     * Returns true on success. Cached — subsequent calls are a no-op.
     */
    static bool Init()
    {
        if (bInitAttempted)
        {
            return JavaClass != nullptr;
        }
        bInitAttempted = true;

        JNIEnv* Env = FAndroidApplication::GetJavaEnv();
        if (!Env)
        {
            UE_LOG(LogInoWebUI, Error, TEXT("FAndroidApplication::GetJavaEnv returned null."));
            return false;
        }

        jclass Local = FAndroidApplication::FindJavaClass("net/inoland/webui/InoWebViewAndroid");
        if (!Local)
        {
            // A failed FindJavaClass leaves a ClassNotFoundException pending on
            // the JNIEnv. The next JNI call that checks pending exceptions
            // (CheckJNI in debug, or any throwing JNI fn) aborts the VM. Clear
            // it before we bail so the caller can recover (e.g. log and skip
            // WebView features instead of crashing the whole process).
            if (Env->ExceptionCheck()) { Env->ExceptionDescribe(); Env->ExceptionClear(); }
            UE_LOG(LogInoWebUI, Error,
                TEXT("InoWebViewAndroid Java class not found — check UPL inclusion "
                     "and ProGuard rules."));
            return false;
        }
        JavaClass = (jclass)Env->NewGlobalRef(Local);
        Env->DeleteLocalRef(Local);

        MCreate         = Env->GetStaticMethodID(JavaClass, "createWebView",   "(IZZ)V");
        MDestroy        = Env->GetStaticMethodID(JavaClass, "destroyWebView",  "(I)V");
        MLoadURL        = Env->GetStaticMethodID(JavaClass, "loadURL",         "(ILjava/lang/String;)V");
        MSetVisible     = Env->GetStaticMethodID(JavaClass, "setVisible",      "(IZ)V");
        MReload         = Env->GetStaticMethodID(JavaClass, "reload",          "(I)V");
        MSyncBounds     = Env->GetStaticMethodID(JavaClass, "syncBounds",      "(IIIII)V");
        MSetVirtualHost    = Env->GetStaticMethodID(JavaClass, "setVirtualHost",     "(ILjava/lang/String;Ljava/lang/String;)V");
        MSetupMessaging    = Env->GetStaticMethodID(JavaClass, "setupMessaging",     "(I)V");
        MPostMessage       = Env->GetStaticMethodID(JavaClass, "postMessageJson",    "(ILjava/lang/String;)V");
        MConfigureLockdown = Env->GetStaticMethodID(JavaClass, "configureLockdown",  "(IZ[Ljava/lang/String;)V");
        MConfigureDialogs  = Env->GetStaticMethodID(JavaClass, "configureDialogs",   "(IZZ)V");
        MFocusWebView           = Env->GetStaticMethodID(JavaClass, "focusWebView",          "(I)V");
        MSetZoomFactor          = Env->GetStaticMethodID(JavaClass, "setZoomFactor",         "(IF)V");
        MClearAllCookies        = Env->GetStaticMethodID(JavaClass, "clearAllCookies",       "(I)V");
        MSetDevToolsEnabled     = Env->GetStaticMethodID(JavaClass, "setDevToolsEnabled",    "(IZ)V");
        MExecuteJavaScript      = Env->GetStaticMethodID(JavaClass, "executeJavaScript",     "(ILjava/lang/String;)V");
        MSetUserAgent           = Env->GetStaticMethodID(JavaClass, "setUserAgent",          "(ILjava/lang/String;)V");
        MSetContextMenusEnabled = Env->GetStaticMethodID(JavaClass, "setContextMenusEnabled","(IZ)V");
        MSetBackgroundOpaque    = Env->GetStaticMethodID(JavaClass, "setBackgroundOpaque",   "(IZ)V");
        MSetAllowMediaAutoplay  = Env->GetStaticMethodID(JavaClass, "setAllowMediaAutoplay", "(IZ)V");
        MSetAllowZoom           = Env->GetStaticMethodID(JavaClass, "setAllowZoom",          "(IZ)V");
        MSetShowScrollBars      = Env->GetStaticMethodID(JavaClass, "setShowScrollBars",     "(IZ)V");

        MGoBack             = Env->GetStaticMethodID(JavaClass, "goBack",             "(I)V");
        MGoForward          = Env->GetStaticMethodID(JavaClass, "goForward",          "(I)V");
        MStopLoading        = Env->GetStaticMethodID(JavaClass, "stopLoading",        "(I)V");
        MLoadHTMLString     = Env->GetStaticMethodID(JavaClass, "loadHTMLString",     "(ILjava/lang/String;Ljava/lang/String;)V");
        MSetCookie          = Env->GetStaticMethodID(JavaClass, "setCookie",          "(ILjava/lang/String;Ljava/lang/String;)V");
        MClearAllData       = Env->GetStaticMethodID(JavaClass, "clearAllData",       "(I)V");
        MCapturePreview     = Env->GetStaticMethodID(JavaClass, "capturePreview",     "(IILjava/lang/String;)V");
        MLoadURLWithHeaders = Env->GetStaticMethodID(JavaClass, "loadURLWithHeaders", "(ILjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V");
        MSetBoundsMode      = Env->GetStaticMethodID(JavaClass, "setBoundsMode",      "(IZ)V");
        MConfigureUnresponsiveTimeout = Env->GetStaticMethodID(JavaClass, "configureUnresponsiveTimeout", "(II)V");
        MConfigureSecurity            = Env->GetStaticMethodID(JavaClass, "configureSecurity",            "(IZZZ)V");

        // Any GetStaticMethodID miss above throws NoSuchMethodError, which
        // sticks to the JNIEnv. CheckJNI (or the next throwing JNI call) will
        // abort the process if we don't clear it. Check + clear before we
        // even look at the method-ID nulls — we want to report which method
        // was missing, not abort the VM.
        const bool bPendingException = Env->ExceptionCheck() != JNI_FALSE;
        if (bPendingException)
        {
            Env->ExceptionDescribe();  // dumps the exception to logcat
            Env->ExceptionClear();
        }
        if (bPendingException
            || !MCreate || !MDestroy || !MLoadURL || !MSetVisible || !MReload
            || !MSyncBounds || !MSetVirtualHost || !MSetupMessaging || !MPostMessage
            || !MConfigureLockdown || !MConfigureDialogs
            || !MFocusWebView || !MSetZoomFactor || !MClearAllCookies
            || !MSetDevToolsEnabled || !MExecuteJavaScript || !MSetUserAgent
            || !MSetContextMenusEnabled || !MSetBackgroundOpaque
            || !MSetAllowMediaAutoplay
            || !MSetAllowZoom || !MSetShowScrollBars
            || !MGoBack || !MGoForward || !MStopLoading || !MLoadHTMLString
            || !MSetCookie || !MClearAllData || !MCapturePreview
            || !MLoadURLWithHeaders || !MSetBoundsMode
            || !MConfigureUnresponsiveTimeout
            || !MConfigureSecurity)
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("One or more InoWebViewAndroid methods not found — Java helper "
                     "out of sync with C++? (pendingException=%s)"),
                bPendingException ? TEXT("true") : TEXT("false"));
            return false;
        }

        UE_LOG(LogInoWebUI, Log, TEXT("InoWebViewAndroid JNI bindings initialized."));
        return true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  JNI string conversion helpers (UTF-16 round-trip)
//
//  Why NOT NewStringUTF / GetStringUTFChars?
//
//    Those JNI functions use Java's "Modified UTF-8", a non-standard encoding
//    that differs from standard UTF-8 for U+0000 (encoded as 0xC0 0x80 to
//    keep C strings null-terminable) and for supplementary-plane characters
//    (≥ U+10000, encoded as a 6-byte surrogate-pair sequence instead of the
//    standard 4-byte sequence).
//
//    UE's TCHAR_TO_UTF8 / UTF8_TO_TCHAR macros produce / consume STANDARD
//    UTF-8. So a JSON payload, URL, page title — anything containing emoji
//    or characters outside the BMP — gets silently mangled when round-tripped
//    via the *UTF* JNI calls. Android's CheckJNI also aborts the VM if it
//    detects invalid Modified UTF-8.
//
//    The fix is to skip UTF-8 entirely on the JNI boundary: use the UTF-16
//    JNI functions (NewString / GetStringChars), which match jchar's native
//    encoding and have no ambiguity. UE's FUTF16ToTCHAR / FTCHARToUTF16
//    bridge UTF-16 ↔ TCHAR cleanly on every platform (TCHAR may be UTF-16
//    on Windows or UTF-32 on Android — the conversion class handles either).
// ─────────────────────────────────────────────────────────────────────────────

/** UE FString → Java jstring via UTF-16. Caller owns the returned local ref. */
static jstring FStringToJString(JNIEnv* Env, const FString& Str)
{
    if (!Env) return nullptr;
    if (Str.IsEmpty())
    {
        // Empty Java string — pass nullptr length 0 so we don't risk a null
        // deref on the input buffer for empty strings.
        const jchar Empty = 0;
        return Env->NewString(&Empty, 0);
    }
    FTCHARToUTF16 Conv(*Str, Str.Len());
    return Env->NewString(
        reinterpret_cast<const jchar*>(Conv.Get()),
        static_cast<jsize>(Conv.Length()));
}

/** Java jstring → UE FString via UTF-16. */
static FString JStringToFString(JNIEnv* Env, jstring JStr)
{
    if (!Env || !JStr) return FString();
    const jsize Len = Env->GetStringLength(JStr);
    if (Len <= 0) return FString();
    const jchar* Chars = Env->GetStringChars(JStr, nullptr);
    if (!Chars) return FString();
    FUTF16ToTCHAR Converted(
        reinterpret_cast<const UTF16CHAR*>(Chars),
        static_cast<int32>(Len));
    FString Result = FString::ConstructFromPtrSize(
        Converted.Get(), Converted.Length());
    Env->ReleaseStringChars(JStr, Chars);
    return Result;
}

/**
 * Warn the developer when they try to load a cleartext http:// URL.
 *
 * Since Android 9 (API 28 — our minSdk), the default NetworkSecurityConfig
 * blocks cleartext traffic. WebView surfaces the block as a silent navigation
 * failure with ERR_CLEARTEXT_NOT_PERMITTED in logcat — no error callback to
 * us, no visible signal in the UE log. The first-time setup confusion this
 * causes is a recurring footgun; we log a loud warning at the C++ layer so
 * the developer sees it without spelunking through logcat filters.
 *
 * The fix is project-side, not plugin-side: either declare
 * `android:usesCleartextTraffic="true"` on `<application>` in the manifest
 * (blunt — affects every URL the app loads) or ship a network_security_config
 * XML that allows cleartext for the specific hosts you control (per
 * https://developer.android.com/privacy-and-security/security-config).
 */
static void WarnIfCleartextHttpURL(const FString& URL, const TCHAR* CallSite)
{
    if (URL.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("%s: URL '%s' uses cleartext http:// — Android 28+ blocks "
                 "cleartext by default. This navigation will fail silently "
                 "with ERR_CLEARTEXT_NOT_PERMITTED unless your app declares "
                 "usesCleartextTraffic=\"true\" in the Android manifest or "
                 "ships a network_security_config allowing this host."),
            CallSite, *URL);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
FInoWebViewImpl_Android::FInoWebViewImpl_Android()
{
    InstanceId = GInstanceIdGenerator.Increment();
}

FInoWebViewImpl_Android::~FInoWebViewImpl_Android()
{
    Shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initialize
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_Android::Initialize(void* /*ParentNativeHandle*/,
                                         const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    if (!InoWebUIJNI::Init())
    {
        return false;
    }

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env) return false;

    // Register this impl in the global registry so JNI callbacks from Java
    // (e.g. nativeOnMessageReceived) can find us. Unregistered in Shutdown.
    {
        FScopeLock Lock(&GRegistryLock);
        GImplRegistry.Add(InstanceId, this);
    }

    // Step 1: create the WebView without navigating.
    Env->CallStaticVoidMethod(
        InoWebUIJNI::JavaClass, InoWebUIJNI::MCreate,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bTransparentBackground ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.View.bVisibleOnCreate        ? JNI_TRUE : JNI_FALSE));

    // Step 2: virtual host. Installs a WebViewClient.shouldInterceptRequest
    // handler that serves files from the resolved folder when the page
    // requests anything under https://<VirtualHostName>/. Must happen before
    // the first navigation so the initial URL hits the handler.
    if (!Config.VirtualHostName.IsEmpty() && !Config.VirtualHostFolder.IsEmpty())
    {
        // Resolve relative → absolute (ProjectContentDir-anchored), mirroring
        // the Windows impl's behavior.
        const FString AbsoluteFolder = FPaths::IsRelative(Config.VirtualHostFolder)
            ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Config.VirtualHostFolder)
            : FPaths::ConvertRelativePathToFull(Config.VirtualHostFolder);

        jstring JHost   = FStringToJString(Env, Config.VirtualHostName);
        jstring JFolder = FStringToJString(Env, AbsoluteFolder);
        Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetVirtualHost,
            static_cast<jint>(InstanceId), JHost, JFolder);
        Env->DeleteLocalRef(JHost);
        Env->DeleteLocalRef(JFolder);

        UE_LOG(LogInoWebUI, Log,
            TEXT("FInoWebViewImpl_Android[%d] virtual host:  https://%s/  ->  %s"),
            InstanceId, *Config.VirtualHostName, *AbsoluteFolder);
    }

    // Step 2b: messaging — expose the JS bridge object + flip the injection
    // flag so the unified WebViewClient injects window.InoWebUI at onPageStarted.
    // Unconditional — parity with Windows where the bridge is always available.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetupMessaging,
        static_cast<jint>(InstanceId));

    // Step 2c: lockdown — hand the Java client the allowlist so
    // shouldOverrideUrlLoading can cancel non-whitelisted nav.
    {
        jclass StringCls = Env->FindClass("java/lang/String");
        jobjectArray JPatterns = Env->NewObjectArray(
            Config.AllowedURIPatterns.Num(), StringCls, nullptr);
        for (int32 i = 0; i < Config.AllowedURIPatterns.Num(); ++i)
        {
            jstring S = FStringToJString(Env, Config.AllowedURIPatterns[i]);
            Env->SetObjectArrayElement(JPatterns, i, S);
            Env->DeleteLocalRef(S);
        }
        Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MConfigureLockdown,
            static_cast<jint>(InstanceId),
            static_cast<jboolean>(Config.bLockToVirtualHost ? JNI_TRUE : JNI_FALSE),
            JPatterns);
        Env->DeleteLocalRef(JPatterns);
        Env->DeleteLocalRef(StringCls);
    }

    // Step 2d: hardening — JS dialog suppression + window.open blocking.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MConfigureDialogs,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.bAllowScriptDialogs ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.bAllowNewWindows     ? JNI_TRUE : JNI_FALSE));

    // Step 2d.0: security — mixed content + file:// + content:// access.
    // All default off; explicit so behaviour matches across API levels.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MConfigureSecurity,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.bAllowMixedContent ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.bAllowFileURLs     ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.bAllowContentURIs  ? JNI_TRUE : JNI_FALSE));

    // Step 2d.1: unresponsive-renderer detection. Installs a
    // WebViewRenderProcessClient regardless of timeout value (so the
    // observability callbacks always fire), and stores the timeout for the
    // auto-terminate path. Timeout 0 = observe-only.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MConfigureUnresponsiveTimeout,
        static_cast<jint>(InstanceId),
        static_cast<jint>(Config.UnresponsiveTimeoutMs));

    // Step 2e: Phase 3 polish — dev tools, context menus, user agent.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetDevToolsEnabled,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bEnableDevTools ? JNI_TRUE : JNI_FALSE));

    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetContextMenusEnabled,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bEnableContextMenus ? JNI_TRUE : JNI_FALSE));

    // Newly exposed view setting (was hardcoded to allow before; default now
    // matches that behaviour). When false, Android requires a user gesture
    // before HTML5 audio / video can begin playback.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetAllowMediaAutoplay,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bAllowMediaAutoplay ? JNI_TRUE : JNI_FALSE));

    // Pinch / double-tap zoom control. Default false — game UI doesn't
    // want users zooming the page.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetAllowZoom,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bAllowZoom ? JNI_TRUE : JNI_FALSE));

    // Scroll-indicator visibility. Default false — clean overlay look.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetShowScrollBars,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.View.bShowScrollBars ? JNI_TRUE : JNI_FALSE));

    if (!Config.View.UserAgentOverride.IsEmpty())
    {
        jstring JUA = FStringToJString(Env, Config.View.UserAgentOverride);
        Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetUserAgent,
            static_cast<jint>(InstanceId), JUA);
        Env->DeleteLocalRef(JUA);
    }

    // bStartMuted, bEnableAcceleratorKeys: no direct Android equivalents.
    //  • Accelerator keys (F5 etc.) don't exist on a touch device.
    //  • WebView has no mute API — SetMuted below is a no-op with a warning.

    // Step 2f: initial cookies — applied before the initial loadURL so
    // they're in the cookie store when the first request fires. Java's
    // runOnUiThread queue serializes setCookie before loadURL.
    for (const FInoInitialCookie& InitCookie : Config.InitialCookies)
    {
        SetCookie(InitCookie.URL, InitCookie.Cookie);
    }

    // Step 3: navigate, if requested. Use loadUrlWithHeaders if any
    // InitialHeaders are configured; otherwise plain loadURL.
    if (!Config.InitialURL.IsEmpty())
    {
        WarnIfCleartextHttpURL(Config.InitialURL, TEXT("Initialize/InitialURL"));

        if (Config.InitialHeaders.Num() > 0)
        {
            LoadURLWithHeaders(Config.InitialURL, Config.InitialHeaders);
        }
        else
        {
            jstring JUrl = FStringToJString(Env, Config.InitialURL);
            Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadURL,
                static_cast<jint>(InstanceId), JUrl);
            Env->DeleteLocalRef(JUrl);
        }

        // Seed cached URL so GetURL() is meaningful even before the first
        // navigation completes.
        CachedURL = Config.InitialURL;
    }

    // Android WebView construction itself runs on the UI thread (the Java
    // helper uses Activity.runOnUiThread). We mark ourselves ready immediately
    // because all subsequent operations also dispatch onto the same UI-thread
    // queue — they execute in submission order, after construction.
    bReady = true;

    UE_LOG(LogInoWebUI, Log,
        TEXT("FInoWebViewImpl_Android[%d] Initialize  url='%s'  transparent=%d  visible=%d"),
        InstanceId, *Config.InitialURL,
        Config.View.bTransparentBackground ? 1 : 0, Config.View.bVisibleOnCreate ? 1 : 0);

    // Fire the one-shot ready signal. The owner (UInoWebView) wraps this
    // in AsyncTask(GameThread), so even though we're calling synchronously
    // during Initialize, the BP OnReady delegate will broadcast on the NEXT
    // game tick — giving the CreateWebView caller time to bind.
    if (OnReadyCallback)
    {
        OnReadyCallback();
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Navigation / visibility / bounds
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Android::Navigate(const FString& URL)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    WarnIfCleartextHttpURL(URL, TEXT("Navigate"));

    jstring JUrl = FStringToJString(Env, URL);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadURL,
        static_cast<jint>(InstanceId), JUrl);
    Env->DeleteLocalRef(JUrl);

    // Mirror Windows: navigation in flight means IsLoading() should report true.
    bCachedLoading = true;
}

void FInoWebViewImpl_Android::Reload()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MReload,
        static_cast<jint>(InstanceId));
}

void FInoWebViewImpl_Android::SetVisible(bool bVisible)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetVisible,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(bVisible ? JNI_TRUE : JNI_FALSE));
}

void FInoWebViewImpl_Android::SyncBounds(int32 ScreenX, int32 ScreenY,
                                         int32 Width, int32 Height)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    // Android treats X/Y as offsets from the content root's top-left; the
    // Windows impl treats them as screen coords (and ScreenToClients them).
    // For a full-screen Android game the two are equivalent. We pass the
    // values straight through; the Java helper clamps/falls-back sensibly.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSyncBounds,
        static_cast<jint>(InstanceId),
        static_cast<jint>(ScreenX),
        static_cast<jint>(ScreenY),
        static_cast<jint>(Width),
        static_cast<jint>(Height));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Android::Shutdown()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    bDestroyed = true;
    bReady     = false;

    // Unregister BEFORE destroying the Java WebView so no late JNI callback
    // can find us after this point. Acquire the lock to serialize with any
    // in-flight dispatch from the game-thread async task.
    {
        FScopeLock Lock(&GRegistryLock);
        GImplRegistry.Remove(InstanceId);
    }

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MDestroy,
        static_cast<jint>(InstanceId));

    UE_LOG(LogInoWebUI, Log, TEXT("FInoWebViewImpl_Android[%d] Shutdown"), InstanceId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 2+ APIs — not implemented in Android MVP.
//  Each is wrapped so callers get a clean warning instead of a no-op silence.
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Android::PostMessageJson(const FString& Json)
{
    check(IsInGameThread());
    if (bDestroyed) return;

    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    jstring JJson = FStringToJString(Env, Json);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MPostMessage,
        static_cast<jint>(InstanceId), JJson);
    Env->DeleteLocalRef(JJson);
}

// ─────────────────────────────────────────────────────────────────────────────
//  JNI callback from Java — fires when window.InoWebUI.send(...) is called
//  in the page. Runs on whichever thread WebView chose (NOT the game thread);
//  we marshal onto the game thread before dispatching to the UObject-layer
//  callback that UInoWebView wired up.
// ─────────────────────────────────────────────────────────────────────────────
// Helper for the common JNI "jstring → FString, marshal to game thread, look up
// impl under lock, call a provided lambda with the impl" pattern.
template <typename FLambda>
static void DispatchOnGameThread(int32 Id, FLambda&& Action)
{
    const int32 LocalId = Id;
    AsyncTask(ENamedThreads::GameThread,
        [LocalId, Action = Forward<FLambda>(Action)]() mutable
        {
            FScopeLock Lock(&GRegistryLock);
            if (FInoWebViewImpl_Android** Found = GImplRegistry.Find(LocalId))
            {
                if (FInoWebViewImpl_Android* Impl = *Found)
                {
                    Action(Impl);
                }
            }
        });
}

// JStringToFString / FStringToJString helpers are defined near the top of this
// file, right after InoWebUIJNI::Init(). They use the UTF-16 JNI functions so
// supplementary characters (emoji, ≥ U+10000) round-trip safely — see the
// big comment block there for why the UTF-8 JNI functions can't be used.

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnMessageReceived(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JEnvelope)
{
    if (!JEnvelope) return;
    const FString Envelope = JStringToFString(Env, JEnvelope);

    DispatchOnGameThread(static_cast<int32>(Id), [Envelope](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnMessageReceivedJson) Impl->OnMessageReceivedJson(Envelope);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnNavigationStarting(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    DispatchOnGameThread(static_cast<int32>(Id), [URI](FInoWebViewImpl_Android* Impl)
    {
        Impl->SetCachedLoading(true);
        if (Impl->OnNavigationStartingCallback) Impl->OnNavigationStartingCallback(URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnNavigationCompleted(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jboolean Success, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    const bool bSuccess = (Success == JNI_TRUE);
    DispatchOnGameThread(static_cast<int32>(Id), [bSuccess, URI](FInoWebViewImpl_Android* Impl)
    {
        Impl->SetCachedURL(URI);
        Impl->SetCachedLoading(false);
        if (Impl->OnNavigationCompletedCallback) Impl->OnNavigationCompletedCallback(bSuccess, URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnDocumentTitleChanged(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JTitle)
{
    const FString Title = JStringToFString(Env, JTitle);
    DispatchOnGameThread(static_cast<int32>(Id), [Title](FInoWebViewImpl_Android* Impl)
    {
        Impl->SetCachedTitle(Title);
        if (Impl->OnDocumentTitleChangedCallback) Impl->OnDocumentTitleChangedCallback(Title);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnNavStateChanged(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id, jboolean CanGoBack, jboolean CanGoForward)
{
    const bool bBack = (CanGoBack == JNI_TRUE);
    const bool bForward = (CanGoForward == JNI_TRUE);
    DispatchOnGameThread(static_cast<int32>(Id), [bBack, bForward](FInoWebViewImpl_Android* Impl)
    {
        Impl->SetCachedNavState(bBack, bForward);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnCapturePreviewComplete(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jboolean Success, jstring JFilePath)
{
    const FString FilePath = JStringToFString(Env, JFilePath);
    const bool bSuccess = (Success == JNI_TRUE);
    DispatchOnGameThread(static_cast<int32>(Id), [bSuccess, FilePath](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnCapturePreviewCompleteCallback)
        {
            Impl->OnCapturePreviewCompleteCallback(bSuccess, FilePath);
        }
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnScriptDialog(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jint Kind, jstring JMessage)
{
    const FString Message = JStringToFString(Env, JMessage);
    // Kind values match EInoScriptDialogKind (Alert/Confirm/Prompt/BeforeUnload
    // = 0/1/2/3) — see InoWebUITypes.h and the constants in the Java helper.
    const EInoScriptDialogKind K = static_cast<EInoScriptDialogKind>(Kind);
    DispatchOnGameThread(static_cast<int32>(Id), [K, Message](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnScriptDialogCallback) Impl->OnScriptDialogCallback(K, Message);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnNewWindowRequested(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    DispatchOnGameThread(static_cast<int32>(Id), [URI](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnNewWindowRequestedCallback) Impl->OnNewWindowRequestedCallback(URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnGotFocus(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnGotFocusCallback) Impl->OnGotFocusCallback();
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnLostFocus(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnLostFocusCallback) Impl->OnLostFocusCallback();
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnProcessFailed(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JDescription)
{
    const FString Description = JStringToFString(Env, JDescription);
    DispatchOnGameThread(static_cast<int32>(Id), [Description](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnProcessFailedCallback) Impl->OnProcessFailedCallback(Description);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnRenderProcessUnresponsive(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnRenderProcessUnresponsiveCallback)
        {
            Impl->OnRenderProcessUnresponsiveCallback();
        }
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnRenderProcessResponsive(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnRenderProcessResponsiveCallback)
        {
            Impl->OnRenderProcessResponsiveCallback();
        }
    });
}

extern "C" JNIEXPORT void JNICALL
Java_net_inoland_webui_InoWebViewAndroid_nativeOnConsoleMessage(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jint Level,
    jstring JMessage, jstring JSource, jint LineNumber)
{
    // Clamp Level to the EInoConsoleMessageLevel range (0..4). Anything
    // outside maps to Log so a misbehaving Java enum can't blow up the
    // C++ enum-cast.
    int32 SafeLevel = static_cast<int32>(Level);
    if (SafeLevel < 0 || SafeLevel > 4) SafeLevel = 1;
    const EInoConsoleMessageLevel LevelEnum =
        static_cast<EInoConsoleMessageLevel>(SafeLevel);

    const FString Message  = JStringToFString(Env, JMessage);
    const FString SourceID = JStringToFString(Env, JSource);
    const int32   Line     = static_cast<int32>(LineNumber);

    DispatchOnGameThread(static_cast<int32>(Id),
        [LevelEnum, Message, SourceID, Line](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnConsoleMessageCallback)
        {
            Impl->OnConsoleMessageCallback(LevelEnum, Message, SourceID, Line);
        }
    });
}

void FInoWebViewImpl_Android::OpenDevTools()
{
    // Android WebView has no programmatic DevTools window. Remote debugging
    // is the equivalent: set WebContentsDebuggingEnabled (done at Initialize
    // via Config.View.bEnableDevTools) and connect chrome://inspect from a
    // desktop Chrome on the same machine via adb.
    UE_LOG(LogInoWebUI, Log,
        TEXT("OpenDevTools on Android: connect this device to a desktop via USB, "
             "open chrome://inspect/#devices in desktop Chrome, and pick this "
             "WebView. (bEnableDevTools must be true on the config.)"));
}

void FInoWebViewImpl_Android::ExecuteJavaScript(const FString& Code)
{
    check(IsInGameThread());
    if (bDestroyed || Code.IsEmpty()) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    jstring JCode = FStringToJString(Env, Code);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MExecuteJavaScript,
        static_cast<jint>(InstanceId), JCode);
    Env->DeleteLocalRef(JCode);
}

void FInoWebViewImpl_Android::SetMuted(bool /*bMuted*/)
{
    // android.webkit.WebView has no audio-mute API. Options are:
    //   • Inject JS that mutes every <audio>/<video> element
    //   • Route the whole app through AudioManager.setStreamMute
    // Both are more surgery than "toggle a property" and affect scope beyond
    // the WebView. Leaving as a diagnostic no-op until a concrete use case
    // comes up.
    UE_LOG(LogInoWebUI, Warning,
        TEXT("SetMuted: not supported on android.webkit.WebView. "
             "Mute individual media elements via ExecuteJavaScript instead."));
}

void FInoWebViewImpl_Android::FocusWebView()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MFocusWebView,
        static_cast<jint>(InstanceId));
}

void FInoWebViewImpl_Android::SetZoomFactor(float Factor)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetZoomFactor,
        static_cast<jint>(InstanceId), static_cast<jfloat>(Factor));
}

void FInoWebViewImpl_Android::ClearAllCookies()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MClearAllCookies,
        static_cast<jint>(InstanceId));
}

void FInoWebViewImpl_Android::SetBackgroundOpaque(bool bOpaque)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetBackgroundOpaque,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(bOpaque ? JNI_TRUE : JNI_FALSE));
}

// ─────────────────────────────────────────────────────────────────────────────
//  New ops
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_Android::GoBack()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MGoBack,
        static_cast<jint>(InstanceId));
}

void FInoWebViewImpl_Android::GoForward()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MGoForward,
        static_cast<jint>(InstanceId));
}

bool FInoWebViewImpl_Android::CanGoBack() const
{
    // O(~1): updated whenever Java's onPageFinished/onReceivedError fires,
    // pushed via nativeOnNavStateChanged.
    if (!bReady || bDestroyed) return false;
    return bCachedCanGoBack;
}

bool FInoWebViewImpl_Android::CanGoForward() const
{
    if (!bReady || bDestroyed) return false;
    return bCachedCanGoForward;
}

void FInoWebViewImpl_Android::StopLoading()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MStopLoading,
        static_cast<jint>(InstanceId));
}

void FInoWebViewImpl_Android::LoadHTMLString(const FString& HTML, const FString& BaseURI)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    jstring JHtml = FStringToJString(Env, HTML);
    jstring JBase = BaseURI.IsEmpty() ? nullptr : FStringToJString(Env, BaseURI);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadHTMLString,
        static_cast<jint>(InstanceId), JHtml, JBase);
    Env->DeleteLocalRef(JHtml);
    if (JBase) Env->DeleteLocalRef(JBase);

    bCachedLoading = true;
}

void FInoWebViewImpl_Android::SetCookie(const FString& URL, const FString& Cookie)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    jstring JUrl    = FStringToJString(Env, URL);
    jstring JCookie = FStringToJString(Env, Cookie);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetCookie,
        static_cast<jint>(InstanceId), JUrl, JCookie);
    Env->DeleteLocalRef(JUrl);
    Env->DeleteLocalRef(JCookie);
}

void FInoWebViewImpl_Android::ClearAllData()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MClearAllData,
        static_cast<jint>(InstanceId));
}

bool FInoWebViewImpl_Android::CapturePreview(EInoImageFormat Format, const FString& OutFilePath)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return false;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return false;

    jstring JPath = FStringToJString(Env, OutFilePath);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MCapturePreview,
        static_cast<jint>(InstanceId),
        static_cast<jint>(Format == EInoImageFormat::JPEG ? 1 : 0),
        JPath);
    Env->DeleteLocalRef(JPath);
    return true;
}

void FInoWebViewImpl_Android::SetBoundsMode(bool bManual)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetBoundsMode,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(bManual ? JNI_TRUE : JNI_FALSE));
}

void FInoWebViewImpl_Android::LoadURLWithHeaders(const FString& URL,
                                                  const TMap<FString, FString>& Headers)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    JNIEnv* Env = FAndroidApplication::GetJavaEnv();
    if (!Env || !InoWebUIJNI::JavaClass) return;

    WarnIfCleartextHttpURL(URL, TEXT("LoadURLWithHeaders"));

    jclass StringCls = Env->FindClass("java/lang/String");

    // Build parallel arrays of header names and values (simpler than HashMap
    // construction across JNI).
    const int32 N = Headers.Num();
    jobjectArray JNames  = Env->NewObjectArray(N, StringCls, nullptr);
    jobjectArray JValues = Env->NewObjectArray(N, StringCls, nullptr);

    int32 i = 0;
    for (const TPair<FString, FString>& KV : Headers)
    {
        jstring JN = FStringToJString(Env, KV.Key);
        jstring JV = FStringToJString(Env, KV.Value);
        Env->SetObjectArrayElement(JNames,  i, JN);
        Env->SetObjectArrayElement(JValues, i, JV);
        Env->DeleteLocalRef(JN);
        Env->DeleteLocalRef(JV);
        ++i;
    }

    jstring JUrl = FStringToJString(Env, URL);
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadURLWithHeaders,
        static_cast<jint>(InstanceId), JUrl, JNames, JValues);
    Env->DeleteLocalRef(JUrl);
    Env->DeleteLocalRef(JNames);
    Env->DeleteLocalRef(JValues);
    Env->DeleteLocalRef(StringCls);

    bCachedLoading = true;
}

#endif // PLATFORM_ANDROID
