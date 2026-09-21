#pragma once

#include "CoreMinimal.h"
#include "Operations/CortexGraphPatchState.h"
#include "CortexTypes.h"

class UBlueprint;
class UEdGraph;
class UK2Node_FunctionEntry;
class UK2Node_FunctionResult;

struct FCortexGraphImplementationEnsureResult : public FCortexCommandResult
{
	UEdGraph* Graph = nullptr;
	UEdGraphNode* EntryNode = nullptr;
	UEdGraphNode* ResultNode = nullptr;
	bool bCreated = false;
};

class FCortexGraphImplementationOps
{
public:
	static FCortexGraphImplementationEnsureResult EnsureForPatch(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Selector,
		FCortexGraphPatchState& PatchState
	);
};
