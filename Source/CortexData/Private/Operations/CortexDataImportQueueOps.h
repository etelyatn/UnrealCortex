#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexDataImportQueueOps
{
public:
	static FCortexCommandResult ApplyImportOpsJson(const TSharedPtr<FJsonObject>& Params);
};
