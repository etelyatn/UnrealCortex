#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;

namespace CortexBPGraphReachability
{
	/** True when the node has at least one exec output pin and no exec input pin. */
	bool IsEntryNode(const UEdGraphNode* Node);

	/**
	 * True when the entry node actually starts execution: it is enabled (a Disabled node, including an automatically placed
	 * ghost event, never runs) and it is a function entry, or another entry with a linked exec output.
	 */
	bool IsLiveEntry(const UEdGraphNode* Node);

	/** True for animation and state machine graphs, whose pose-flow networks have no event/exec semantics. */
	bool IsAnimationOrStateMachineGraph(const UEdGraph* Graph);

	/**
	 * Adds every node reachable by following exec output pins forward from the seed nodes (seeds included), ignoring enabled state.
	 * Seeds already present in OutReachable are still expanded. Enabled-state-aware, pin-aware reach is FReachContext.
	 */
	void GatherExecForward(const TArray<UEdGraphNode*>& Seeds, TSet<UEdGraphNode*>& OutReachable);

	/** Expands the set with every node linked to a member through any pin, in either direction, until closed. */
	void ExpandToLinkedDataDependencies(TSet<UEdGraphNode*>& InOutReachable);
}
