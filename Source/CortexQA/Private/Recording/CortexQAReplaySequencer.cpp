// Source/CortexQA/Private/Recording/CortexQAReplaySequencer.cpp
#include "Recording/CortexQAReplaySequencer.h"
#include "CortexQAModule.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexCoreDelegates.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"

void FCortexQAReplaySequencer::Start(
    const TArray<FCortexQAStep>& InSteps,
    FCortexCommandRouter& InRouter,
    FDeferredResponseCallback InFinalCallback,
    EQAReplayOnFailure InOnFailure)
{
    Steps = InSteps;
    Router = &InRouter;
    FinalCallback = MoveTemp(InFinalCallback);
    OnFailurePolicy = InOnFailure;
    CurrentStepIndex = 0;
    bCancelled = false;
    bAdvancing = false;
    Results.Empty();

    // Bind PIE lifecycle (only valid in editor)
    if (GEditor != nullptr)
    {
        EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FCortexQAReplaySequencer::OnPIEEnded);
    }

    AdvanceStep();
}

void FCortexQAReplaySequencer::Cancel()
{
    bCancelled = true;
}

void FCortexQAReplaySequencer::AdvanceStep()
{
    // Re-entrancy guard: only applies when an async command is in flight
    if (bAdvancing)
    {
        // Defer to next tick
        TWeakPtr<FCortexQAReplaySequencer> WeakSelf = AsShared();
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([WeakSelf](float)
            {
                if (TSharedPtr<FCortexQAReplaySequencer> Self = WeakSelf.Pin())
                {
                    Self->AdvanceStep();
                }
                return false; // one-shot
            }));
        return;
    }

    // Iterative loop: handles synchronous steps (unknown type, sync commands) without ticker
    while (true)
    {
        // Check cancellation
        if (bCancelled)
        {
            Complete(false);
            return;
        }

        // Check if all steps done
        if (CurrentStepIndex >= Steps.Num())
        {
            Complete(true);
            return;
        }

        const FCortexQAStep& Step = Steps[CurrentStepIndex];
        const FString Command = StepTypeToCommand(Step.Type);

        if (Command.IsEmpty())
        {
            UE_LOG(LogCortexQA, Warning, TEXT("Unknown step type: %s at index %d"), *Step.Type, CurrentStepIndex);
            // Record as failure, continue loop
            FCortexQAStepResult StepResult;
            StepResult.StepIndex = CurrentStepIndex;
            StepResult.StepType = Step.Type;
            StepResult.bPassed = false;
            StepResult.ErrorMessage = FString::Printf(TEXT("Unknown step type: %s"), *Step.Type);
            Results.Add(StepResult);
            CurrentStepIndex++;
            continue; // advance to next step in loop
        }

        // Fire progress delegate (only if CortexCore is loaded)
        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
        {
            FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
            TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
            Progress->SetStringField(TEXT("type"), TEXT("replay_progress"));
            Progress->SetNumberField(TEXT("current_step"), CurrentStepIndex);
            Progress->SetNumberField(TEXT("total_steps"), Steps.Num());
            Progress->SetStringField(TEXT("step_type"), Step.Type);
            Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);
        }

        // Execute step
        TSharedPtr<FJsonObject> Params = Step.Params.IsValid() ? Step.Params : MakeShared<FJsonObject>();

        TWeakPtr<FCortexQAReplaySequencer> WeakSelf = AsShared();

        // Mark as advancing before executing so re-entrancy is detected if callback fires synchronously
        bAdvancing = true;
        FCortexCommandResult Result = Router->Execute(
            Command,
            Params,
            [WeakSelf](FCortexCommandResult DeferredResult)
            {
                if (TSharedPtr<FCortexQAReplaySequencer> Self = WeakSelf.Pin())
                {
                    Self->OnStepCompleted(MoveTemp(DeferredResult));
                }
            });
        bAdvancing = false;

        // Handle synchronous completion
        if (!Result.bIsDeferred)
        {
            // Process synchronously: record result and loop
            FCortexQAStepResult StepResult;
            StepResult.StepIndex = CurrentStepIndex;
            StepResult.StepType = Step.Type;
            StepResult.bPassed = Result.bSuccess;
            StepResult.ErrorMessage = Result.ErrorMessage;
            Results.Add(StepResult);

            // Fire step_completed progress
            if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
            {
                FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
                TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
                Progress->SetStringField(TEXT("type"), TEXT("step_completed"));
                Progress->SetNumberField(TEXT("step_index"), CurrentStepIndex);
                Progress->SetStringField(TEXT("step_type"), StepResult.StepType);
                Progress->SetBoolField(TEXT("passed"), StepResult.bPassed);
                Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);
            }

            if (!Result.bSuccess && OnFailurePolicy == EQAReplayOnFailure::Stop)
            {
                Complete(false);
                return;
            }

            CurrentStepIndex++;
            continue; // loop to next step
        }

        // Deferred: wait for callback to call OnStepCompleted -> AdvanceStep
        return;
    }
}

