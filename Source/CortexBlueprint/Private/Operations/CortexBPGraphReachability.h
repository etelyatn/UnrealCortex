#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

namespace CortexBPGraphReachability
{
	/** True when the node has at least one exec output pin and no exec input pin. */
	bool IsEntryNode(const UEdGraphNode* Node);

	/** True when the entry node actually starts execution: function entries always, other entries only when an exec output is linked. */
	bool IsLiveEntry(const UEdGraphNode* Node);

	/**
	 * Adds every node reachable by following exec output pins forward from the seed nodes (seeds included).
	 * Seeds already present in OutReachable are still expanded.
	 */
	void GatherExecForward(const TArray<UEdGraphNode*>& Seeds, TSet<UEdGraphNode*>& OutReachable);

	/** Expands the set with every node linked to a member through any pin, in either direction, until closed. */
	void ExpandToLinkedDataDependencies(TSet<UEdGraphNode*>& InOutReachable);

	/** Expands the set with the producers feeding the members' non-exec input pins, transitively. */
	void ExpandBackwardDataClosure(TSet<UEdGraphNode*>& InOutReachable);

	/** Reachable set of one entry node: exec-forward BFS from the entry, then the backward data closure over input pins. */
	void GatherReachableFromEntry(UEdGraphNode* Entry, TSet<UEdGraphNode*>& OutReachable);
}
