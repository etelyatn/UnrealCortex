#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPCompareOps
{
public:
	/**
	 * Compare two Blueprints and return structured differences.
	 * Params: source_path (string), target_path (string), sections (array<string>, optional)
	 */
	static FCortexCommandResult CompareBlueprints(const TSharedPtr<FJsonObject>& Params);
};
