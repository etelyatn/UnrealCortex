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
};
