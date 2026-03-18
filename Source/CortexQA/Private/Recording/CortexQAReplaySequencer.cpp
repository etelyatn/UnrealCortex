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
    EQAReplayOnFailure InOnFailure,
    EQAReplayMode InReplayMode)
{
    Steps = InSteps;
    Router = &InRouter;
    FinalCallback = MoveTemp(InFinalCallback);
    OnFailurePolicy = InOnFailure;
    ReplayMode = InReplayMode;
    CurrentStepIndex = 0;
    bCancelled = false;
    bAdvancing = false;
    bCompleted = false;
    bFirstPositionSnapshotSeen = false;
    Results.Empty();

    UE_LOG(LogCortexQA, Log, TEXT("Replay: starting with %d steps, mode=%s, on_failure=%s"),
        Steps.Num(),
        ReplayMode == EQAReplayMode::Smooth ? TEXT("smooth") : TEXT("teleport"),
        OnFailurePolicy == EQAReplayOnFailure::Continue ? TEXT("continue") : TEXT("stop"));

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

        UE_LOG(LogCortexQA, Log, TEXT("Replay: step %d/%d — type='%s' command='%s'"),
            CurrentStepIndex, Steps.Num(), *Step.Type, *Command);

        // Adapt params if command was remapped (e.g., position_snapshot → move_to in smooth mode)
        TSharedPtr<FJsonObject> Params = AdaptParamsForCommand(Step, Command);

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
            // Check for logical failures (e.g., move_to arrived=false on timeout)
            bool bStepPassed = Result.bSuccess;
            if (bStepPassed && Result.Data.IsValid() && Result.Data->HasField(TEXT("arrived")))
            {
                if (!Result.Data->GetBoolField(TEXT("arrived")))
                {
                    bStepPassed = false;
                    Result.ErrorMessage = TEXT("Movement timed out before reaching target");
                }
            }

            // Process synchronously: record result and loop
            UE_LOG(LogCortexQA, Log, TEXT("Replay: step %d sync complete — %s%s"),
                CurrentStepIndex,
                bStepPassed ? TEXT("PASSED") : TEXT("FAILED"),
                Result.ErrorMessage.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Result.ErrorMessage));

            FCortexQAStepResult StepResult;
            StepResult.StepIndex = CurrentStepIndex;
            StepResult.StepType = Step.Type;
            StepResult.bPassed = bStepPassed;
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

            if (!bStepPassed && OnFailurePolicy == EQAReplayOnFailure::Stop)
            {
                Complete(false);
                return;
            }

            CurrentStepIndex++;

            // For synchronous steps, use timestamp-based delay to match recording timing
            if (CurrentStepIndex < Steps.Num())
            {
                const double PrevTimestamp = Step.TimestampMs;
                const double NextTimestamp = Steps[CurrentStepIndex].TimestampMs;
                const double DeltaSeconds = (NextTimestamp - PrevTimestamp) / 1000.0;

                if (DeltaSeconds > 0.05) // More than one frame? Schedule delayed advance
                {
                    ScheduleNextStep(DeltaSeconds);
                    return;
                }
            }

            continue; // very small delta or last step — advance immediately
        }

        // Deferred: wait for callback to call OnStepCompleted -> AdvanceStep
        return;
    }
}

