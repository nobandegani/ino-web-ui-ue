// Copyright Inoksan. All Rights Reserved.

#include "InoWebViewImpl_Android.h"

#if PLATFORM_ANDROID

#include "InoWebUILog.h"
#include "Android/AndroidApplication.h"
#include "Android/AndroidJavaEnv.h"
#include "HAL/ThreadSafeCounter.h"

// Process-unique instance id generator. Crosses JNI as a plain jint and keys
// the Java-side SparseArray<WebView>. Atomic so CreateWebView is safe if ever
// called off the game thread in the future.
static FThreadSafeCounter GInstanceIdGenerator(0);

// ─────────────────────────────────────────────────────────────────────────────
//  Cached JNI handles (one-time init, process-wide).
// ─────────────────────────────────────────────────────────────────────────────
namespace InoWebUIJNI
{
    static bool      bInitAttempted = false;
    static jclass    JavaClass      = nullptr;    // global ref; freed on module unload
    static jmethodID MCreate        = nullptr;
    static jmethodID MDestroy       = nullptr;
    static jmethodID MLoadURL       = nullptr;
    static jmethodID MSetVisible    = nullptr;
    static jmethodID MReload        = nullptr;
    static jmethodID MSyncBounds    = nullptr;

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

        MCreate     = Env->GetStaticMethodID(JavaClass, "createWebView",  "(ILjava/lang/String;ZZ)V");
        MDestroy    = Env->GetStaticMethodID(JavaClass, "destroyWebView", "(I)V");
        MLoadURL    = Env->GetStaticMethodID(JavaClass, "loadURL",        "(ILjava/lang/String;)V");
        MSetVisible = Env->GetStaticMethodID(JavaClass, "setVisible",     "(IZ)V");
        MReload     = Env->GetStaticMethodID(JavaClass, "reload",         "(I)V");
        MSyncBounds = Env->GetStaticMethodID(JavaClass, "syncBounds",     "(IIIII)V");

        if (!MCreate || !MDestroy || !MLoadURL || !MSetVisible || !MReload || !MSyncBounds)
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

    jstring JUrl = nullptr;
    if (!Config.InitialURL.IsEmpty())
    {
        JUrl = Env->NewStringUTF(TCHAR_TO_UTF8(*Config.InitialURL));
    }

    Env->CallStaticVoidMethod(
        InoWebUIJNI::JavaClass, InoWebUIJNI::MCreate,
        static_cast<jint>(InstanceId),
        JUrl,
        static_cast<jboolean>(Config.bTransparentBackground ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(Config.bVisibleOnCreate        ? JNI_TRUE : JNI_FALSE));

    if (JUrl) Env->DeleteLocalRef(JUrl);

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
void FInoWebViewImpl_Android::PostMessageJson(const FString& /*Json*/)
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("PostMessage not implemented on Android MVP (Phase 6)."));
}

void FInoWebViewImpl_Android::OpenDevTools()
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("OpenDevTools not implemented on Android — use chrome://inspect from a "
             "connected desktop Chrome instead."));
}

void FInoWebViewImpl_Android::ExecuteJavaScript(const FString& /*Code*/)
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("ExecuteJavaScript not implemented on Android MVP (Phase 6)."));
}

void FInoWebViewImpl_Android::SetMuted(bool /*bMuted*/)
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("SetMuted not implemented on Android MVP (Phase 6)."));
}

void FInoWebViewImpl_Android::FocusWebView()
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("FocusWebView not implemented on Android MVP (Phase 6)."));
}

void FInoWebViewImpl_Android::SetZoomFactor(float /*Factor*/)
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("SetZoomFactor not implemented on Android MVP (Phase 6)."));
}

void FInoWebViewImpl_Android::ClearAllCookies()
{
    UE_LOG(LogInoWebUI, Warning,
        TEXT("ClearAllCookies not implemented on Android MVP (Phase 6)."));
}

#endif // PLATFORM_ANDROID
