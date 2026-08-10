#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimNotifyStateOps
{
	static FCortexCommandResult Add(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Update(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Remove(const TSharedPtr<FJsonObject>& Params);
};
