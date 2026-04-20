// Copyright Inoksan. All Rights Reserved.

#include "InoWebBundleFactory.h"
#include "InoWebBundle.h"

UInoWebBundleFactory::UInoWebBundleFactory()
{
    SupportedClass = UInoWebBundle::StaticClass();
    bCreateNew     = true;   // show up in the "New Asset" menu
    bEditAfterNew  = true;   // open the asset editor immediately after creation
}

UObject* UInoWebBundleFactory::FactoryCreateNew(UClass* InClass, UObject* InParent,
    FName InName, EObjectFlags Flags, UObject* /*Context*/, FFeedbackContext* /*Warn*/)
{
    // Empty bundle: the user sets SourceFolder + fields in the details panel,
    // then clicks Reimport to populate Files[].
    return NewObject<UInoWebBundle>(InParent, InClass, InName, Flags);
}
