// Source/CortexFrontend/Private/QA/CortexQATabTypes.h
#pragma once

#include "CoreMinimal.h"

/** Lightweight session info for UI list display (no step data loaded). */
struct FCortexQASessionListItem
{
    FString Name;
    FString Source;       // "recorded" or "ai_generated"
    FString MapPath;
    FDateTime RecordedAt;
    int32 StepCount = 0;
    bool bComplete = true;
    bool bLastRunPassed = false;
    bool bHasBeenRun = false;
    FString FilePath;
};
