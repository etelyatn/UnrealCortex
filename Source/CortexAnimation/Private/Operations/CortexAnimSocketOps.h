#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimSocketOps
{
	static FCortexCommandResult AddSocket(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetSocketTransform(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveSocket(const TSharedPtr<FJsonObject>& Params);
};
