#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexAssetFingerprintOps
{
public:
	static FCortexCommandResult AssetFingerprint(const TSharedPtr<FJsonObject>& Params);
};
