#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Categories of user-visible graphs in a Blueprint, in the order
 * EnumerateUserGraphs walks them. Compiler-generated graphs
 * (IntermediateGeneratedGraphs) are deliberately excluded.
 */
enum class ECortexGraphKind : uint8
{
	Ubergraph,
	Function,
	Macro,
	Delegate,
	InterfaceImpl,
};

/**
 * One entry from EnumerateUserGraphs.
 * OwningInterface is NAME_None except when Kind == InterfaceImpl,
 * in which case it holds the FName of the implemented interface class.
 */
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

	// Shared helpers for reuse across CortexGraph operations
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError);

	/**
	 * Enumerate user-visible top-level graphs in a Blueprint.
	 *
	 * Walk order (also defines first-match-wins resolution priority):
	 *   1. UbergraphPages              -> Ubergraph
	 *   2. FunctionGraphs              -> Function
	 *   3. MacroGraphs                 -> Macro
	 *   4. DelegateSignatureGraphs     -> Delegate
	 *   5. ImplementedInterfaces[i].Graphs -> InterfaceImpl
	 *      (within step 5, iteration order matches Blueprint->ImplementedInterfaces[])
	 *
	 * Compiler-generated graphs (IntermediateGeneratedGraphs) are intentionally
	 * skipped — they are not user-editable and exposing them through
	 * graph_cmd would be a footgun.
	 *
	 * Composite/state-machine inner graphs are NOT included here. They are
	 * reached via ResolveSubgraph after the top-level graph is found.
	 */
	static void EnumerateUserGraphs(UBlueprint* Blueprint, TArray<FCortexGraphEntry>& OutEntries);

	/** JSON-friendly tag for ECortexGraphKind ("ubergraph", "function", "macro", "delegate", "interface_impl"). */
	static FString GraphKindToString(ECortexGraphKind Kind);

	static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError);
	static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError);
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError);
	/**
	 * Serialize pin to JSON.
	 * bDetailed: if true, includes is_connected and default_value fields.
	 * bCompact: if true (and bDetailed is true), omits false is_connected and empty default_value/default_text_value.
	 *   Default is false to preserve existing call sites (e.g. AddNode, pin text tests).
	 *   MCP commands (GetNode) pass bCompact explicitly based on the user-supplied compact param (default true).
	 */
	static TSharedRef<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bDetailed = true, bool bCompact = false);

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
