#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Runtime (PIE) readback of live UserWidget instances and label-keyed snapshot/diff of that readback.
 * All three commands require the PIE world (diff additionally reads the stored snapshot first); none
 * mutates an asset. Snapshots are plain JSON files under Saved/Cortex/WidgetSnapshots.
 */
class FCortexUMGLiveWidgetOps
{
public:
    static FCortexCommandResult GetLiveWidgets(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SnapshotLiveWidgets(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DiffLiveWidgets(const TSharedPtr<FJsonObject>& Params);

    /**
     * Flattens a JSON value into leaf entries keyed by field path: object members join with '.', array elements
     * index as `[i]`, and '.', '[', ']' and '\\' inside object keys are backslash-escaped so a literal key "a.b"
     * never aliases the nested path a -> b. An empty object or array is kept as a leaf (an empty container value).
     * Key is the prefix of the value itself. Exposed for the unit tests.
     */
    static void FlattenValue(const FString& Key, const TSharedPtr<FJsonValue>& Value, TMap<FString, TSharedPtr<FJsonValue>>& Out);

    /**
     * Equality of two flattened leaves: same JSON type and value; strings compare case-sensitively, numbers
     * exactly (NaN equals NaN); empty-container leaves equal only the same container type.
     */
    static bool LeavesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B);
};
