#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * graph.get_node_settings / graph.set_node_settings: the settings struct of an animation graph node (the FAnimNode_*
 * held by a UAnimGraphNode_*), read and written by property path relative to that struct. A write converts and
 * validates every path on a scratch copy first, then applies it in one transaction through the editor's property-edit
 * notifications and reconstructs the node, so pin visibility matches an edit made in the details panel.
 */
class FCortexGraphNodeSettingsOps
{
public:
	static FCortexCommandResult GetNodeSettings(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetNodeSettings(const TSharedPtr<FJsonObject>& Params);
};
