// CortexBPVariableReferencesOps.h
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPVariableReferencesOps
{
public:
	/**
	 * blueprint.find_variable_references (read-only, #277): every get and set node of one variable, matched by resolved member
	 * owner. Params: asset_path, variable (a Blueprint variable or an inherited/native property name).
	 */
	static FCortexCommandResult FindVariableReferences(const TSharedPtr<FJsonObject>& Params);
};
