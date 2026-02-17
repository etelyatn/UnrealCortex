
#pragma once

#include "CoreMinimal.h"

class UMaterial;

/**
 * RAII guard for batch execution.
 * Increments BatchDepth on construction, decrements on destruction.
 * On destruction, invokes all registered cleanup actions, then calls
 * PostEditChange + RebuildGraph for all dirty materials.
 */
class CORTEXCORE_API FCortexBatchScope
{
public:
	using FBatchCleanupCallback = TFunction<void()>;

	FCortexBatchScope();
	~FCortexBatchScope();

	// Non-copyable, non-movable
	FCortexBatchScope(const FCortexBatchScope&) = delete;
	FCortexBatchScope& operator=(const FCortexBatchScope&) = delete;

	/** Mark a material as needing PostEditChange on batch end. */
	static void MarkMaterialDirty(UMaterial* Material);

	/**
	 * Register a cleanup action to run when the outermost batch ends.
	 * Key-based deduplication: only the first callback per key is kept.
	 * Domain modules use this for deferred notifications (NotifyGraphChanged, etc.)
	 */
	static void AddCleanupAction(const FString& Key, FBatchCleanupCallback Callback);

private:
	/** Materials that need PostEditChange when batch ends. */
	static TSet<TWeakObjectPtr<UMaterial>> DirtyMaterials;

	/** Generic cleanup actions keyed for deduplication. */
	static TMap<FString, FBatchCleanupCallback> CleanupActions;
};
