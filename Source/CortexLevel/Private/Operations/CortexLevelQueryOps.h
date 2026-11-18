#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelQueryOps
{
public:
    static FCortexCommandResult ListActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult FindActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetBounds(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SelectActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetSelection(const TSharedPtr<FJsonObject>& Params);

    /** Filtered actor scan with a bounded scan budget. Sets bOutScanTruncated when the cap cut
     *  the scan (issue #225), so callers can tell an incomplete result from an exhausted one. */
    static TArray<AActor*> CollectFilteredActors(
        UWorld* World,
        const TSharedPtr<FJsonObject>& Params,
        FCortexCommandResult& OutError,
        int32 MaxScanCount = 10000,
        bool* bOutScanTruncated = nullptr);
};
