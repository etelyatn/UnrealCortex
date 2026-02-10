
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexDataAssetSearchOps
{
public:
	static FCortexCommandResult SearchAssets(const TSharedPtr<FJsonObject>& Params);
};
