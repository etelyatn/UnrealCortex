#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimAuthorOps
{
	static FCortexCommandResult AddNamedNotify(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult UpdateNamedNotify(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveNamedNotify(const TSharedPtr<FJsonObject>& Params);
};
