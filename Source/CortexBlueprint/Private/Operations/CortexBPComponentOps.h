#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPComponentOps
{
public:
	static FCortexCommandResult SetComponentDefaults(const TSharedPtr<FJsonObject>& Params);
};
