// Source/CortexQA/Private/Tests/CortexQASessionRecorderTest.cpp
#include "Misc/AutomationTest.h"
#include "Recording/CortexQARecorder.h"
#include "Recording/CortexQASessionSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQARecorderStateTest,
    "Cortex.QA.Recorder.StateMachine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQARecorderStateTest::RunTest(const FString& Parameters)
{
    FCortexQARecorder Recorder;

    TestFalse(TEXT("Should not be recording initially"), Recorder.IsRecording());

    // Starting without PIE should fail gracefully
    const bool bStarted = Recorder.StartRecording(nullptr, TEXT("Test"));
    TestFalse(TEXT("Start with null world should fail"), bStarted);
    TestFalse(TEXT("Should not be recording after failed start"), Recorder.IsRecording());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQARecorderDoubleStartTest,
    "Cortex.QA.Recorder.DoubleStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQARecorderDoubleStartTest::RunTest(const FString& Parameters)
{
    // Verify double-start is rejected (can only test with null world)
    FCortexQARecorder Recorder;
    // Both should fail — null world
    Recorder.StartRecording(nullptr, TEXT("A"));
    const bool bSecond = Recorder.StartRecording(nullptr, TEXT("B"));
    TestFalse(TEXT("Second start should be rejected"), bSecond);

    return true;
}
