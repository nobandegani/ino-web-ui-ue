// Copyright Inoland. All Rights Reserved.

#include "IInoWebViewImpl.h"      // now a public header
#include "InoWebUILog.h"

#if PLATFORM_WINDOWS
#include "Windows/InoWebViewImpl_Windows.h"
#include "Windows/InoWebViewImpl_Windows_Composition.h"
#elif PLATFORM_ANDROID
#include "Android/InoWebViewImpl_Android.h"
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  CreateInoWebViewImpl — picks the right backend for the current platform
//  and (on Windows) the current run mode.
//
//  Windows has TWO impls:
//
//    FInoWebViewImpl_Windows               — child-HWND WebView2 controller.
//                                             Simple, battle-tested, used for
//                                             Standalone Game and packaged
//                                             builds. Breaks on PIE because
//                                             Slate's DWM flags confuse the
//                                             compositor (transparent pixels
//                                             show the desktop instead of
//                                             UE's swap chain).
//
//    FInoWebViewImpl_Windows_Composition   — DirectComposition visual hosting.
//                                             Used ONLY in PIE (GIsEditor ==
//                                             true). Sidesteps the DWM
//                                             child-HWND problem by running
//                                             the WebView as a visual in our
//                                             own top-level overlay window.
//
//  The PIE dispatch key is GIsEditor. GIsEditor is true only when the process
//  IS the editor. Standalone Game launched from the editor is a separate
//  process (GIsEditor=false); packaged builds have it false at link time. So
//  GIsEditor === "we're about to run inside PIE," which is exactly what we
//  want to gate on.
// ─────────────────────────────────────────────────────────────────────────────
TUniquePtr<IInoWebViewImpl> CreateInoWebViewImpl()
{
#if PLATFORM_WINDOWS
    if (GIsEditor)
    {
        UE_LOG(LogInoWebUI, Log,
            TEXT("CreateInoWebViewImpl: PIE detected — using composition-hosting impl."));
        return MakeUnique<FInoWebViewImpl_Windows_Composition>();
    }
    UE_LOG(LogInoWebUI, Verbose,
        TEXT("CreateInoWebViewImpl: using child-HWND controller impl."));
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
