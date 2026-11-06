// Source/CortexQA/Private/Tests/CortexQACommandCutoverTest.cpp
//
// Regressions for the CortexReplay QA/Frontend cutover. The four legacy QA
// recording commands (start_recording, stop_recording, replay_session,
// cancel_replay) must no longer execute or be advertised, while the retained QA
// semantic scenario commands (world/setup/action/assert) must keep routing to
// their handlers.
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateCutoverQARouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.1"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }

    const TArray<FString>& LegacyRecordingCommandIds()
    {
        static const TArray<FString> Commands = {
            TEXT("qa.start_recording"),
            TEXT("qa.stop_recording"),
            TEXT("qa.replay_session"),
            TEXT("qa.cancel_replay"),
        };
        return Commands;
    }

    const TArray<FString>& LegacyRecordingCommandNames()
    {
        static const TArray<FString> Names = {
            TEXT("start_recording"),
            TEXT("stop_recording"),
            TEXT("replay_session"),
            TEXT("cancel_replay"),
        };
        return Names;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALegacyRecordingCommandsRejectedTest,
    "Cortex.QA.CommandCutover.LegacyRecordingCommandsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQALegacyRecordingCommandsRejectedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCommandRouter Router = CreateCutoverQARouter();

    for (const FString& Command : LegacyRecordingCommandIds())
    {
        // Supply a path so a legacy replay_session cannot be mistaken for a
        // missing-parameter rejection: the command itself must be gone.
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), TEXT("/nonexistent/cutover.json"));

        const FCortexCommandResult Result = Router.Execute(Command, Params);

        TestFalse(FString::Printf(TEXT("%s must no longer execute"), *Command), Result.bSuccess);
        TestEqual(FString::Printf(TEXT("%s must report UNKNOWN_COMMAND"), *Command),
            Result.ErrorCode, CortexErrorCodes::UnknownCommand);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALegacyRecordingCommandsUnregisteredTest,
    "Cortex.QA.CommandCutover.LegacyRecordingCommandsUnregistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQALegacyRecordingCommandsUnregisteredTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexQACommandHandler Handler;
    const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

    TSet<FString> Names;
    for (const FCortexCommandInfo& Info : Commands)
    {
        Names.Add(Info.Name);
    }

    for (const FString& Command : LegacyRecordingCommandNames())
    {
        TestFalse(FString::Printf(TEXT("%s must not be advertised"), *Command), Names.Contains(Command));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQARetainedSemanticScenarioStillRoutesTest,
    "Cortex.QA.CommandCutover.RetainedSemanticScenarioStillRoutes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQARetainedSemanticScenarioStillRoutesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCommandRouter Router = CreateCutoverQARouter();

    struct FScenarioStep
    {
        FString Command;
        TSharedPtr<FJsonObject> Params;
    };

    TArray<FScenarioStep> Scenario;
    Scenario.Add(FScenarioStep{ TEXT("qa.observe_state"), MakeShared<FJsonObject>() });

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("location"), {
            MakeShared<FJsonValueNumber>(0.0),
            MakeShared<FJsonValueNumber>(0.0),
            MakeShared<FJsonValueNumber>(100.0)
        });
        Scenario.Add(FScenarioStep{ TEXT("qa.teleport_player"), Params });
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetNumberField(TEXT("seed"), 42);
        Scenario.Add(FScenarioStep{ TEXT("qa.set_random_seed"), Params });
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("target"), TEXT("AnyActor"));
        Scenario.Add(FScenarioStep{ TEXT("qa.look_at"), Params });
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("target"), {
            MakeShared<FJsonValueNumber>(100.0),
            MakeShared<FJsonValueNumber>(0.0),
            MakeShared<FJsonValueNumber>(0.0)
        });
        Scenario.Add(FScenarioStep{ TEXT("qa.move_to"), Params });
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("delay"));
        Params->SetNumberField(TEXT("timeout"), 0.1);
        Scenario.Add(FScenarioStep{ TEXT("qa.wait_for"), Params });
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("delay"));
        Params->SetBoolField(TEXT("expected"), true);
        Scenario.Add(FScenarioStep{ TEXT("qa.assert_state"), Params });
    }

    for (const FScenarioStep& Step : Scenario)
    {
        const FCortexCommandResult Result = Router.Execute(Step.Command, Step.Params);

        // PIE is not active in automation, so every retained semantic command
        // must reach its handler and be gated by PIE rather than UNKNOWN_COMMAND.
        TestEqual(FString::Printf(TEXT("%s must still reach its retained handler"), *Step.Command),
            Result.ErrorCode, CortexErrorCodes::PIENotActive);
    }

    return true;
}
