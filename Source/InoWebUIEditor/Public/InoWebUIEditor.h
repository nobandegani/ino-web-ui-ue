// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class IAssetTypeActions;

/**
 * Editor-only module for the InoWebUI plugin.
 *
 * Hosts the asset factory and AssetTypeActions that surface UInoWebBundle
 * in the Content Browser (right-click → Ino → Web Bundle). Runtime code
 * stays in the InoWebUI module and is safe to ship; everything in this
 * module is stripped from cooked builds.
 */
class FInoWebUIEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule()  override;
    virtual void ShutdownModule() override;

private:
    /** Actions registered with the Content Browser — one per asset type. */
    TArray<TSharedPtr<IAssetTypeActions>> RegisteredActions;
};
