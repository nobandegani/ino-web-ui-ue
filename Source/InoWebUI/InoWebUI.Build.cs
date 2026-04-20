// Copyright Inoksan. All Rights Reserved.

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
        });
    }
}
