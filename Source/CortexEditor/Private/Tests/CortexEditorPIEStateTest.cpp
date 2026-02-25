#include "Misc/AutomationTest.h"
#include "CortexEditorPIEState.h"
#include "Containers/Ticker.h"

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

	const uint32 Id1 = PIEState.RegisterPendingCallback([&CancelledCount, &LastErrorCode](FCortexCommandResult Result)
	{
		CancelledCount++;
		LastErrorCode = Result.ErrorCode;
	});
	const uint32 Id2 = PIEState.RegisterPendingCallback([&CancelledCount](FCortexCommandResult Result)
	{
		(void)Result;
		CancelledCount++;
	});
	(void)Id1;
	(void)Id2;

	PIEState.OnPIEEnded();

	TestEqual(TEXT("State should be Stopped after crash"), PIEState.GetState(), ECortexPIEState::Stopped);
	TestEqual(TEXT("Both callbacks should have been cancelled"), CancelledCount, 2);
	TestEqual(TEXT("Error code should be PIE_TERMINATED"), LastErrorCode, TEXT("PIE_TERMINATED"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateKeyedCallbackTest,
	"Cortex.Editor.PIEState.KeyedCallbackCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateKeyedCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;
	PIEState.SetState(ECortexPIEState::Playing);

	bool bCallback1Fired = false;
	bool bCallback2Fired = false;
	FString Callback1ErrorCode;

	const uint32 Id1 = PIEState.RegisterPendingCallback(
		[&bCallback1Fired, &Callback1ErrorCode](FCortexCommandResult Result)
		{
			bCallback1Fired = true;
			Callback1ErrorCode = Result.ErrorCode;
		});

	const uint32 Id2 = PIEState.RegisterPendingCallback(
		[&bCallback2Fired](FCortexCommandResult Result)
		{
			(void)Result;
			bCallback2Fired = true;
		});

	FCortexCommandResult SuccessResult;
	SuccessResult.bSuccess = true;
	PIEState.CompletePendingCallback(Id1, SuccessResult);

	TestTrue(TEXT("Callback 1 should have fired"), bCallback1Fired);
	TestFalse(TEXT("Callback 2 should NOT have fired"), bCallback2Fired);

	PIEState.OnPIEEnded();

	TestTrue(TEXT("Callback 2 should now have fired"), bCallback2Fired);
	(void)Id2;
	(void)Callback1ErrorCode;

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateCancelTokenTest,
	"Cortex.Editor.PIEState.CancelTokenInvalidatesOldToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateCancelTokenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;

	const TSharedRef<FThreadSafeBool> OldToken = PIEState.GetInputCancelToken();
	TestFalse(TEXT("Fresh token should be false"), *OldToken);

	bool bCallbackFired = false;
	FString CallbackErrorCode;
	PIEState.RegisterPendingCallback([&bCallbackFired, &CallbackErrorCode](FCortexCommandResult Result)
	{
		bCallbackFired = true;
		CallbackErrorCode = Result.ErrorCode;
	});

	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			return false;
		}),
		10.0f);
	PIEState.RegisterInputTickerHandle(Handle);

	PIEState.CancelAllInputTickers();

	TestTrue(TEXT("Old token should be true after cancel"), *OldToken);

	const TSharedRef<FThreadSafeBool> NewToken = PIEState.GetInputCancelToken();
	TestFalse(TEXT("New token should be false"), *NewToken);
	TestTrue(TEXT("Callback should have fired on cancel"), bCallbackFired);
	TestEqual(TEXT("Error code should be OperationCancelled"), CallbackErrorCode, FString(TEXT("OperationCancelled")));

	return true;
}
