// Copyright Inoland. All Rights Reserved.
//
// Module-private precompiled header — required by InoWebUI.Build.cs's iOS
// path. iOS enables Obj-C ARC (bEnableObjCAutomaticReferenceCounting = true)
// for this module's .mm files, which means the PCH it consumes must ALSO
// have been compiled with ARC enabled. The engine's project-wide shared
// PCH (SharedPCH.Slate.Project...) is built WITHOUT ARC, so we can't use
// it on iOS — clang refuses with:
//
//   "Objective-C automated reference counting was disabled in precompiled
//    file 'SharedPCH....gch' but is currently enabled"
//
// SetupIOS in Build.cs therefore sets PCHUsage = NoSharedPCHs and points
// PrivatePCHHeaderFile at this file. UBT compiles it with the module's
// flags (including ARC), and every .cpp / .mm in the module consumes a
// PCH compiled under the same rules.
//
// Win64 / Android still use the engine shared PCH per the constructor's
// UseExplicitOrSharedPCHs default — they don't touch PrivatePCHHeaderFile
// and fall back to the shared PCH unchanged.
//
// Keep this file minimal: just the common CoreMinimal include. WebKit /
// UIKit / Foundation are platform-specific and pulled in only by the
// .mm that actually uses them, so they don't belong in the PCH.

#pragma once

#include "CoreMinimal.h"
