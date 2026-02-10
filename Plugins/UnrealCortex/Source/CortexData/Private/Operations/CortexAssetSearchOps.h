
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexAssetSearchOps
{
public:
	static FCortexCommandResult SearchAssets(const TSharedPtr<FJsonObject>& Params);
};
