// Source/CortexQA/Private/Tests/CortexQARecordingCommandTest.cpp
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateRecordingRouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAStartRecordingNoPIETest,
    "Cortex.QA.RecordingCommands.StartRecordingNoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAStartRecordingNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateRecordingRouter();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), TEXT("Test"));

    FCortexCommandResult Result = Router.Execute(TEXT("qa.start_recording"), Params);

    TestFalse(TEXT("start_recording without PIE should fail"), Result.bSuccess);
    TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAStopRecordingNotRecordingTest,
    "Cortex.QA.RecordingCommands.StopRecordingNotRecording",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAStopRecordingNotRecordingTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateRecordingRouter();
    FCortexCommandResult Result = Router.Execute(TEXT("qa.stop_recording"), MakeShared<FJsonObject>());

    TestFalse(TEXT("stop_recording when not recording should fail"), Result.bSuccess);
    TestEqual(TEXT("Error should be INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQACancelReplayNotReplayingTest,
    "Cortex.QA.RecordingCommands.CancelReplayNotReplaying",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQACancelReplayNotReplayingTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateRecordingRouter();
    FCortexCommandResult Result = Router.Execute(TEXT("qa.cancel_replay"), MakeShared<FJsonObject>());

    TestFalse(TEXT("cancel_replay when not replaying should fail"), Result.bSuccess);
    TestEqual(TEXT("Error should be INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAReplayNoPIETest,
    "Cortex.QA.RecordingCommands.ReplayNoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAReplayNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateRecordingRouter();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), TEXT("/nonexistent/path.json"));

    FCortexCommandResult Result = Router.Execute(TEXT("qa.replay_session"), Params);

    TestFalse(TEXT("replay_session without PIE should fail"), Result.bSuccess);
    TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQANewCommandsRegisteredTest,
    "Cortex.QA.RecordingCommands.CommandsRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQANewCommandsRegisteredTest::RunTest(const FString& Parameters)
{
    FCortexQACommandHandler Handler;
    const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

    TSet<FString> Names;
    for (const FCortexCommandInfo& Info : Commands)
    {
        Names.Add(Info.Name);
    }

    TestTrue(TEXT("Should include start_recording"), Names.Contains(TEXT("start_recording")));
    TestTrue(TEXT("Should include stop_recording"), Names.Contains(TEXT("stop_recording")));
    TestTrue(TEXT("Should include replay_session"), Names.Contains(TEXT("replay_session")));
    TestTrue(TEXT("Should include cancel_replay"), Names.Contains(TEXT("cancel_replay")));
    TestEqual(TEXT("Total command count should be 15"), Commands.Num(), 15);

    return true;
}
