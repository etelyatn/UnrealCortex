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

	/**
	 * Recompile all Blueprints that depend on the given Blueprint.
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult RecompileDependents(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Remove an SCS (Simple Construction Script) component node from a Blueprint.
	 * Used when migrating Blueprint-layer components to C++ UPROPERTY members.
	 * Params: asset_path (string), component_name (string),
	 *         compile (bool, optional, default true)
	 */
	static FCortexCommandResult RemoveSCSComponent(const TSharedPtr<FJsonObject>& Params);
};
