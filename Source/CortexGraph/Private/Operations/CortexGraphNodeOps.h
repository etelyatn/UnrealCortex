#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

class FCortexGraphNodeOps
{
public:
	static FCortexCommandResult ListGraphs(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListNodes(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SearchNodes(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetPinValue(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AutoLayout(const TSharedPtr<FJsonObject>& Params);

	// Shared helpers for reuse across CortexGraph operations
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError);
	static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError);
	static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError);
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError);
	// Serialize pin to JSON. bDetailed: if true, includes is_connected and default_value fields
	static TSharedRef<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bDetailed = true);

	/**
	 * Resolve a dot-separated subgraph path from a root graph.
	 * Each segment matches a UK2Node_Composite whose BoundGraph name equals the segment.
	 * Returns the root Graph if SubgraphPath is empty.
	 * Returns nullptr and populates OutError if resolution fails.
	 * Max depth: 5 levels.
	 */
	static UEdGraph* ResolveSubgraph(UEdGraph* RootGraph, const FString& SubgraphPath, FCortexCommandResult& OutError);

private:
	static constexpr int32 MaxSubgraphDepth = 5;
	/** Recursively collect composite subgraph entries for ListGraphs. */
	static void CollectSubgraphsRecursive(
		UEdGraph* Graph,
		const FString& ParentGraphName,
		const FString& CurrentSubgraphPath,
		TArray<TSharedPtr<FJsonValue>>& OutArray,
		int32 Depth
	);
};
