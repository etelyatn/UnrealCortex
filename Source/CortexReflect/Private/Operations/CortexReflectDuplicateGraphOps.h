#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * reflect.find_duplicate_graphs: finds copy-pasted logic across the Blueprints under the requested prefixes.
 *
 * Graph units are every function graph (including interface implementations), macro graph, and ubergraph event
 * chain (an event node plus every node reachable from it through links, with the pure producers feeding them).
 * Each unit is reduced to a normalised form that ignores GUIDs, node names, positions, sizes, comments, reroute
 * knots, comment boxes and disabled nodes: one descriptor per node (class, member reference, non-default
 * unlinked input pin values) plus the link topology expressed through canonical descriptor ranks.
 * Exact groups are units with an identical normalised hash across at least two Blueprints; similar groups are
 * same-named units of different Blueprints whose descriptor multisets have a Jaccard ratio of at least the
 * requested similarity. Widget trees are compared as class+name multisets (kind widget_tree). Read-only.
 */
class FCortexReflectDuplicateGraphOps
{
public:
	static FCortexCommandResult FindDuplicateGraphs(const TSharedPtr<FJsonObject>& Params);
};
