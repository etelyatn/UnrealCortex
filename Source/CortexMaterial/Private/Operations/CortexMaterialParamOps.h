#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexMaterialParamOps
{
public:
	static FCortexCommandResult ListParameters(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetParameters(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ResetParameter(const TSharedPtr<FJsonObject>& Params);
};
