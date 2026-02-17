#include "Misc/AutomationTest.h"
#include "CortexEditorPIEState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateInitTest,
	"Cortex.Editor.PIEState.InitialStateStopped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateInitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;

	TestEqual(TEXT("Initial state should be Stopped"),
		PIEState.GetState(), ECortexPIEState::Stopped);
	TestFalse(TEXT("Should not be in transition"), PIEState.IsInTransition());
	TestFalse(TEXT("PIE should not be active"), PIEState.IsActive());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateTransitionTest,
	"Cortex.Editor.PIEState.StateTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateTransitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;

	PIEState.SetState(ECortexPIEState::Starting);
	TestEqual(TEXT("Should be Starting"), PIEState.GetState(), ECortexPIEState::Starting);
	TestTrue(TEXT("Starting is a transition"), PIEState.IsInTransition());

	PIEState.SetState(ECortexPIEState::Playing);
	TestEqual(TEXT("Should be Playing"), PIEState.GetState(), ECortexPIEState::Playing);
	TestFalse(TEXT("Playing is not a transition"), PIEState.IsInTransition());
	TestTrue(TEXT("PIE should be active"), PIEState.IsActive());

	PIEState.SetState(ECortexPIEState::Paused);
	TestEqual(TEXT("Should be Paused"), PIEState.GetState(), ECortexPIEState::Paused);
	TestTrue(TEXT("PIE should still be active when paused"), PIEState.IsActive());

	PIEState.SetState(ECortexPIEState::Stopping);
	TestTrue(TEXT("Stopping is a transition"), PIEState.IsInTransition());

	PIEState.SetState(ECortexPIEState::Stopped);
	TestEqual(TEXT("Should be Stopped"), PIEState.GetState(), ECortexPIEState::Stopped);
	TestFalse(TEXT("PIE should not be active"), PIEState.IsActive());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateCrashRecoveryTest,
	"Cortex.Editor.PIEState.CrashRecoveryCancelsCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateCrashRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;
	PIEState.SetState(ECortexPIEState::Playing);

	int32 CancelledCount = 0;
	FString LastErrorCode;

	PIEState.RegisterPendingCallback([&CancelledCount, &LastErrorCode](FCortexCommandResult Result)
	{
		CancelledCount++;
		LastErrorCode = Result.ErrorCode;
	});
	PIEState.RegisterPendingCallback([&CancelledCount](FCortexCommandResult Result)
	{
		(void)Result;
		CancelledCount++;
	});

	PIEState.OnPIEEnded();

	TestEqual(TEXT("State should be Stopped after crash"), PIEState.GetState(), ECortexPIEState::Stopped);
	TestEqual(TEXT("Both callbacks should have been cancelled"), CancelledCount, 2);
	TestEqual(TEXT("Error code should be PIE_TERMINATED"), LastErrorCode, TEXT("PIE_TERMINATED"));

	return true;
}
