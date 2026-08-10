#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimMontageOps
{
	static FCortexCommandResult AddSection(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult UpdateSection(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveSection(const TSharedPtr<FJsonObject>& Params);
};
