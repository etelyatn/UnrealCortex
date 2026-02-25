#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPCleanupOps
{
public:
	/**
	 * Clean up a Blueprint after C++ migration.
	 * Params: asset_path (string), new_parent_class (string, optional),
	 *         remove_variables (array, optional), remove_functions (array, optional),
	 *         compile (bool, optional, default true)
	 */
	static FCortexCommandResult CleanupMigration(const TSharedPtr<FJsonObject>& Params);
};
