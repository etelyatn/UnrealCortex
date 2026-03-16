// Source/CortexQA/Private/Tests/CortexQARecorderTest.cpp
#include "Misc/AutomationTest.h"
#include "Recording/CortexQAInputRecorder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAInputRecorderStartStopTest,
    "Cortex.QA.InputRecorder.StartStop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAInputRecorderStartStopTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FCortexQAInputRecorder> Recorder = MakeShared<FCortexQAInputRecorder>();

    TestFalse(TEXT("Should not be recording initially"), Recorder->IsRecording());

    Recorder->StartRecording(0.0);
    TestTrue(TEXT("Should be recording after start"), Recorder->IsRecording());

    Recorder->StopRecording();
    TestFalse(TEXT("Should not be recording after stop"), Recorder->IsRecording());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAInputRecorderEmptyTest,
    "Cortex.QA.InputRecorder.EmptyAfterStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAInputRecorderEmptyTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FCortexQAInputRecorder> Recorder = MakeShared<FCortexQAInputRecorder>();
    Recorder->StartRecording(100.0);

    const TArray<FCortexQARawInputEvent>& Events = Recorder->GetRecordedEvents();
    TestEqual(TEXT("No events recorded without input"), Events.Num(), 0);

    Recorder->StopRecording();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAInputRecorderPassthroughTest,
    "Cortex.QA.InputRecorder.Passthrough",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQAInputRecorderPassthroughTest::RunTest(const FString& Parameters)
{
    // IInputProcessor Handle* methods should return false (passthrough)
    TSharedPtr<FCortexQAInputRecorder> Recorder = MakeShared<FCortexQAInputRecorder>();
    Recorder->StartRecording(0.0);

    // Construct a minimal FKeyEvent for testing
    const FKeyEvent KeyEvt(
        FKey(EKeys::W),
        FModifierKeysState(),
        0, // UserIndex
        false, // bIsRepeat
        0, // CharacterCode
        0  // KeyCode
    );

    FSlateApplication& App = FSlateApplication::Get();
    const bool bHandled = Recorder->HandleKeyDownEvent(App, KeyEvt);
    TestFalse(TEXT("Key events should pass through (return false)"), bHandled);

    const TArray<FCortexQARawInputEvent>& Events = Recorder->GetRecordedEvents();
    TestEqual(TEXT("Should have recorded one event"), Events.Num(), 1);
    TestEqual(TEXT("Event type should be key_down"), Events[0].Type, TEXT("key_down"));

    Recorder->StopRecording();
    return true;
}
