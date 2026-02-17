#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPIEState;

class FCortexEditorUtilityOps
{
public:
	static FCortexCommandResult GetEditorState(const FCortexEditorPIEState& PIEState);
};
