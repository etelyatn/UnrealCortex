#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UStateTree;

class FCortexSTValidationOps
{
public:
	static FCortexCommandResult CheckStructure(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ValidateAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult Compile(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RunPostMutationFixups(UStateTree* StateTree);
};
