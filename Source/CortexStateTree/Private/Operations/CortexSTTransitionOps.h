#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexSTTransitionOps
{
public:
	static FCortexCommandResult AddTransition(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveTransition(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetTransitionProperties(const TSharedPtr<FJsonObject>& Params);
};
