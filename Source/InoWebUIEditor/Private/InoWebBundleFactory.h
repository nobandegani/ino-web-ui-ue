// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "InoWebBundleFactory.generated.h"

/**
 * Editor factory for UInoWebBundle — exposes "Web Bundle" under the custom
 * "Ino" category in the Content Browser's New Asset menu.
 *
 * No input file wizard; the asset is created empty and populated later via
 * the Reimport button once the user has set SourceFolder.
 */
UCLASS()
class UInoWebBundleFactory : public UFactory
{
    GENERATED_BODY()

public:
    UInoWebBundleFactory();

    //~ UFactory
    virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName,
        EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};
