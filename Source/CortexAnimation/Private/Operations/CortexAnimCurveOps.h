#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimCurveOps
{
	static FCortexCommandResult AddCurve(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetCurveKeys(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveCurve(const TSharedPtr<FJsonObject>& Params);
};
