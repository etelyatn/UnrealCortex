#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexSTAssetOps
{
public:
	static FCortexCommandResult ListAssets(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DuplicateAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SaveAsset(const FString& AssetPath);
};
