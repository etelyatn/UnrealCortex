#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPStructureOps
{
public:
	static FCortexCommandResult AddVariable(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveVariable(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddFunction(const TSharedPtr<FJsonObject>& Params);
};
