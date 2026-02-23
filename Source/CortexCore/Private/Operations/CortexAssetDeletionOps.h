#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexAssetDeletionOps
{
public:
	static FCortexCommandResult DeleteAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteFolder(const TSharedPtr<FJsonObject>& Params);
};
