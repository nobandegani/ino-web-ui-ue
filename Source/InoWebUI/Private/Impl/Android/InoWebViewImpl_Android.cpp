// Copyright Inoksan. All Rights Reserved.

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

        jclass Local = FAndroidApplication::FindJavaClass("com/inoksan/webui/InoWebViewAndroid");
        if (!Local)
        {
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

        if (!MCreate || !MDestroy || !MLoadURL || !MSetVisible || !MReload
            || !MSyncBounds || !MSetVirtualHost || !MSetupMessaging || !MPostMessage
            || !MConfigureLockdown || !MConfigureDialogs
            || !MFocusWebView || !MSetZoomFactor || !MClearAllCookies
            || !MSetDevToolsEnabled || !MExecuteJavaScript || !MSetUserAgent
            || !MSetContextMenusEnabled)
        {
            UE_LOG(LogInoWebUI, Error,
                TEXT("One or more InoWebViewAndroid methods not found — Java helper "
                     "out of sync with C++?"));
            return false;
        }

        UE_LOG(LogInoWebUI, Log, TEXT("InoWebViewAndroid JNI bindings initialized."));
        return true;
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
        static_cast<jboolean>(Config.bTransparentBackground ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.bVisibleOnCreate        ? JNI_TRUE : JNI_FALSE));

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

        jstring JHost   = Env->NewStringUTF(TCHAR_TO_UTF8(*Config.VirtualHostName));
        jstring JFolder = Env->NewStringUTF(TCHAR_TO_UTF8(*AbsoluteFolder));
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
            jstring S = Env->NewStringUTF(TCHAR_TO_UTF8(*Config.AllowedURIPatterns[i]));
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

    // Step 2e: Phase 3 polish — dev tools, context menus, user agent.
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetDevToolsEnabled,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.bEnableDevTools ? JNI_TRUE : JNI_FALSE));

    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetContextMenusEnabled,
        static_cast<jint>(InstanceId),
        static_cast<jboolean>(Config.bEnableContextMenus ? JNI_TRUE : JNI_FALSE));

    if (!Config.UserAgentOverride.IsEmpty())
    {
        jstring JUA = Env->NewStringUTF(TCHAR_TO_UTF8(*Config.UserAgentOverride));
        Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MSetUserAgent,
            static_cast<jint>(InstanceId), JUA);
        Env->DeleteLocalRef(JUA);
    }

    // bStartMuted, bEnableAcceleratorKeys: no direct Android equivalents.
    //  • Accelerator keys (F5 etc.) don't exist on a touch device.
    //  • WebView has no mute API — SetMuted below is a no-op with a warning.

    // Step 3: navigate, if requested.
    if (!Config.InitialURL.IsEmpty())
    {
        jstring JUrl = Env->NewStringUTF(TCHAR_TO_UTF8(*Config.InitialURL));
        Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadURL,
            static_cast<jint>(InstanceId), JUrl);
        Env->DeleteLocalRef(JUrl);
    }

    // Android WebView construction itself runs on the UI thread (the Java
    // helper uses Activity.runOnUiThread). We mark ourselves ready immediately
    // because all subsequent operations also dispatch onto the same UI-thread
    // queue — they execute in submission order, after construction.
    bReady = true;

    UE_LOG(LogInoWebUI, Log,
        TEXT("FInoWebViewImpl_Android[%d] Initialize  url='%s'  transparent=%d  visible=%d"),
        InstanceId, *Config.InitialURL,
        Config.bTransparentBackground ? 1 : 0, Config.bVisibleOnCreate ? 1 : 0);
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

    jstring JUrl = Env->NewStringUTF(TCHAR_TO_UTF8(*URL));
    Env->CallStaticVoidMethod(InoWebUIJNI::JavaClass, InoWebUIJNI::MLoadURL,
        static_cast<jint>(InstanceId), JUrl);
    Env->DeleteLocalRef(JUrl);
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

    jstring JJson = Env->NewStringUTF(TCHAR_TO_UTF8(*Json));
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

static FString JStringToFString(JNIEnv* Env, jstring JStr)
{
    if (!JStr) return FString();
    const char* Chars = Env->GetStringUTFChars(JStr, nullptr);
    FString Result = UTF8_TO_TCHAR(Chars);
    Env->ReleaseStringUTFChars(JStr, Chars);
    return Result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnMessageReceived(
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
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnNavigationStarting(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    DispatchOnGameThread(static_cast<int32>(Id), [URI](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnNavigationStartingCallback) Impl->OnNavigationStartingCallback(URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnNavigationCompleted(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jboolean Success, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    const bool bSuccess = (Success == JNI_TRUE);
    DispatchOnGameThread(static_cast<int32>(Id), [bSuccess, URI](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnNavigationCompletedCallback) Impl->OnNavigationCompletedCallback(bSuccess, URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnDocumentTitleChanged(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JTitle)
{
    const FString Title = JStringToFString(Env, JTitle);
    DispatchOnGameThread(static_cast<int32>(Id), [Title](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnDocumentTitleChangedCallback) Impl->OnDocumentTitleChangedCallback(Title);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnScriptDialog(
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
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnNewWindowRequested(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JUri)
{
    const FString URI = JStringToFString(Env, JUri);
    DispatchOnGameThread(static_cast<int32>(Id), [URI](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnNewWindowRequestedCallback) Impl->OnNewWindowRequestedCallback(URI);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnGotFocus(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnGotFocusCallback) Impl->OnGotFocusCallback();
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnLostFocus(
    JNIEnv* /*Env*/, jclass /*Cls*/, jint Id)
{
    DispatchOnGameThread(static_cast<int32>(Id), [](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnLostFocusCallback) Impl->OnLostFocusCallback();
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_inoksan_webui_InoWebViewAndroid_nativeOnProcessFailed(
    JNIEnv* Env, jclass /*Cls*/, jint Id, jstring JDescription)
{
    const FString Description = JStringToFString(Env, JDescription);
    DispatchOnGameThread(static_cast<int32>(Id), [Description](FInoWebViewImpl_Android* Impl)
    {
        if (Impl->OnProcessFailedCallback) Impl->OnProcessFailedCallback(Description);
    });
}

void FInoWebViewImpl_Android::OpenDevTools()
{
    // Android WebView has no programmatic DevTools window. Remote debugging
    // is the equivalent: set WebContentsDebuggingEnabled (done at Initialize
    // via Config.bEnableDevTools) and connect chrome://inspect from a
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

    jstring JCode = Env->NewStringUTF(TCHAR_TO_UTF8(*Code));
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

#endif // PLATFORM_ANDROID
