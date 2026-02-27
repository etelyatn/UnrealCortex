#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPRedirectorOps
{
public:
	/**
	 * Fix up redirectors in a given path.
	 * Params: path (string), recursive (bool, optional, default true)
	 */
	static FCortexCommandResult FixupRedirectors(const TSharedPtr<FJsonObject>& Params);
};
