#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPAnalysisOps
{
public:
	/**
	 * Analyze a Blueprint for C++ migration.
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult AnalyzeForMigration(const TSharedPtr<FJsonObject>& Params);
};
