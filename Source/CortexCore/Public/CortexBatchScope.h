
#pragma once

#include "CoreMinimal.h"

class UMaterial;

/** Callback type for deferred batch cleanup actions. */
using FBatchCleanupCallback = TFunction<void()>;

/**
 * RAII guard for batch execution.
 * Increments BatchDepth on construction, decrements on destruction.
 * On destruction, calls PostEditChange + RebuildGraph for all dirty materials,
 * then invokes all registered cleanup actions.
 */
class CORTEXCORE_API FCortexBatchScope
{
public:
	FCortexBatchScope();
	~FCortexBatchScope();

	// Non-copyable, non-movable
	FCortexBatchScope(const FCortexBatchScope&) = delete;
	FCortexBatchScope& operator=(const FCortexBatchScope&) = delete;

	/** Mark a material as needing PostEditChange on batch end. */
	static void MarkMaterialDirty(UMaterial* Material);

	/** Register a cleanup action to run when the outermost batch scope ends. */
	static void AddCleanupAction(FBatchCleanupCallback Callback);

private:
	/** Materials that need PostEditChange when batch ends. */
	static TSet<TWeakObjectPtr<UMaterial>> DirtyMaterials;

	/** Generic cleanup callbacks registered by domain modules. */
	static TArray<FBatchCleanupCallback> CleanupActions;
};
