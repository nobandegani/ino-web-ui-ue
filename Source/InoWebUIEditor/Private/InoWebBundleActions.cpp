// Copyright Inoksan. All Rights Reserved.

#include "InoWebBundleActions.h"
#include "InoWebBundle.h"

#define LOCTEXT_NAMESPACE "InoWebBundleAssetTypeActions"

FInoWebBundleAssetTypeActions::FInoWebBundleAssetTypeActions(EAssetTypeCategories::Type InCategory)
    : Category(InCategory)
{
}

FText FInoWebBundleAssetTypeActions::GetName() const
{
    return LOCTEXT("AssetName", "Web Bundle");
}

UClass* FInoWebBundleAssetTypeActions::GetSupportedClass() const
{
    return UInoWebBundle::StaticClass();
}

FColor FInoWebBundleAssetTypeActions::GetTypeColor() const
{
    // Soft emerald — distinctive in the Content Browser thumbnail strip.
    return FColor(52, 200, 140);
}

uint32 FInoWebBundleAssetTypeActions::GetCategories()
{
    return Category;
}

#undef LOCTEXT_NAMESPACE
