#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexGraphTree.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

struct FCortexGraphEntry
{
	UEdGraph* Graph = nullptr;
	ECortexGraphKind Kind = ECortexGraphKind::Function;
	FName OwningInterface = NAME_None;
};

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
	static FCortexCommandResult DescribeNode(const TSharedPtr<FJsonObject>& Params);
	static UClass* ResolveNodeClass(const FString& NodeClassName);

	// Shared helpers for reuse across CortexGraph operations
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError);
	static void EnumerateUserGraphs(const UBlueprint* Blueprint, TArray<FCortexGraphEntry>& OutEntries);
	static FString GraphKindToString(ECortexGraphKind Kind);
	static bool FindGraphEntry(UBlueprint* Blueprint, const FString& GraphName, FCortexGraphEntry& OutEntry);
	static bool IsMutableGraphKind(ECortexGraphKind Kind);
	static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError);
	static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError);
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError);
	/** Refuse an asset path outside the writable content roots (the check every graph mutator runs before loading). */
	static bool ValidateWritableAssetPath(const FString& AssetPath, FCortexCommandResult& OutError);
	/** Resolve a root graph by name that graph_cmd may mutate; GRAPH_NOT_FOUND or INVALID_OPERATION otherwise. */
	static bool ResolveMutableGraph(UBlueprint* Blueprint, const FString& GraphName, UEdGraph*& OutGraph, FCortexCommandResult& OutError);
	/**
	 * Serialize pin to JSON.
	 * bDetailed: if true, includes is_connected and default_value fields.
	 * bCompact: if true (and bDetailed is true), omits false is_connected and empty default_value/default_text_value.
	 *   Default is false to preserve existing call sites (e.g. AddNode, pin text tests).
	 *   MCP commands (GetNode) pass bCompact explicitly based on the user-supplied compact param (default true).
	 */
	static TSharedRef<FJsonObject> SerializePin(
		const UEdGraphPin* Pin,
		bool bDetailed = true,
		bool bCompact = false,
		const UBlueprint* ContextBlueprint = nullptr);
	static TSharedRef<FJsonObject> SerializeNode(const UEdGraphNode* Node, bool bIncludePins, bool bCompact);

	/**
	 * Returns true when a pin should be omitted in compact mode.
	 * A pin is skipped when ALL of the following hold:
	 *   - bHidden is true
	 *   - LinkedTo.Num() == 0 (not connected)
	 *   - DefaultValue is empty
	 *   - DefaultTextValue is empty
	 *   - DefaultObject is nullptr
	 *
	 * Note: SubPins (e.g., split Vector X/Y/Z) are separate UEdGraphPin* entries and are
	 * evaluated independently — a hidden parent pin may be skipped even if its sub-pins
	 * have connections, since those sub-pins will be serialized on their own.
	 */
	static bool ShouldSkipPinCompact(const UEdGraphPin* Pin);

	/**
	 * Resolve a dot-separated subgraph path from a root graph.
	 * Each segment matches a UK2Node_Composite whose BoundGraph name equals the segment.
	 * Returns the root Graph if SubgraphPath is empty.
	 * Returns nullptr and populates OutError if resolution fails.
	 * Max depth: 5 levels.
	 */
	static UEdGraph* ResolveSubgraph(UEdGraph* RootGraph, const FString& SubgraphPath, FCortexCommandResult& OutError);

	/**
	 * Runs the editor placement hook (PostPlacedNewNode) on a new anim graph node inside an Anim Blueprint, before its pins
	 * are allocated, as FGraphNodeCreator does. It creates the node's Binding (without it the anim compiler ignores linked
	 * pins), registers Anim Blueprint extensions and applies subclass placement (e.g. BlendListByInt's second pose).
	 * Anything else is left alone: the hook's extension request asserts on an anim node outside an Anim Blueprint.
	 */
	static void PlaceNewAnimNode(UEdGraphNode* Node);

private:
	static constexpr int32 MaxSubgraphDepth = 5;
	static FCortexCommandResult ApplyTextPinValue(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		UEdGraphNode* Node,
		UEdGraphPin* Pin,
		const TSharedPtr<FJsonObject>& TextObject,
		const FString& AssetPath,
		const FString& GraphName,
		const FString& GraphKind,
		const FString& OwningInterface,
		const FString& SubgraphPath,
		const FString& NodeId,
		const FString& PinName,
		const TSharedPtr<FJsonObject>& FingerprintBefore);

	static TSharedPtr<FJsonObject> BuildPinLocator(
		const FString& AssetPath,
		UEdGraph* Graph,
		UEdGraphNode* Node,
		const FString& GraphName,
		const FString& GraphKind,
		const FString& OwningInterface,
		const FString& SubgraphPath,
		const FString& NodeId,
		const FString& PinName);
};
