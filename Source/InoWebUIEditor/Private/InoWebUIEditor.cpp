// Copyright Inoksan. All Rights Reserved.

#include "InoWebUIEditor.h"
#include "InoWebUILog.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "IAssetTypeActions.h"

#define LOCTEXT_NAMESPACE "FInoWebUIEditorModule"

IMPLEMENT_MODULE(FInoWebUIEditorModule, InoWebUIEditor)

void FInoWebUIEditorModule::StartupModule()
{
    // Phase 7 step 3 will register the AssetTypeActions for UInoWebBundle here.
    // For this scaffolding commit we only confirm the module loads so UBT
    // picks up the new Source/InoWebUIEditor tree and the uplugin entry.
    UE_LOG(LogInoWebUI, Log, TEXT("InoWebUIEditor module started."));
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
