
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FUDBAssetSearchOps
{
public:
	static FUDBCommandResult SearchAssets(const TSharedPtr<FJsonObject>& Params);
};
