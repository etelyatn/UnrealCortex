#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UK2Node_CallFunction;

/**
 * Class references carried by Blueprint graph nodes, shared by find_usages and export_dependency_graph so both
 * read cast, spawn and create-widget targets the same way.
 */
namespace CortexReflectNodeRefs
{
	/** Class a UK2Node_DynamicCast casts to; null for any other node or an unset target. */
	UClass* GetCastTargetClass(const UEdGraphNode* Node);

	/** Class held as the default of a UK2Node_SpawnActorFromClass class pin; null when unset or wired. */
	UClass* GetSpawnedClass(const UEdGraphNode* Node);

	/**
	 * Class held as the default of a CreateWidget node class pin. The node type lives in UMGEditor, so it is
	 * recognised by class name (K2Node_CreateWidget) on a UK2Node_ConstructObjectFromClass; null otherwise.
	 */
	UClass* GetCreatedWidgetClass(const UEdGraphNode* Node);

	/**
	 * Calls Visitor for every node of every graph of Blueprint: event graphs, function graphs, macros, delegate signatures
	 * and the graphs of implemented interfaces, descending into composite (collapsed) subgraphs. TopGraph is the top-level graph
	 * that holds the node; SubgraphPath is the dot-separated names of the composite subgraphs below it ("" at the top level).
	 */
	void ForEachNode(
		const UBlueprint* Blueprint,
		TFunctionRef<void(const UEdGraph* TopGraph, const FString& SubgraphPath, const UEdGraphNode* Node)> Visitor);

	/** `GetPrefixCPP() + GetName()` of the node's class, e.g. `UK2Node_Message`; empty for null. */
	FString GetNodeClassLabel(const UEdGraphNode* Node);

	/**
	 * The class a call or message node's function belongs to, resolved in Context (a self-context member resolves to Context).
	 * Skeleton classes are normalised to their authoritative class so the result compares with the real class; null when the
	 * owner cannot be resolved. Matching by this owner, never by member name, is what keeps same-named members apart.
	 */
	const UClass* GetCallMemberOwner(const UK2Node_CallFunction* Node, const UClass* Context);
}
