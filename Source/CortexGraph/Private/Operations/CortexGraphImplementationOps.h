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
	/** The function is declared by an interface the Blueprint implements through ImplementedInterfaces (migration shell only). */
	bool bInterfaceImplementation = false;
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
	/**
	 * bAllowInterfaceImplementation is set only by the migration shell (replace_entry): it accepts a function declared by an
	 * interface that Blueprint->ImplementedInterfaces lists, whose function-style implementation lives in
	 * FBPInterfaceDescription::Graphs. The authoring shell never sets it and refuses interface functions with a message that
	 * names replace_interface.
	 */
	static bool ValidateEligibility(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Selector,
		FCortexGraphImplementationPlan& OutPlan,
		FCortexCommandResult& OutError,
		bool bAllowInterfaceImplementation = false
	);

	static FCortexGraphImplementationEnsureResult EnsureForPatch(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Selector,
		FCortexGraphPatchState& PatchState
	);
};
