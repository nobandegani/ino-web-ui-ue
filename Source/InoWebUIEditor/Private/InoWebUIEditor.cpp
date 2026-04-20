// Copyright Inoksan. All Rights Reserved.

#include "InoWebUIEditor.h"
#include "InoWebUILog.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "IAssetTypeActions.h"
#include "InoWebBundleActions.h"

#define LOCTEXT_NAMESPACE "FInoWebUIEditorModule"

IMPLEMENT_MODULE(FInoWebUIEditorModule, InoWebUIEditor)

void FInoWebUIEditorModule::StartupModule()
{
    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    // Register a dedicated "Ino" top-level category in the Content Browser's
    // "New Asset" and filter menus. All our asset types will live here, so
    // we only need to register the category once.
    const EAssetTypeCategories::Type InoCategory =
        AssetTools.RegisterAdvancedAssetCategory(
            FName(TEXT("Ino")),
            LOCTEXT("InoCategory", "Ino"));

    // Register our asset type(s).
    TSharedRef<IAssetTypeActions> BundleActions =
        MakeShared<FInoWebBundleAssetTypeActions>(InoCategory);
    AssetTools.RegisterAssetTypeActions(BundleActions);
    RegisteredActions.Add(BundleActions);

    UE_LOG(LogInoWebUI, Log, TEXT("InoWebUIEditor module started (registered %d asset type(s))."),
        RegisteredActions.Num());
}

void FInoWebUIEditorModule::ShutdownModule()
{
    // Unregister anything we added — safe if the list is empty.
    if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
    {
        IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
        for (const TSharedPtr<IAssetTypeActions>& Action : RegisteredActions)
        {
            if (Action.IsValid())
            {
                AssetTools.UnregisterAssetTypeActions(Action.ToSharedRef());
            }
        }
    }
    RegisteredActions.Empty();

    UE_LOG(LogInoWebUI, Log, TEXT("InoWebUIEditor module shut down."));
}

#undef LOCTEXT_NAMESPACE
