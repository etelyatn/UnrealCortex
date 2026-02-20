
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UDataAsset;

class FCortexDataAssetOps
{
public:
	static FCortexCommandResult ListDataAssets(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDataAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult UpdateDataAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateDataAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteDataAsset(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a DataAsset by asset path, returns nullptr and sets OutError if not found */
	static UDataAsset* LoadDataAsset(const FString& AssetPath, FCortexCommandResult& OutError);

	/** Resolve class name (short name or full path) to a concrete UDataAsset subclass. */
	static UClass* ResolveDataAssetClass(const FString& ClassName, FCortexCommandResult& OutError);
};
