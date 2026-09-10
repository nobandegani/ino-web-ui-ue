// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoWebBundleActions.h"
#include "InoWebBundle.h"
#include "InoWebUILog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"

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

void FInoWebBundleAssetTypeActions::GetActions(const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder)
{
    // Collect weak refs — the menu may execute much later, after the selection
    // has changed. Weak refs keep the lambda safe across GC.
    TArray<TWeakObjectPtr<UInoWebBundle>> Bundles;
    for (UObject* Obj : InObjects)
    {
        if (UInoWebBundle* Bundle = Cast<UInoWebBundle>(Obj))
        {
            Bundles.Add(Bundle);
        }
    }
    if (Bundles.Num() == 0) return;

    MenuBuilder.AddMenuEntry(
        LOCTEXT("Reimport_Label",   "Reimport Source Folder"),
        LOCTEXT("Reimport_Tooltip",
            "Re-read SourceFolder into this bundle, recompute its content hash, "
            "and mark the asset dirty so you can save it."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
        FUIAction(FExecuteAction::CreateStatic(&FInoWebBundleAssetTypeActions::ExecuteReimport, Bundles))
    );
}

void FInoWebBundleAssetTypeActions::ExecuteReimport(TArray<TWeakObjectPtr<UInoWebBundle>> Bundles)
{
    int32 OK = 0;
    int32 Fail = 0;

    for (const TWeakObjectPtr<UInoWebBundle>& Weak : Bundles)
    {
        UInoWebBundle* Bundle = Weak.Get();
        if (!Bundle) continue;

        const FString Folder = Bundle->GetAbsoluteSourceFolder();
        if (Folder.IsEmpty())
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("Reimport: '%s' has no SourceFolder set — skipping."),
                *Bundle->GetName());
            ++Fail;
            continue;
        }

        if (Bundle->BundleFromFolder(Folder))
        {
            ++OK;
        }
        else
        {
            ++Fail;
        }
    }

    UE_LOG(LogInoWebUI, Log,
        TEXT("Reimport finished: %d succeeded, %d failed."), OK, Fail);
}

#undef LOCTEXT_NAMESPACE
