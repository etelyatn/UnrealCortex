#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexBPGraphCleanupOps
{
public:
	/**
	 * Delete orphaned nodes from a Blueprint graph.
	 *
	 * An orphaned node is one not reachable from event entry nodes through exec
	 * pin chains. Event entry nodes (UK2Node_Event subclasses) are always
	 * preserved.
	 *
	 * Params:
	 * - asset_path (string, required)
	 * - graph_name (string, required)
	 * - compile (bool, optional, default true)
	 */
	static FCortexCommandResult DeleteOrphanedNodes(const TSharedPtr<FJsonObject>& Params);
};
