#include "CortexCommandRouter.h"
#include "CortexQACommandHandler.h"
#include "CortexTypes.h"
#include "CortexQADelegateFixture.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
    constexpr double BroadcastPIEWaitSeconds = 15.0;

    struct FBroadcastFixture
    {
        AActor* Actor = nullptr;
        UCortexQADelegateFixtureComponent* Component = nullptr;
    };

    FCortexCommandRouter CreateBroadcastRouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.1"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }

    TFunction<bool()> BroadcastWaitForPIEPlaying(FAutomationTestBase* Test)
    {
        TSharedRef<double> StartTime = MakeShared<double>(0.0);
        return [Test, StartTime]()
        {
            if (*StartTime == 0.0)
            {
                *StartTime = FPlatformTime::Seconds();
            }
            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
            if (PlayWorld != nullptr && PlayWorld->HasBegunPlay())
            {
                return true;
            }
            if (FPlatformTime::Seconds() - *StartTime > BroadcastPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to begin play"));
                return true;
            }
            return false;
        };
    }

    TFunction<bool()> BroadcastWaitForPIEStopped(FAutomationTestBase* Test)
    {
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
            if (FPlatformTime::Seconds() - *StartTime > BroadcastPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to stop"));
                return true;
            }
            return false;
        };
    }

    /** Starts PIE, runs Body once against the play world, then ends PIE. */
    void RunInPIE(FAutomationTestBase* Test, TFunction<void(UWorld*)> Body)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(BroadcastWaitForPIEPlaying(Test)));
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Body]()
        {
            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
            if (PlayWorld == nullptr)
            {
                Test->AddError(TEXT("No PIE world available for the broadcast_delegate test"));
                return true;
            }
            Body(PlayWorld);
            return true;
        }));
        ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(BroadcastWaitForPIEStopped(Test)));
    }

    /** Spawns a PIE actor and adds the fixture component to it at runtime. */
    FBroadcastFixture SpawnBroadcastFixture(FAutomationTestBase* Test, UWorld* World)
    {
        FBroadcastFixture Fixture;

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Fixture.Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
        if (Fixture.Actor == nullptr)
        {
            Test->AddError(TEXT("Failed to spawn the fixture actor in the PIE world"));
            return Fixture;
        }

        Fixture.Component = NewObject<UCortexQADelegateFixtureComponent>(Fixture.Actor, TEXT("QADelegateFixture"));
        Fixture.Component->RegisterComponent();
        return Fixture;
    }

    FCortexCommandResult ExecuteBroadcast(
        const FBroadcastFixture& Fixture,
        const FString& DelegateName,
        const TSharedPtr<FJsonObject>& Args)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), Fixture.Actor->GetPathName());
        Params->SetStringField(TEXT("component"), Fixture.Component->GetName());
        Params->SetStringField(TEXT("delegate"), DelegateName);
        if (Args.IsValid())
        {
            Params->SetObjectField(TEXT("args"), Args);
        }

        FCortexCommandRouter Router = CreateBroadcastRouter();
        return Router.Execute(TEXT("qa.broadcast_delegate"), Params);
    }

    TSharedPtr<FJsonObject> MakeBroadcastArgs(const FBroadcastFixture& Fixture, const int32 Code)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("Target"), Fixture.Actor->GetPathName());
        Args->SetNumberField(TEXT("Code"), Code);
        return Args;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateInvokesBoundHandlersTest,
    "Cortex.QA.BroadcastDelegate.InvokesBoundHandlers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateInvokesBoundHandlersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }
        Fixture.Component->OnTargetChanged.AddDynamic(Fixture.Component, &UCortexQADelegateFixtureComponent::HandleTargetChanged);

        const FCortexCommandResult Result = ExecuteBroadcast(Fixture, TEXT("OnTargetChanged"), MakeBroadcastArgs(Fixture, 7));
        TestTrue(TEXT("broadcast_delegate should succeed"), Result.bSuccess);
        if (Result.bSuccess && Result.Data.IsValid())
        {
            TestEqual(TEXT("bound_count is the listener count before the broadcast"),
                static_cast<int32>(Result.Data->GetNumberField(TEXT("bound_count"))), 1);
            TestEqual(TEXT("reply names the actor"), Result.Data->GetStringField(TEXT("actor")), Fixture.Actor->GetName());
            TestEqual(TEXT("reply names the component"), Result.Data->GetStringField(TEXT("component")), Fixture.Component->GetName());
            TestEqual(TEXT("reply names the delegate"), Result.Data->GetStringField(TEXT("delegate")), FString(TEXT("OnTargetChanged")));
            TestTrue(TEXT("reply names the signature"),
                Result.Data->GetStringField(TEXT("signature")).Contains(TEXT("CortexQATestTargetChanged")));
            const TSharedPtr<FJsonObject>* ArgsEcho = nullptr;
            if (TestTrue(TEXT("reply echoes args"), Result.Data->TryGetObjectField(TEXT("args"), ArgsEcho) && ArgsEcho != nullptr))
            {
                TestEqual(TEXT("echoed Code"), static_cast<int32>((*ArgsEcho)->GetNumberField(TEXT("Code"))), 7);
            }
        }

        TestEqual(TEXT("handler ran exactly once"), Fixture.Component->CallCount, 1);
        TestEqual(TEXT("handler received Code"), Fixture.Component->LastCode, 7);
        TestTrue(TEXT("handler received Target"), Fixture.Component->LastTarget.Get() == Fixture.Actor);

        Fixture.Actor->Destroy();
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateRejectsMissingAndUnknownArgsTest,
    "Cortex.QA.BroadcastDelegate.RejectsMissingAndUnknownArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateRejectsMissingAndUnknownArgsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }
        Fixture.Component->OnTargetChanged.AddDynamic(Fixture.Component, &UCortexQADelegateFixtureComponent::HandleTargetChanged);

        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetNumberField(TEXT("Code"), 7);
        Args->SetNumberField(TEXT("Bogus"), 1);

        const FCortexCommandResult Result = ExecuteBroadcast(Fixture, TEXT("OnTargetChanged"), Args);
        TestFalse(TEXT("missing and unknown args should be refused"), Result.bSuccess);
        TestEqual(TEXT("error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("message names the unknown arg"), Result.ErrorMessage.Contains(TEXT("unknown arg 'Bogus'")));
        TestTrue(TEXT("message names the missing arg"), Result.ErrorMessage.Contains(TEXT("missing arg 'Target'")));
        TestTrue(TEXT("message lists Target:AActor*"), Result.ErrorMessage.Contains(TEXT("Target:AActor*")));
        TestTrue(TEXT("message lists Code:int32"), Result.ErrorMessage.Contains(TEXT("Code:int32")));
        TestEqual(TEXT("no handler ran"), Fixture.Component->CallCount, 0);

        // No args at all: every signature parameter is missing.
        const FCortexCommandResult NoArgsResult = ExecuteBroadcast(Fixture, TEXT("OnTargetChanged"), nullptr);
        TestFalse(TEXT("a call without args should be refused"), NoArgsResult.bSuccess);
        TestEqual(TEXT("no-args error code"), NoArgsResult.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("no-args message lists the expected args"),
            NoArgsResult.ErrorMessage.Contains(TEXT("Target:AActor*")) && NoArgsResult.ErrorMessage.Contains(TEXT("Code:int32")));
        TestEqual(TEXT("still no handler ran"), Fixture.Component->CallCount, 0);

        Fixture.Actor->Destroy();
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateRejectsUnconvertibleArgTest,
    "Cortex.QA.BroadcastDelegate.RejectsUnconvertibleArg",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateRejectsUnconvertibleArgTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }
        Fixture.Component->OnTargetChanged.AddDynamic(Fixture.Component, &UCortexQADelegateFixtureComponent::HandleTargetChanged);

        // An object parameter cannot be addressed by a number; the parameter storage must be released cleanly.
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetNumberField(TEXT("Target"), 5);
        Args->SetNumberField(TEXT("Code"), 7);

        const FCortexCommandResult Result = ExecuteBroadcast(Fixture, TEXT("OnTargetChanged"), Args);
        TestFalse(TEXT("an unconvertible arg should be refused"), Result.bSuccess);
        TestEqual(TEXT("error code"), Result.ErrorCode, CortexErrorCodes::InvalidValue);
        TestTrue(TEXT("message names the arg"), Result.ErrorMessage.Contains(TEXT("'Target'")));
        TestEqual(TEXT("no handler ran"), Fixture.Component->CallCount, 0);

        Fixture.Actor->Destroy();
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateRejectsNonDelegatePropertyTest,
    "Cortex.QA.BroadcastDelegate.RejectsNonDelegateProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateRejectsNonDelegatePropertyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }

        const FCortexCommandResult Result = ExecuteBroadcast(Fixture, TEXT("PlainValue"), nullptr);
        TestFalse(TEXT("a non-delegate property should be refused"), Result.bSuccess);
        TestEqual(TEXT("error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("message names the property type"), Result.ErrorMessage.Contains(TEXT("int32")));

        const FCortexCommandResult MissingResult = ExecuteBroadcast(Fixture, TEXT("NoSuchDelegate"), nullptr);
        TestFalse(TEXT("an unknown property should be refused"), MissingResult.bSuccess);
        TestEqual(TEXT("unknown property error code"), MissingResult.ErrorCode, CortexErrorCodes::PropertyNotFound);

        Fixture.Actor->Destroy();
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateRequiresPIETest,
    "Cortex.QA.BroadcastDelegate.RequiresPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateRequiresPIETest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCommandRouter Router = CreateBroadcastRouter();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("AnyActor"));
    Params->SetStringField(TEXT("delegate"), TEXT("OnTargetChanged"));
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.broadcast_delegate"), Params);
    TestFalse(TEXT("broadcast_delegate should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("broadcast_delegate should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateSparseSelfRemovingListenerTest,
    "Cortex.QA.BroadcastDelegate.SparseSelfRemovingListener",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateSparseSelfRemovingListenerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }
        Fixture.Component->OnSparseChanged.AddDynamic(Fixture.Component, &UCortexQADelegateFixtureComponent::HandleSparseRecording);
        Fixture.Component->OnSparseChanged.AddDynamic(Fixture.Component, &UCortexQADelegateFixtureComponent::HandleSparseSelfRemoving);

        // The self-removing listener unbinds itself while the broadcast is running.
        const FCortexCommandResult First = ExecuteBroadcast(Fixture, TEXT("OnSparseChanged"), MakeBroadcastArgs(Fixture, 11));
        TestTrue(TEXT("first sparse broadcast should succeed"), First.bSuccess);
        if (First.bSuccess && First.Data.IsValid())
        {
            TestEqual(TEXT("bound_count counts both listeners before the broadcast"),
                static_cast<int32>(First.Data->GetNumberField(TEXT("bound_count"))), 2);
        }
        TestEqual(TEXT("recording listener ran once"), Fixture.Component->SparseRecordCount, 1);
        TestEqual(TEXT("self-removing listener ran once"), Fixture.Component->SparseSelfRemovingCount, 1);
        TestEqual(TEXT("recording listener received Code"), Fixture.Component->LastCode, 11);

        // The removal took effect: only the recording listener remains.
        const FCortexCommandResult Second = ExecuteBroadcast(Fixture, TEXT("OnSparseChanged"), MakeBroadcastArgs(Fixture, 12));
        TestTrue(TEXT("second sparse broadcast should succeed"), Second.bSuccess);
        if (Second.bSuccess && Second.Data.IsValid())
        {
            TestEqual(TEXT("bound_count reflects the removal"),
                static_cast<int32>(Second.Data->GetNumberField(TEXT("bound_count"))), 1);
        }
        TestEqual(TEXT("recording listener ran again"), Fixture.Component->SparseRecordCount, 2);
        TestEqual(TEXT("self-removing listener did not run again"), Fixture.Component->SparseSelfRemovingCount, 1);
        TestEqual(TEXT("recording listener received the second Code"), Fixture.Component->LastCode, 12);

        Fixture.Actor->Destroy();
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQABroadcastDelegateUnboundSparseReportsZeroTest,
    "Cortex.QA.BroadcastDelegate.UnboundSparseReportsZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQABroadcastDelegateUnboundSparseReportsZeroTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    RunInPIE(this, [this](UWorld* World)
    {
        const FBroadcastFixture Fixture = SpawnBroadcastFixture(this, World);
        if (Fixture.Component == nullptr)
        {
            return;
        }

        const FCortexCommandResult Result = ExecuteBroadcast(Fixture, TEXT("OnSparseChanged"), MakeBroadcastArgs(Fixture, 5));
        TestTrue(TEXT("broadcasting an unbound sparse delegate should succeed"), Result.bSuccess);
        if (Result.bSuccess && Result.Data.IsValid())
        {
            TestEqual(TEXT("an unbound sparse delegate has zero listeners"),
                static_cast<int32>(Result.Data->GetNumberField(TEXT("bound_count"))), 0);
        }
        TestEqual(TEXT("no recording listener ran"), Fixture.Component->SparseRecordCount, 0);

        Fixture.Actor->Destroy();
    });
    return true;
}
