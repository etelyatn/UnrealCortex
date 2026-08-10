#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexDataJsonDiffOps
{
public:
	static FCortexCommandResult CompareDataJson(const TSharedPtr<FJsonObject>& Params);
};