void FCortexQAReplaySequencer::OnStepCompleted(FCortexCommandResult Result)
{
    // Check for logical failures (e.g., move_to arrived=false on timeout)
    bool bStepPassed = Result.bSuccess;
    if (bStepPassed && Result.Data.IsValid() && Result.Data->HasField(TEXT("arrived")))
    {
        if (!Result.Data->GetBoolField(TEXT("arrived")))
        {
            bStepPassed = false;
            Result.ErrorMessage = TEXT("Movement timed out before reaching target");
        }
    }

    const FString StepType = Steps.IsValidIndex(CurrentStepIndex) ? Steps[CurrentStepIndex].Type : TEXT("unknown");
    UE_LOG(LogCortexQA, Log, TEXT("Replay: step %d deferred complete — %s%s"),
        CurrentStepIndex,
        bStepPassed ? TEXT("PASSED") : TEXT("FAILED"),
        Result.ErrorMessage.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Result.ErrorMessage));

    // In smooth mode, set camera rotation after move_to completes for position_snapshot steps
    if (bStepPassed && ReplayMode == EQAReplayMode::Smooth && StepType == TEXT("position_snapshot"))
    {
        if (Steps.IsValidIndex(CurrentStepIndex) && Steps[CurrentStepIndex].Params.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* RotArray = nullptr;
            if (Steps[CurrentStepIndex].Params->TryGetArrayField(TEXT("rotation"), RotArray) && RotArray != nullptr && RotArray->Num() == 3)
            {
                UWorld* PIEWorld = (GEditor != nullptr) ? GEditor->PlayWorld : nullptr;
                if (PIEWorld != nullptr)
                {
                    APlayerController* PC = PIEWorld->GetFirstPlayerController();
                    if (PC != nullptr)
                    {
                        const FRotator Rot(
                            static_cast<float>((*RotArray)[0]->AsNumber()),
                            static_cast<float>((*RotArray)[1]->AsNumber()),
                            static_cast<float>((*RotArray)[2]->AsNumber()));
                        PC->SetControlRotation(Rot);
                    }
                }
            }
        }
    }

    FCortexQAStepResult StepResult;
    StepResult.StepIndex = CurrentStepIndex;
    StepResult.StepType = StepType;
    StepResult.bPassed = bStepPassed;
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
    if (!bStepPassed && OnFailurePolicy == EQAReplayOnFailure::Stop)
    {
        Complete(false);
        return;
    }

    CurrentStepIndex++;
    AdvanceStep();
}

void FCortexQAReplaySequencer::Complete(bool bSuccess)
{
    // Guard against double completion (PIE end + cancel race)
    if (bCompleted)
    {
        return;
    }
    bCompleted = true;

    // Unbind PIE delegate
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }

    UE_LOG(LogCortexQA, Log, TEXT("Replay: completed — overall %s, cancelled=%s"),
        bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"), (bool)bCancelled ? TEXT("true") : TEXT("false"));

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

FString FCortexQAReplaySequencer::StepTypeToCommand(const FString& StepType)
{
    if (StepType == TEXT("position_snapshot"))
    {
        if (ReplayMode == EQAReplayMode::Smooth)
        {
            // First position_snapshot teleports to start; subsequent ones walk
            if (!bFirstPositionSnapshotSeen)
            {
                bFirstPositionSnapshotSeen = true;
                return TEXT("qa.teleport_player");
            }
            return TEXT("qa.move_to");
        }
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

TSharedPtr<FJsonObject> FCortexQAReplaySequencer::AdaptParamsForCommand(
    const FCortexQAStep& Step,
    const FString& Command) const
{
    TSharedPtr<FJsonObject> Params = Step.Params.IsValid() ? Step.Params : MakeShared<FJsonObject>();

    // position_snapshot stores {location: [x,y,z], rotation: [p,y,r]}
    // move_to expects {target: [x,y,z]}
    if (Step.Type == TEXT("position_snapshot") && Command == TEXT("qa.move_to"))
    {
        TSharedPtr<FJsonObject> Adapted = MakeShared<FJsonObject>();
        if (Params->HasField(TEXT("location")))
        {
            Adapted->SetField(TEXT("target"), Params->TryGetField(TEXT("location")));
        }
        Adapted->SetNumberField(TEXT("acceptance_radius"), 50.0);
        Adapted->SetNumberField(TEXT("timeout"), 5.0);
        return Adapted;
    }

    return Params;
}

void FCortexQAReplaySequencer::ScheduleNextStep(double DelaySeconds)
{
    // Clamp delay to reasonable range (match recording pace but don't stall)
    DelaySeconds = FMath::Clamp(DelaySeconds, 0.05, 5.0);

    if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
    {
        Complete(false);
        return;
    }

    TWeakPtr<FCortexQAReplaySequencer> WeakSelf = AsShared();
    TSharedPtr<FTimerHandle> Handle = MakeShared<FTimerHandle>();
    GEditor->PlayWorld->GetTimerManager().SetTimer(
        *Handle,
        [Handle, WeakSelf]()
        {
            if (TSharedPtr<FCortexQAReplaySequencer> Self = WeakSelf.Pin())
            {
                Self->AdvanceStep();
            }
        },
        static_cast<float>(DelaySeconds),
        false);
}
