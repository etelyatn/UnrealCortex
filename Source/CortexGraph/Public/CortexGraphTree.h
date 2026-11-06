#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Misc/Guid.h"

class FJsonObject;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/** Root graph category of a Blueprint, as published by graph.list_graphs `kind` for root graphs. */
enum class ECortexGraphKind : uint8
{
	Ubergraph,
	Function,
	Macro,
	Delegate,
	InterfaceImpl,
};

/** One graph of a Blueprint's graph tree: a root graph or a graph nested below one. */
struct CORTEXGRAPH_API FCortexGraphTreeEntry
{
	UEdGraph* Graph = nullptr;
	/**
	 * Root graphs: ubergraph | function | macro | delegate | interface_impl (unchanged from graph.list_graphs).
	 * Nested graphs: composite | anim_state_machine | anim_state | anim_transition_rule | anim_custom_transition |
	 * anim_conduit | anim_blend_space | anim_blend_space_sample | anim_graph | nested.
	 */
	FString Kind;
	/** Invalid for root graphs. */
	FGuid ParentGraphGuid;
	FString ParentGraphName;
	/** Node whose GetSubGraphs() returned this graph; invalid when the graph was found only through UEdGraph::SubGraphs. */
	FGuid OwnerNodeGuid;
	FString OwnerNodeName;
	/**
	 * Root-relative locator, unique per tree: the root graph name, then one `/`-separated segment per nesting level.
	 * A segment repeated in the tree gets a `~N` suffix in collection order, so equal graph names never share a path.
	 */
	FString Path;
	/** Dot-separated composite path relative to the root graph (the legacy subgraph_path); empty unless every level is a composite. */
	FString SubgraphPath;
	FString RootGraphName;
	/** 0 for root graphs. */
	int32 Depth = 0;
	/** Index of the parent entry in the collected array; INDEX_NONE for root graphs. */
	int32 ParentIndex = INDEX_NONE;
	bool bRoot = false;
	/** Category of the root graph this entry belongs to (the entry's own category when bRoot). */
	ECortexGraphKind RootKind = ECortexGraphKind::Function;
	/** Interface name for root graphs of an implemented interface; NAME_None otherwise. */
	FName OwningInterface = NAME_None;
	/** False when another collected graph carries the same GraphGuid. */
	bool bGuidUnique = true;
	/** True when this graph has nested graphs that were not collected because the depth limit was reached. */
	bool bChildrenTruncated = false;
};

/**
 * The shared catalogue of a Blueprint's graphs, including graphs nested below root graphs (composite bodies and, for
 * Animation Blueprints, state machine, state, transition and conduit graphs).
 *
 * Nested graphs are discovered through engine virtuals and reflection only: UEdGraph::SubGraphs plus the non-null
 * UEdGraphNode::GetSubGraphs() of every node. No animation module is linked. Nested graphs are read-only for every Cortex
 * authoring command; only the read commands address them.
 */
class CORTEXGRAPH_API FCortexGraphTree
{
public:
	/** Deepest nesting level collected below a root graph (roots are depth 0). */
	static constexpr int32 MaxDepth = 8;

	/**
	 * Roots in EnumerateUserGraphs order, each followed by its nested graphs depth first (owner nodes in node order, then
	 * graphs reachable only through SubGraphs). Pointer-deduplicated, cycle-safe, deterministic. A cut at MaxDepth is
	 * recorded in FCortexGraphTreeEntry::bChildrenTruncated, never dropped silently.
	 */
	static void Collect(const UBlueprint* Blueprint, TArray<FCortexGraphTreeEntry>& OutEntries);

	/** Kind of a nested graph from its owner node first (exact engine class names), then from the graph class; "nested" otherwise. */
	static FString KindFromGraph(const UEdGraph* Graph, const UEdGraphNode* Owner);

	/** {name, class, node_count, kind, graph_guid, graph_guid_unique, path[, parent_*, owner_node_guid, subgraph_path, owning_interface, graph_class_kind]} */
	static TSharedRef<FJsonObject> ToJson(const FCortexGraphTreeEntry& Entry);

	/**
	 * Resolve the single graph a read command addresses.
	 *  - graph_path names one graph exactly; graph_guid names one graph anywhere in the tree. When both are given they must agree.
	 *  - A graph_guid shared by several graphs refuses INVALID_FIELD and lists their paths (ErrorDetails.colliding_graphs);
	 *    graph_path disambiguates. An unparsable graph_guid is INVALID_FIELD, an unknown graph_guid or graph_path GRAPH_NOT_FOUND.
	 *  - graph_name and subgraph_path must agree with a graph_guid/graph_path target, else INVALID_FIELD.
	 *  - Without graph_guid/graph_path, graph_name keeps its root-graph meaning (default EventGraph, then subgraph_path as a
	 *    composite path). A name that matches only nested graphs refuses GRAPH_NOT_FOUND with
	 *    ErrorDetails.nested_candidates[{graph_guid, graph_path, kind}] and is never resolved by guesswork.
	 * OutEntry, when given, receives the resolved graph's catalogue entry.
	 */
	static bool ResolveReadTarget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, UEdGraph*& OutGraph,
		FCortexGraphTreeEntry* OutEntry, FCortexCommandResult& OutError);

	/** ResolveReadTarget against an already collected tree; returns the entry inside Tree, or null with OutError set. */
	static const FCortexGraphTreeEntry* ResolveInTree(const TArray<FCortexGraphTreeEntry>& Tree, const TSharedPtr<FJsonObject>& Params,
		FCortexCommandResult& OutError);

	/** True when Params names a graph through graph_guid, graph_path, graph_name or subgraph_path. */
	static bool HasGraphSelector(const TSharedPtr<FJsonObject>& Params);

	/**
	 * The graphs a read command covers: the whole tree when Params carries no graph selector, otherwise the resolved graph
	 * (ResolveReadTarget rules) and every graph nested below it, in collection order.
	 */
	static bool ResolveReadScope(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
		TArray<FCortexGraphTreeEntry>& OutEntries, FCortexCommandResult& OutError);

	/** Report truncation of the collected entries into Data: truncated=true and truncated_parent_graph_guids; no-op when nothing was cut. */
	static void AppendTruncation(const TArray<FCortexGraphTreeEntry>& Entries, const TSharedPtr<FJsonObject>& Data);
};
