#pragma once

#include "CoreMinimal.h"
#include "Operations/CortexGraphPatchState.h"
#include "CortexTypes.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UFunction;
class UClass;

struct FCortexGraphImplementationPlan
{
	UFunction* Function = nullptr;
	UClass* FunctionClass = nullptr;
	UEdGraph* ExistingGraph = nullptr;
	UEdGraphNode* ExistingEntryNode = nullptr;
	bool bCanBePlacedAsEvent = false;
	bool bParentCall = false;
	bool bWouldCreate = false;
};

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
	static bool ValidateEligibility(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Selector,
		FCortexGraphImplementationPlan& OutPlan,
		FCortexCommandResult& OutError
	);

	static FCortexGraphImplementationEnsureResult EnsureForPatch(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Selector,
		FCortexGraphPatchState& PatchState
	);
};
