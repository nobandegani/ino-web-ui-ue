// Copyright Inoland. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

/**
 * Content Browser integration for UInoWebBundle.
 *   • Places the asset under a custom "Ino" category in New Asset + filters
 *   • Sets a distinctive swatch color
 *   • Phase 7 step 4 adds a Reimport menu entry here
 */
class FInoWebBundleAssetTypeActions : public FAssetTypeActions_Base
{
public:
    explicit FInoWebBundleAssetTypeActions(EAssetTypeCategories::Type InCategory);

    //~ FAssetTypeActions_Base
    virtual FText   GetName()          const override;
    virtual UClass* GetSupportedClass() const override;
    virtual FColor  GetTypeColor()     const override;
    virtual uint32  GetCategories()          override;
    virtual void    GetActions(const TArray<UObject*>& InObjects, FMenuBuilder& MenuBuilder) override;

private:
    /** Re-read SourceFolder into Files[] for every selected bundle. */
    static void ExecuteReimport(TArray<TWeakObjectPtr<class UInoWebBundle>> Bundles);
    /** Category bit assigned by AssetTools::RegisterAdvancedAssetCategory. */
    EAssetTypeCategories::Type Category;
};
