// Source/CortexQA/Private/Tests/CortexQAReplayTest.cpp
#include "Misc/AutomationTest.h"
#include "Recording/CortexQAReplaySequencer.h"
#include "Recording/CortexQASessionTypes.h"
#include "CortexCommandRouter.h"
#include "CortexQACommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAReplaySequencerCancelBeforeStartTest,
    "Cortex.QA.ReplaySequencer.CancelBeforeStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAReplaySequencerCancelBeforeStartTest::RunTest(const FString& Parameters)
{
    // Cancel on an idle sequencer should be safe (no-op)
    auto Sequencer = MakeShared<FCortexQAReplaySequencer>();
    Sequencer->Cancel(); // Should not crash
    TestEqual(TEXT("Step index should be 0 after cancel-before-start"), Sequencer->GetCurrentStepIndex(), 0);
    TestEqual(TEXT("Total steps should be 0"), Sequencer->GetTotalSteps(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAReplaySequencerEmptyStepsTest,
    "Cortex.QA.ReplaySequencer.EmptySteps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAReplaySequencerEmptyStepsTest::RunTest(const FString& Parameters)
{
    // Replaying empty session should complete immediately with success
    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
        MakeShared<FCortexQACommandHandler>());

    TArray<FCortexQAStep> EmptySteps;
    bool bCallbackFired = false;
    bool bCallbackSuccess = false;

    auto Sequencer = MakeShared<FCortexQAReplaySequencer>();
    Sequencer->Start(
        EmptySteps,
        Router,
        [&bCallbackFired, &bCallbackSuccess](FCortexCommandResult Result)
        {
            bCallbackFired = true;
            bCallbackSuccess = Result.bSuccess;
        },
        EQAReplayOnFailure::Continue);

    TestTrue(TEXT("Callback should fire for empty steps"), bCallbackFired);
    TestTrue(TEXT("Empty replay should succeed"), bCallbackSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAReplayUnknownStepTypeTest,
    "Cortex.QA.ReplaySequencer.UnknownStepType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAReplayUnknownStepTypeTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
        MakeShared<FCortexQACommandHandler>());

    FCortexQAStep BadStep;
    BadStep.Type = TEXT("nonexistent_step_type");
    BadStep.Params = MakeShared<FJsonObject>();

    TArray<FCortexQAStep> Steps = { BadStep };
    bool bDone = false;
    int32 StepsFailed = 0;

    auto Seq = MakeShared<FCortexQAReplaySequencer>();
    Seq->Start(Steps, Router,
        [&bDone, &StepsFailed](FCortexCommandResult Result)
        {
            bDone = true;
            if (Result.Data.IsValid())
            {
                StepsFailed = static_cast<int32>(Result.Data->GetNumberField(TEXT("steps_failed")));
            }
        },
        EQAReplayOnFailure::Continue);

    TestTrue(TEXT("Replay should complete"), bDone);
    TestEqual(TEXT("Unknown step type should be recorded as failed"), StepsFailed, 1);
    return true;
}
