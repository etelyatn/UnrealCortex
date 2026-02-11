#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexGraphConnectionOps
{
public:
	static FCortexCommandResult Connect(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Disconnect(const TSharedPtr<FJsonObject>& Params);
};
