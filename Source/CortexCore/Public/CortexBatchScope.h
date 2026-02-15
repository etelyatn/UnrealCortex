
#pragma once

#include "CoreMinimal.h"

class UMaterial;

/**
 * RAII guard for batch execution.
 * Increments BatchDepth on construction, decrements on destruction.
 * On destruction, calls PostEditChange + RebuildGraph for all dirty materials.
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

private:
	/** Materials that need PostEditChange when batch ends. */
	static TSet<TWeakObjectPtr<UMaterial>> DirtyMaterials;
};
