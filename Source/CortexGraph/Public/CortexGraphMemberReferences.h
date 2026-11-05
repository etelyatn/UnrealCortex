#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

class FJsonObject;
class UBlueprint;

/** One graph node that references a Blueprint member (variable or function). */
struct CORTEXGRAPH_API FCortexMemberReference
{
	FString GraphName;
	FGuid GraphGuid;
	FString NodeName;
	FGuid NodeGuid;
	FString NodeClass;
	/** k2_variable_node | anim_node_binding | anim_node_function | property_access_node | other_k2_node */
	FString Kind;
	/** Anim binding property or function slot; empty for plain K2 nodes. */
	FString Slot;
	/** True for variable nodes whose reference is a self context. */
	bool bSelfContext = false;
};

/**
 * Reusable reference query for Blueprint members.
 *
 * Uses the engine hooks UK2Node::ReferencesVariable / ReferencesFunction with a null scope
 * (member variables have an empty scope name, so a non-null scope hides every member node).
 * The query refuses nothing; the skeleton class is only as current as the last compile,
 * so callers that need fresh results compile first.
 */
class CORTEXGRAPH_API FCortexGraphMemberReferences
{
public:
	/** Member-variable references; same-named local variable nodes are excluded. */
	static void FindVariableReferences(const UBlueprint* Blueprint, FName VariableName, TArray<FCortexMemberReference>& OutReferences);

	/** Member-function references (call nodes and engine-hooked nodes). */
	static void FindFunctionReferences(const UBlueprint* Blueprint, FName FunctionName, TArray<FCortexMemberReference>& OutReferences);

	/** {member, native_parent_declares, reference_count, references[]} */
	static TSharedPtr<FJsonObject> ToJson(const UBlueprint* Blueprint, FName Member, const TArray<FCortexMemberReference>& References);

	static TSharedRef<FJsonObject> ReferenceToJson(const FCortexMemberReference& Reference);
};
