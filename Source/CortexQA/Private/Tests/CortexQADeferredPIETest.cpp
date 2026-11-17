#include "Algo/AllOf.h"
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
    constexpr double CortexQAPIEWaitSeconds = 15.0;

    FCortexCommandRouter CreateQARouterDeferredPIE()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.1"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }

    /** Latent predicate that finishes once PIE has begun play with a possessed pawn, or reports a timeout. */
    TFunction<bool()> WaitForPIEPlaying(FAutomationTestBase* Test)
    {
        const double StartTime = FPlatformTime::Seconds();
        return [Test, StartTime]()
        {
            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
            if (PlayWorld != nullptr && PlayWorld->HasBegunPlay())
            {
                const APlayerController* PC = PlayWorld->GetFirstPlayerController();
                if (PC != nullptr && PC->GetPawn() != nullptr)
                {
                    return true;
                }
            }
            if (FPlatformTime::Seconds() - StartTime > CortexQAPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to begin play with a player pawn"));
                return true;
            }
            return false;
        };
    }

    TFunction<bool()> WaitForPIEStopped(FAutomationTestBase* Test)
    {
        // The deadline starts when the predicate first runs, not when the latent command is queued.
        TSharedRef<double> StartTime = MakeShared<double>(0.0);
        return [Test, StartTime]()
        {
            if (*StartTime == 0.0)
            {
                *StartTime = FPlatformTime::Seconds();
            }
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                return true;
            }
            if (FPlatformTime::Seconds() - *StartTime > CortexQAPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to stop"));
                return true;
            }
            return false;
        };
    }

    struct FCortexQADeferredOpState
    {
        FString Command;
        double RequestedSeconds = 0.0;
        FCortexCommandResult Immediate;
        TOptional<FCortexCommandResult> Final;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAPIEDeferredOpsLifecycleTest,
    "Cortex.QA.PIE.DeferredOpsLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAPIEDeferredOpsLifecycleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TSharedRef<TArray<TSharedRef<FCortexQADeferredOpState>>> Ops =
        MakeShared<TArray<TSharedRef<FCortexQADeferredOpState>>>();

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEPlaying(this)));

    // Start interact/move_to/wait_for with spans at or past the transport's 30 s default.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Ops]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        const APawn* Pawn = PlayWorld != nullptr && PlayWorld->GetFirstPlayerController() != nullptr
            ? PlayWorld->GetFirstPlayerController()->GetPawn()
            : nullptr;
        if (Pawn == nullptr)
        {
            AddError(TEXT("No player pawn to drive deferred QA commands"));
            return true;
        }

        const FVector FarTarget = Pawn->GetActorLocation() + FVector(1000000.0, 0.0, 0.0);
        TArray<TSharedPtr<FJsonValue>> TargetArray;
        TargetArray.Add(MakeShared<FJsonValueNumber>(FarTarget.X));
        TargetArray.Add(MakeShared<FJsonValueNumber>(FarTarget.Y));
        TargetArray.Add(MakeShared<FJsonValueNumber>(FarTarget.Z));

        TSharedPtr<FJsonObject> InteractParams = MakeShared<FJsonObject>();
        InteractParams->SetArrayField(TEXT("target"), TargetArray);
        InteractParams->SetNumberField(TEXT("duration"), 30.0);

        TSharedPtr<FJsonObject> MoveParams = MakeShared<FJsonObject>();
        MoveParams->SetArrayField(TEXT("target"), TargetArray);
        MoveParams->SetNumberField(TEXT("timeout"), 45.0);

        TSharedPtr<FJsonObject> WaitParams = MakeShared<FJsonObject>();
        WaitParams->SetStringField(TEXT("type"), TEXT("delay"));
        WaitParams->SetNumberField(TEXT("timeout"), 60.0);

        const TArray<TTuple<FString, double, TSharedPtr<FJsonObject>>> Requests = {
            MakeTuple(FString(TEXT("qa.interact")), 30.0, InteractParams),
            MakeTuple(FString(TEXT("qa.move_to")), 45.0, MoveParams),
            MakeTuple(FString(TEXT("qa.wait_for")), 60.0, WaitParams),
        };

        FCortexCommandRouter Router = CreateQARouterDeferredPIE();
        for (const TTuple<FString, double, TSharedPtr<FJsonObject>>& Request : Requests)
        {
            TSharedRef<FCortexQADeferredOpState> State = MakeShared<FCortexQADeferredOpState>();
            State->Command = Request.Get<0>();
            State->RequestedSeconds = Request.Get<1>();
            State->Immediate = Router.Execute(State->Command, Request.Get<2>(),
                [State](FCortexCommandResult Final)
                {
                    if (!State->Final.IsSet())
                    {
                        State->Final = MoveTemp(Final);
                    }
                });
            Ops->Add(State);

            TestTrue(*FString::Printf(TEXT("%s should be deferred"), *State->Command), State->Immediate.bIsDeferred);
            TestTrue(
                *FString::Printf(TEXT("%s transport timeout (%.1f s) should exceed its requested %.1f s"),
                    *State->Command, State->Immediate.DeferredTimeoutSeconds, State->RequestedSeconds),
                State->Immediate.DeferredTimeoutSeconds > State->RequestedSeconds);
        }
        return true;
    }));

    // Let a few polls run so termination is observed mid-operation, not before the first poll.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Ops]()
    {
        for (const TSharedRef<FCortexQADeferredOpState>& State : *Ops)
        {
            TestFalse(*FString::Printf(TEXT("%s should still be pending before PIE ends"), *State->Command),
                State->Final.IsSet());
        }
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEStopped(this)));

    // Every pending operation must answer promptly once PIE has ended.
    const double TerminationDeadlineSeconds = 5.0;
    TSharedRef<double> WaitStart = MakeShared<double>(0.0);
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Ops, WaitStart, TerminationDeadlineSeconds]()
    {
        if (*WaitStart == 0.0)
        {
            *WaitStart = FPlatformTime::Seconds();
        }

        const bool bAllAnswered = Algo::AllOf(*Ops, [](const TSharedRef<FCortexQADeferredOpState>& State)
        {
            return State->Final.IsSet();
        });
        if (!bAllAnswered && FPlatformTime::Seconds() - *WaitStart < TerminationDeadlineSeconds)
        {
            return false;
        }

        for (const TSharedRef<FCortexQADeferredOpState>& State : *Ops)
        {
            if (!State->Final.IsSet())
            {
                AddError(FString::Printf(TEXT("%s did not answer within %.0f s after PIE ended"),
                    *State->Command, TerminationDeadlineSeconds));
                continue;
            }
            TestFalse(*FString::Printf(TEXT("%s should fail after PIE ends"), *State->Command),
                State->Final->bSuccess);
            TestEqual(*FString::Printf(TEXT("%s should report PIE_TERMINATED"), *State->Command),
                State->Final->ErrorCode, FString(CortexErrorCodes::PIETerminated));
        }
        return true;
    }));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAPIEObserveStateMaxActorsClampTest,
    "Cortex.QA.PIE.ObserveStateMaxActorsClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAPIEObserveStateMaxActorsClampTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEPlaying(this)));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        const APawn* Pawn = PlayWorld != nullptr && PlayWorld->GetFirstPlayerController() != nullptr
            ? PlayWorld->GetFirstPlayerController()->GetPawn()
            : nullptr;
        if (Pawn == nullptr)
        {
            AddError(TEXT("No player pawn to observe from"));
            return true;
        }

        // More observable actors than the cap, all next to the player (PIE teardown destroys them).
        constexpr int32 SpawnCount = 210;
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        for (int32 Index = 0; Index < SpawnCount; ++Index)
        {
            const FVector Location = Pawn->GetActorLocation() + FVector(100.0 + Index, 0.0, 0.0);
            if (PlayWorld->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, SpawnParams) == nullptr)
            {
                AddError(TEXT("Failed to spawn observation fixture actor"));
                return true;
            }
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetNumberField(TEXT("max_actors"), 1000.0);
        Params->SetNumberField(TEXT("radius"), 100000.0);

        FCortexCommandRouter Router = CreateQARouterDeferredPIE();
        const FCortexCommandResult Result = Router.Execute(TEXT("qa.observe_state"), Params);
        TestTrue(TEXT("observe_state should succeed"), Result.bSuccess);
        if (Result.bSuccess && Result.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
            TestTrue(TEXT("observe_state should return nearby_actors"),
                Result.Data->TryGetArrayField(TEXT("nearby_actors"), Actors));
            if (Actors != nullptr)
            {
                TestEqual(TEXT("max_actors above the cap should return exactly 200 actors"), Actors->Num(), 200);
            }
        }
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForPIEStopped(this)));
    return true;
}
