#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

namespace CortexBPGraphReachability
{
	/** True when the node has at least one exec output pin and no exec input pin. */
	bool IsEntryNode(const UEdGraphNode* Node);

	/** True when the entry node actually starts execution: function entries always, other entries only when an exec output is linked. */
	bool IsLiveEntry(const UEdGraphNode* Node);
}
