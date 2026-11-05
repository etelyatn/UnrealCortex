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
	 * Adds every node reachable by following exec output pins forward from the seed nodes (seeds included).
	 * Seeds already present in OutReachable are still expanded.
	 * With bEnabledOnly, a Disabled node is neither added nor traversed (the compiler prunes it).
	 */
	void GatherExecForward(const TArray<UEdGraphNode*>& Seeds, TSet<UEdGraphNode*>& OutReachable, bool bEnabledOnly = false);

	/** Expands the set with every node linked to a member through any pin, in either direction, until closed. */
	void ExpandToLinkedDataDependencies(TSet<UEdGraphNode*>& InOutReachable);

	/** Expands the set with the producers feeding the members' non-exec input pins, transitively. With bEnabledOnly, Disabled producers are not added. */
	void ExpandBackwardDataClosure(TSet<UEdGraphNode*>& InOutReachable, bool bEnabledOnly = false);
}
