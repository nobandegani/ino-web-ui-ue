// Copyright Inoland. All Rights Reserved.

#include "IInoWebViewImpl.h"      // now a public header
#include "InoWebUILog.h"

#if PLATFORM_WINDOWS
#include "Windows/InoWebViewImpl_Windows.h"
#elif PLATFORM_ANDROID
#include "Android/InoWebViewImpl_Android.h"
#endif

TUniquePtr<IInoWebViewImpl> CreateInoWebViewImpl()
{
#if PLATFORM_WINDOWS
    return MakeUnique<FInoWebViewImpl_Windows>();
#elif PLATFORM_ANDROID
    return MakeUnique<FInoWebViewImpl_Android>();
#else
    UE_LOG(LogInoWebUI, Warning,
        TEXT("CreateInoWebViewImpl: no implementation for this platform. "
             "WebView operations will be no-ops."));
    return nullptr;
#endif
}
