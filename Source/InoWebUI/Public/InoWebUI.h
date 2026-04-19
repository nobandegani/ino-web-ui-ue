// Copyright Inoksan. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * InoWebUI — native WebView overlay module.
 *
 * The module itself is intentionally thin: it only owns the lifetime of the
 * log category. All real work happens in UInoWebUISubsystem (which is created
 * automatically when a UGameInstance comes up) and UInoWebView (handle to a
 * single WebView overlay).
 */
class FInoWebUIModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule()  override;
    virtual void ShutdownModule() override;

    /** Convenience accessor. Returns nullptr if the module hasn't loaded yet. */
    static FInoWebUIModule* GetPtr()
    {
        return FModuleManager::GetModulePtr<FInoWebUIModule>(TEXT("InoWebUI"));
    }
};
