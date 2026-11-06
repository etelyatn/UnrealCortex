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
	/**
	 * True when the node names this Blueprint's own member chain (self context, a class this Blueprint generates,
	 * or a member parent that cannot be resolved). A node that names a member of another class is not a reference
	 * and is never returned: the engine's own RemoveMemberVariable/rename skip it.
	 */
	bool bResolvesToOwner = true;
};

/**
 * Reusable reference query for Blueprint members.
 *
 * Uses the engine hooks UK2Node::ReferencesVariable / ReferencesFunction with a null scope
 * (member variables have an empty scope name, so a non-null scope hides every member node), then
 * keeps only the hits that resolve to the owner Blueprint's member chain: a node naming a member of
 * another class that happens to share the name is not a reference (it is what the engine's own
 * RemoveMemberVariable / rename leave alone). The query refuses nothing; the skeleton class is only
 * as current as the last compile, so callers that need fresh results compile first.
 */
class CORTEXGRAPH_API FCortexGraphMemberReferences
{
public:
	/**
	 * Member-variable references; same-named local variable nodes are excluded, and so is a node that names the same
	 * member of an unrelated class. MemberOwnerClass is the class that declares the scanned member when it is not the
	 * containing Blueprint's own chain (a dependent scan); null selects the containing Blueprint's chain.
	 */
	static void FindVariableReferences(const UBlueprint* Blueprint, FName VariableName, TArray<FCortexMemberReference>& OutReferences,
		const UClass* MemberOwnerClass = nullptr);

	/** Member-function references (call nodes and engine-hooked nodes); MemberOwnerClass as above. */
	static void FindFunctionReferences(const UBlueprint* Blueprint, FName FunctionName, TArray<FCortexMemberReference>& OutReferences,
		const UClass* MemberOwnerClass = nullptr);

	/** {member, native_parent_declares, reference_count, references[]} */
	static TSharedPtr<FJsonObject> ToJson(const UBlueprint* Blueprint, FName Member, const TArray<FCortexMemberReference>& References);

	static TSharedRef<FJsonObject> ReferenceToJson(const FCortexMemberReference& Reference);
};