void FCortexQAReplaySequencer::OnStepCompleted(FCortexCommandResult Result)
{
    FCortexQAStepResult StepResult;
    StepResult.StepIndex = CurrentStepIndex;
    StepResult.StepType = Steps.IsValidIndex(CurrentStepIndex) ? Steps[CurrentStepIndex].Type : TEXT("unknown");
    StepResult.bPassed = Result.bSuccess;
    StepResult.ErrorMessage = Result.ErrorMessage;
    Results.Add(StepResult);

    // Fire step_completed progress
    if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
    {
        FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
        TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
        Progress->SetStringField(TEXT("type"), TEXT("step_completed"));
        Progress->SetNumberField(TEXT("step_index"), CurrentStepIndex);
        Progress->SetStringField(TEXT("step_type"), StepResult.StepType);
        Progress->SetBoolField(TEXT("passed"), StepResult.bPassed);
        Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);
    }

    // Check failure policy
    if (!Result.bSuccess && OnFailurePolicy == EQAReplayOnFailure::Stop)
    {
        Complete(false);
        return;
    }

    CurrentStepIndex++;
    AdvanceStep();
}

void FCortexQAReplaySequencer::Complete(bool bSuccess)
{
    // Unbind PIE delegate
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }

    // Count results
    int32 Passed = 0;
    int32 Failed = 0;
    for (const FCortexQAStepResult& R : Results)
    {
        if (R.bPassed)
        {
            Passed++;
        }
        else
        {
            Failed++;
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("passed"), bSuccess && Failed == 0);
    Data->SetNumberField(TEXT("steps_passed"), Passed);
    Data->SetNumberField(TEXT("steps_failed"), Failed);
    Data->SetNumberField(TEXT("steps_total"), Steps.Num());
    Data->SetBoolField(TEXT("cancelled"), (bool)bCancelled);

    // Build step results array
    TArray<TSharedPtr<FJsonValue>> ResultsArr;
    for (const FCortexQAStepResult& R : Results)
    {
        TSharedPtr<FJsonObject> StepObj = MakeShared<FJsonObject>();
        StepObj->SetNumberField(TEXT("step_index"), R.StepIndex);
        StepObj->SetStringField(TEXT("step_type"), R.StepType);
        StepObj->SetBoolField(TEXT("passed"), R.bPassed);
        if (!R.ErrorMessage.IsEmpty())
        {
            StepObj->SetStringField(TEXT("error"), R.ErrorMessage);
        }
        ResultsArr.Add(MakeShared<FJsonValueObject>(StepObj));
    }
    Data->SetArrayField(TEXT("results"), ResultsArr);

    FCortexCommandResult FinalResult = FCortexCommandRouter::Success(Data);

    if (FinalCallback)
    {
        FinalCallback(MoveTemp(FinalResult));
    }
}

void FCortexQAReplaySequencer::OnPIEEnded(bool bIsSimulating)
{
    UE_LOG(LogCortexQA, Warning, TEXT("PIE ended during replay"));
    bCancelled = true;
    Complete(false);
}

FString FCortexQAReplaySequencer::StepTypeToCommand(const FString& StepType) const
{
    if (StepType == TEXT("position_snapshot"))
    {
        return TEXT("qa.teleport_player");
    }
    if (StepType == TEXT("move_to"))
    {
        return TEXT("qa.move_to");
    }
    if (StepType == TEXT("interact"))
    {
        return TEXT("qa.interact");
    }
    if (StepType == TEXT("look_at"))
    {
        return TEXT("qa.look_at");
    }
    if (StepType == TEXT("wait"))
    {
        return TEXT("qa.wait_for");
    }
    if (StepType == TEXT("assert"))
    {
        return TEXT("qa.assert_state");
    }
    if (StepType == TEXT("key_press"))
    {
        return TEXT("editor.inject_key");
    }
    return FString();
}
