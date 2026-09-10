// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

public class InoWebUIEditor : ModuleRules
{
    public InoWebUIEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "UnrealEd",      // UFactory, FAssetTypeActions_Base, reimport plumbing
            "AssetTools",    // IAssetTools / FAssetToolsModule
            "EditorStyle",   // icons / brushes for the content browser
            "InputCore",     // selectable commands / hotkeys (used by reimport)
            "InoWebUI",      // UInoWebBundle lives here once Phase 7 step 2 lands
        });
    }
}
