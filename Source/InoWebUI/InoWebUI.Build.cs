// Copyright Inoland. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class InoWebUI : ModuleRules
{
    public InoWebUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "ApplicationCore",      // FGenericWindow -> OS window handle (HWND)
            "Json",                 // UE <-> JS message envelope (de)serialization
            "JsonUtilities",        // FJsonObjectWrapper — BP-visible JSON objects
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            SetupWebView2(Target);
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            SetupAndroid(Target);
        }
        else if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            SetupIOS(Target);
        }
    }

    // ── iOS ──────────────────────────────────────────────────────────────────
    void SetupIOS(ReadOnlyTargetRules Target)
    {
        // Launch gives us [IOSAppDelegate GetDelegate] (RootView, IOSView, etc.).
        // ApplicationCore is already in the list above.
        PrivateDependencyModuleNames.Add("Launch");

        // System frameworks: WKWebView lives in WebKit; UIKit is needed for the
        // RootView, frame, snapshot UIImage, etc.; Foundation is implicit but
        // kept explicit for self-documentation.
        PublicFrameworks.AddRange(new string[]
        {
            "WebKit",
            "UIKit",
            "Foundation",
        });

        // Enable Objective-C Automatic Reference Counting for the .mm in this
        // module. UBT's default for game/plugin modules is MRR (manual retain
        // / release); the iOS impl is written assuming ARC semantics
        // (NSString*/UIView*/WKWebView* members of FInternal don't retain
        // explicitly). Module-scoped flag, doesn't affect any other code.
        bEnableObjCAutomaticReferenceCounting = true;

        // ARC must match the PCH it consumes. The project-wide shared PCH
        // (SharedPCH.Slate.Project...) is compiled WITHOUT ARC, so any module
        // that enables ARC and reuses that PCH fails with:
        //
        //   "Objective-C automated reference counting was disabled in
        //    precompiled file 'SharedPCH...gch' but is currently enabled"
        //
        // Force a module-private PCH on iOS so this module compiles its own
        // PCH with ARC enabled. Adds a small one-time compile cost
        // (a few seconds) and only on iOS — Win64 / Android still use the
        // shared PCH per the constructor's UseExplicitOrSharedPCHs default.
        //
        // NoSharedPCHs without an explicit PrivatePCHHeaderFile makes UBT
        // refuse to build the module ("must specify an explicit precompiled
        // header"). The PCH file is intentionally minimal — see its header
        // doc for what it does and doesn't include.
        PCHUsage              = PCHUsageMode.NoSharedPCHs;
        PrivatePCHHeaderFile  = "Private/InoWebUIPrivatePCH.h";

        // No UPL/IPL needed for the core WKWebView display path — Apple does
        // NOT require any Info.plist additions for inline WKWebView use.
        // (Network features like ATS exceptions remain the consumer project's
        // call, exactly like https requirements on the other platforms.)
    }

    // ── Android (MVP) ─────────────────────────────────────────────────────────
    void SetupAndroid(ReadOnlyTargetRules Target)
    {
        // Launch gives us FAndroidApplication::GetJavaEnv / FindJavaClass /
        // GetGameActivityThis. ApplicationCore is already in the list above.
        PrivateDependencyModuleNames.Add("Launch");

        // Register the Unreal Plugin Language file. UBT reads this during the
        // Android build to: copy our Java helper into the APK, add the
        // INTERNET permission to AndroidManifest.xml, and inject lifecycle
        // forwarders into GameActivity.java.
        string RelativeModulePath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
        AdditionalPropertiesForReceipt.Add(
            "AndroidPlugin",
            Path.Combine(RelativeModulePath, "InoWebUI_UPL.xml"));
    }

    // ── WebView2 (Windows only) ───────────────────────────────────────────────
    void SetupWebView2(ReadOnlyTargetRules Target)
    {
        string SDKRoot   = Path.Combine(PluginDirectory, "Source", "ThirdParty", "WebView2");
        string IncPath   = Path.Combine(SDKRoot, "include");
        string StaticLib = Path.Combine(SDKRoot, "lib", "Win64", "WebView2LoaderStatic.lib");

        if (!File.Exists(StaticLib))
        {
            throw new BuildException(
                "\n" +
                "WebView2 SDK not found. Run this script first:\n" +
                "  Plugins/InoWebUI/Scripts/AcquireWebView2SDK.ps1\n\n" +
                "Expected file: " + StaticLib + "\n");
        }

        // Third-party headers — suppress warnings
        PublicSystemIncludePaths.Add(IncPath);

        // Static loader shim: links into our binary; no extra DLL to deploy.
        // The shim locates the system Edge/WebView2 runtime at runtime.
        PublicAdditionalLibraries.Add(StaticLib);

        // System libraries required by WebView2LoaderStatic.lib
        PublicSystemLibraries.AddRange(new string[]
        {
            "shlwapi.lib",      // Shell lightweight API (path, registry helpers)
            "version.lib",      // Version info (used by the loader to find Edge)
            "ole32.lib",        // COM (CoCreateInstance, CoInitialize...)
            // DirectComposition — used ONLY by the PIE-only composition-hosting
            // impl (FInoWebViewImpl_Windows_Composition). The standalone /
            // packaged path never loads this code. Ships on Win8+ so no new
            // minimum platform requirement beyond WebView2's own Win10+.
            "dcomp.lib",
        });
    }
}
