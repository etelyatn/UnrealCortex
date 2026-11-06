#pragma once

#include "CoreMinimal.h"
#include "CortexSTCompat.h"
#include "CortexSTNodeLocator.h"
#include "CortexSTTypes.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * statetree.get_node_bindings: per-node binding inspection (read-only). Never validates, compiles or dirties the asset.
 *
 * Params: asset_path, node_id? | state_id? | state_path?, direction? ("target"|"source"|"both", default both), limit? (default 50, max 200), offset?.
 * With no selector it pages every node that has at least one binding. Bindings are selected by the editor node ID and by the node-struct
 * ID (FStateTreeEditorNode::GetNodeID, StateTreeEditorNode.h:104-107), which task-completion bindings use as their source.
 */
class FCortexSTBindingOps
{
public:
	static FCortexCommandResult GetNodeBindings(const TSharedPtr<FJsonObject>& Params);

	/**
	 * The unpaged whole-tree report used as content readback by replace_node: every node (slot, owner, node_type, instance type,
	 * as_target/as_source), every stored binding with its resolved owners, and the whole-tree binding count. Two reports of unchanged
	 * content are equal as JSON (FJsonValue::CompareEqual).
	 */
	static TSharedPtr<FJsonObject> BuildTreeReport(const FCortexSTAssetContext& Context);

	/** Number of stored editor bindings in the whole tree. */
	static int32 CountTreeBindings(const FCortexSTAssetContext& Context);
};
