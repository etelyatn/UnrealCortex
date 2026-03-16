// Source/CortexQA/Private/Recording/CortexQAReplaySequencer.cpp (STUB)
#include "Recording/CortexQAReplaySequencer.h"
#include "CortexCommandRouter.h"

void FCortexQAReplaySequencer::Start(
    const TArray<FCortexQAStep>& InSteps,
    FCortexCommandRouter& InRouter,
    FDeferredResponseCallback InFinalCallback,
    EQAReplayOnFailure InOnFailure)
{
    Steps = InSteps;
    CurrentStepIndex = 0;
    // Stub: immediately complete with success
    if (InFinalCallback)
    {
        FCortexCommandResult Result;
        Result.bSuccess = true;
        Result.Data = MakeShared<FJsonObject>();
        Result.Data->SetBoolField(TEXT("passed"), true);
        Result.Data->SetNumberField(TEXT("steps_passed"), 0);
        Result.Data->SetNumberField(TEXT("steps_failed"), 0);
        Result.Data->SetNumberField(TEXT("steps_total"), InSteps.Num());
        Result.Data->SetBoolField(TEXT("cancelled"), false);
        TArray<TSharedPtr<FJsonValue>> Empty;
        Result.Data->SetArrayField(TEXT("results"), Empty);
        InFinalCallback(MoveTemp(Result));
    }
}

void FCortexQAReplaySequencer::Cancel()
{
    bCancelled = true;
}
