#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPComponentOps
{
public:
	static FCortexCommandResult SetComponentDefaults(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Add an SCS component to a Blueprint's SimpleConstructionScript hierarchy.
	 * Params: asset_path (string), component_class (string),
	 *         component_name (string, optional), parent_component (string, optional),
	 *         compile (bool, optional, default true)
	 */
	static FCortexCommandResult AddSCSComponent(const TSharedPtr<FJsonObject>& Params);
};
