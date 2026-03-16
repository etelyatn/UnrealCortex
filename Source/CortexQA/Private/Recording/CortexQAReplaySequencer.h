// Source/CortexQA/Private/Recording/CortexQAReplaySequencer.h (STUB)
#pragma once
#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Recording/CortexQASessionTypes.h"

class FCortexCommandRouter;

class FCortexQAReplaySequencer : public TSharedFromThis<FCortexQAReplaySequencer>
{
public:
    void Start(
        const TArray<FCortexQAStep>& InSteps,
        FCortexCommandRouter& InRouter,
        FDeferredResponseCallback InFinalCallback,
        EQAReplayOnFailure InOnFailure = EQAReplayOnFailure::Continue);
    void Cancel();
    int32 GetCurrentStepIndex() const { return CurrentStepIndex; }
    int32 GetTotalSteps() const { return Steps.Num(); }

private:
    TArray<FCortexQAStep> Steps;
    int32 CurrentStepIndex = 0;
    FThreadSafeBool bCancelled;
};
