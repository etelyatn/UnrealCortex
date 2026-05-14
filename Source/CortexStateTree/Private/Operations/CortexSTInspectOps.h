#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexSTInspectOps
{
public:
	static FCortexCommandResult DumpTree(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetState(const TSharedPtr<FJsonObject>& Params);
};
