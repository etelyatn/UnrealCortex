#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexSTStateOps
{
public:
	static FCortexCommandResult AddState(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveState(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RenameState(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult MoveState(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetStateProperties(const TSharedPtr<FJsonObject>& Params);
};
