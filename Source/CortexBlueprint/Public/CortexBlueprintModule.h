#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCortexBlueprint, Log, All);
struct FAssetData;

class CORTEXBLUEPRINT_API FCortexBlueprintModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	void RebuildBlueprintCache();

private:
	FDelegateHandle OnAssetAddedHandle;
	FDelegateHandle OnAssetRemovedHandle;
	FDelegateHandle OnAssetUpdatedHandle;
	FDelegateHandle OnFilesLoadedHandle;
	FTimerHandle CacheWriteTimerHandle;
	bool bInitialCacheBuilt = false;

	void OnAssetRegistryReady();
	void OnAssetChanged(const FAssetData& AssetData);
	void OnAssetRemoved(const FAssetData& AssetData);
	void WriteBlueprintCache();
};
