// Copyright Inoksan. All Rights Reserved.

#include "IInoWebViewImpl.h"
#include "InoWebUILog.h"

#if PLATFORM_WINDOWS
#include "Windows/InoWebViewImpl_Windows.h"
#endif

TUniquePtr<IInoWebViewImpl> CreateInoWebViewImpl()
{
#if PLATFORM_WINDOWS
    return MakeUnique<FInoWebViewImpl_Windows>();
#else
    UE_LOG(LogInoWebUI, Warning,
        TEXT("CreateInoWebViewImpl: no implementation for this platform. "
             "WebView operations will be no-ops."));
    return nullptr;
#endif
}
