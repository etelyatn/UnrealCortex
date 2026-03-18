// Source/CortexQA/Private/Recording/CortexQAReplaySequencer.h
#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Recording/CortexQASessionTypes.h"

class FCortexCommandRouter;

/**
 * Step-by-step replay executor for QA sessions.
 * Chains deferred commands via the command router, advancing one step at a time.
 */
class FCortexQAReplaySequencer : public TSharedFromThis<FCortexQAReplaySequencer>
{
public:
    /**
     * Start replaying steps.
     * IMPORTANT: Caller must hold a TSharedPtr to this sequencer before calling Start(),
     * because AdvanceStep() uses AsShared() for weak captures in ticker delegates.
     */
    void Start(
        const TArray<FCortexQAStep>& InSteps,
        FCortexCommandRouter& InRouter,
        FDeferredResponseCallback InFinalCallback,
        EQAReplayOnFailure InOnFailure = EQAReplayOnFailure::Continue,
        EQAReplayMode InReplayMode = EQAReplayMode::Smooth);

    /** Request cancellation. Current step runs to completion. */
    void Cancel();

    int32 GetCurrentStepIndex() const { return CurrentStepIndex; }
    int32 GetTotalSteps() const { return Steps.Num(); }

private:
    void AdvanceStep();
    void OnStepCompleted(FCortexCommandResult Result);
    void Complete(bool bSuccess);
    void OnPIEEnded(bool bIsSimulating);

    /** Map step type to router command name, considering replay mode. */
    FString StepTypeToCommand(const FString& StepType);

    /** Adapt params when converting position_snapshot to move_to in smooth mode. */
    TSharedPtr<FJsonObject> AdaptParamsForCommand(const FCortexQAStep& Step, const FString& Command) const;

    /** Schedule AdvanceStep after a delay (for timing-accurate replay of synchronous steps). */
    void ScheduleNextStep(double DelaySeconds);

    TArray<FCortexQAStep> Steps;
    int32 CurrentStepIndex = 0;
    FThreadSafeBool bCancelled;
    EQAReplayOnFailure OnFailurePolicy = EQAReplayOnFailure::Continue;
    EQAReplayMode ReplayMode = EQAReplayMode::Smooth;
    bool bFirstPositionSnapshotSeen = false;
    TArray<FCortexQAStepResult> Results;
    FCortexCommandRouter* Router = nullptr;
    FDeferredResponseCallback FinalCallback;
    FDelegateHandle EndPIEHandle;
    bool bAdvancing = false;
    bool bCompleted = false;
};
