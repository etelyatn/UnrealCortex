#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

class FCortexGraphAuthoringContext
{
public:
	static FCortexCommandResult Read(const TSharedPtr<FJsonObject>& Params);
};
