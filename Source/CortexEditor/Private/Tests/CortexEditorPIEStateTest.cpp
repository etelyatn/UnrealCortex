#include "Misc/AutomationTest.h"
#include "CortexEditorPIEState.h"
#include "Containers/Ticker.h"
#include "Editor.h"

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
	PIEState.RegisterPendingInputCallback([&bCallbackFired, &CallbackErrorCode](FCortexCommandResult Result)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateCancelInputTickerDoesNotCancelGeneralCallbacksTest,
	"Cortex.Editor.PIEState.CancelInputTickerDoesNotCancelGeneralCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateCancelInputTickerDoesNotCancelGeneralCallbacksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;

	bool bGeneralCallbackFired = false;
	bool bInputCallbackFired = false;
	FString GeneralErrorCode;
	FString InputErrorCode;

	PIEState.RegisterPendingCallback([&bGeneralCallbackFired, &GeneralErrorCode](FCortexCommandResult Result)
	{
		bGeneralCallbackFired = true;
		GeneralErrorCode = Result.ErrorCode;
	});

	PIEState.RegisterPendingInputCallback([&bInputCallbackFired, &InputErrorCode](FCortexCommandResult Result)
	{
		bInputCallbackFired = true;
		InputErrorCode = Result.ErrorCode;
	});

	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			return false;
		}),
		10.0f);
	PIEState.RegisterInputTickerHandle(Handle);

	PIEState.CancelAllInputTickers();

	TestFalse(TEXT("General callback should remain pending"), bGeneralCallbackFired);
	TestTrue(TEXT("Input callback should be completed on cancel"), bInputCallbackFired);
	TestEqual(TEXT("Input callback should be OperationCancelled"), InputErrorCode, FString(TEXT("OperationCancelled")));

	FCortexCommandResult SuccessResult;
	SuccessResult.bSuccess = true;
	PIEState.CompletePendingCallbacks(SuccessResult);

	TestTrue(TEXT("General callback should complete on general broadcast"), bGeneralCallbackFired);
	TestEqual(TEXT("General callback error code should remain empty"), GeneralErrorCode, FString(TEXT("")));

	return true;
}

// UEditorEngine::EndPlayMap() broadcasts FEditorDelegates::CancelPIE unconditionally after EndPIE
// (PlayLevel.cpp), so every completed stop leaves a deferred OnPIEEnded() queued by HandleCancelPIE().
// A start_pie request admitted before that cleanup runs must not be terminated by it: the tracker has
// to stay Starting, keep the new caller pending, and leave the successor session's input untouched.
// Before the fix the stale cleanup completed the new callback with PIE_TERMINATED and forced the
// tracker to Stopped while the engine went on to start PIE (post/start response contradiction).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateStaleCancelKeepsNewRequestTest,
	"Cortex.Editor.PIEState.StaleCancelDoesNotTerminateNewStartRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateStaleCancelKeepsNewRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Captured results are declared before the tracker so that a callback still pending on any
	// failure path is completed by ~FCortexEditorPIEState() while these locals are still alive.
	bool bStartCallbackFired = false;
	int32 StartCallbackCount = 0;
	FString StartErrorCode;

	FCortexEditorPIEState PIEState;
	PIEState.BindDelegates();

	// Cancellation of the previous session, deferred exactly as EndPlayMap leaves it.
	FEditorDelegates::CancelPIE.Broadcast();

	// A new start request is admitted before the deferred cleanup runs. start_pie registers its
	// deferred caller and then enters Starting (see FCortexEditorPIEOps::StartPIE).
	const uint32 CallbackId = PIEState.RegisterPendingCallback(
		[&bStartCallbackFired, &StartCallbackCount, &StartErrorCode](FCortexCommandResult Result)
		{
			bStartCallbackFired = true;
			++StartCallbackCount;
			StartErrorCode = Result.ErrorCode;
		});
	(void)CallbackId;
	PIEState.SetState(ECortexPIEState::Starting);

	const TSharedRef<FThreadSafeBool> SuccessorInputToken = PIEState.GetInputCancelToken();

	FTSTicker::GetCoreTicker().Tick(0.0f);

	TestFalse(TEXT("Stale cancel must not complete the new start request"), bStartCallbackFired);
	TestEqual(TEXT("New request must stay Starting"), PIEState.GetState(), ECortexPIEState::Starting);
	// Identity, not value: a replacement token would also read false, so compare the objects.
	TestTrue(TEXT("Successor input token must not be replaced"),
		&PIEState.GetInputCancelToken().Get() == &SuccessorInputToken.Get());
	TestFalse(TEXT("Successor input token must not be cancelled"), *SuccessorInputToken);

	// The request still completes normally, which is what HandlePostPIEStarted does on success.
	PIEState.SetState(ECortexPIEState::Playing);
	FCortexCommandResult SuccessResult;
	SuccessResult.bSuccess = true;
	PIEState.CompletePendingCallbacks(SuccessResult);

	TestEqual(TEXT("Start callback should complete exactly once"), StartCallbackCount, 1);
	TestTrue(TEXT("Start callback should succeed"), StartErrorCode.IsEmpty());
	TestEqual(TEXT("Tracked state should be Playing"), PIEState.GetState(), ECortexPIEState::Playing);

	return true;
}

// A cancellation that still belongs to the request in flight must terminate it, both when it arrives
// before the engine's PreBeginPIE (queued Cortex start, rejected while dequeuing: PlayLevel.cpp save
// failure path) and after it (authorizer/compilation rejection). PreBeginPIE for the same request
// repeats Starting without beginning a new request, so it must not invalidate the cancellation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateCancelTerminatesPendingStartRequestTest,
	"Cortex.Editor.PIEState.CancelTerminatesPendingStartRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateCancelTerminatesPendingStartRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	bool bStartCallbackFired = false;
	FString StartErrorCode;

	FCortexEditorPIEState PIEState;
	PIEState.BindDelegates();

	PIEState.RegisterPendingCallback([&bStartCallbackFired, &StartErrorCode](FCortexCommandResult Result)
	{
		bStartCallbackFired = true;
		StartErrorCode = Result.ErrorCode;
	});
	// start_pie boundary, then the engine's PreBeginPIE for that same request.
	PIEState.SetState(ECortexPIEState::Starting);
	PIEState.SetState(ECortexPIEState::Starting);

	FEditorDelegates::CancelPIE.Broadcast();

	TestFalse(TEXT("Cancellation stays deferred out of the broadcast stack"), bStartCallbackFired);
	TestEqual(TEXT("Tracker is still Starting until the deferred cleanup runs"),
		PIEState.GetState(), ECortexPIEState::Starting);

	FTSTicker::GetCoreTicker().Tick(0.0f);

	TestTrue(TEXT("Pending start request should be terminated"), bStartCallbackFired);
	TestEqual(TEXT("Error code should be PIE_TERMINATED"), StartErrorCode, TEXT("PIE_TERMINATED"));
	TestEqual(TEXT("Tracker should be Stopped"), PIEState.GetState(), ECortexPIEState::Stopped);

	return true;
}

// Duplicate cancellations must collapse onto one cleanup: an armed ticker is removed before the new
// one is armed, so no raw-this ticker is leaked, and destroying the tracker removes the single
// pending cleanup instead of leaving it to run against freed state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateDuplicateCancelCoalescesTest,
	"Cortex.Editor.PIEState.DuplicateCancelCoalesces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateDuplicateCancelCoalescesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	int32 TerminatedCount = 0;
	FString LastErrorCode;

	{
		FCortexEditorPIEState PIEState;
		PIEState.BindDelegates();

		PIEState.RegisterPendingCallback([&TerminatedCount, &LastErrorCode](FCortexCommandResult Result)
		{
			++TerminatedCount;
			LastErrorCode = Result.ErrorCode;
		});
		PIEState.SetState(ECortexPIEState::Starting);

		// EndPlayMap cancels once, but a stop and an engine-side start rejection can both arrive.
		FEditorDelegates::CancelPIE.Broadcast();
		FEditorDelegates::CancelPIE.Broadcast();

		TestEqual(TEXT("Duplicate cancellation stays deferred until the tick"), TerminatedCount, 0);
	}
	// Destruction removes the single pending cleanup and flushes the pending caller once.

	TestEqual(TEXT("Pending cancellation should be completed exactly once"), TerminatedCount, 1);
	TestEqual(TEXT("Destruction flush should report PIE_TERMINATED"), LastErrorCode, TEXT("PIE_TERMINATED"));

	FTSTicker::GetCoreTicker().Tick(0.0f);
	TestEqual(TEXT("No leaked cleanup may run against the destroyed tracker"), TerminatedCount, 1);

	return true;
}

// A cancellation armed for a request that has already ended must not swallow a later, genuine
// cancellation of the request that replaced it: the newer generation re-arms the single cleanup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateLaterCancelStillAppliesTest,
	"Cortex.Editor.PIEState.LaterCancelStillAppliesAfterNewRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateLaterCancelStillAppliesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	int32 TerminatedCount = 0;
	FString LastErrorCode;

	FCortexEditorPIEState PIEState;
	PIEState.BindDelegates();

	PIEState.RegisterPendingCallback([&TerminatedCount, &LastErrorCode](FCortexCommandResult Result)
	{
		++TerminatedCount;
		LastErrorCode = Result.ErrorCode;
	});
	PIEState.SetState(ECortexPIEState::Starting);

	// Cancellation of the first request stays pending...
	FEditorDelegates::CancelPIE.Broadcast();

	// ...and a replacement request begins before that cleanup runs (end of session, then start_pie).
	PIEState.SetState(ECortexPIEState::Stopped);
	PIEState.SetState(ECortexPIEState::Starting);

	// The replacement request is itself rejected while dequeuing; that cancellation must apply.
	FEditorDelegates::CancelPIE.Broadcast();

	FTSTicker::GetCoreTicker().Tick(0.0f);

	TestEqual(TEXT("Later cancellation should complete the pending caller once"), TerminatedCount, 1);
	TestEqual(TEXT("Error code should be PIE_TERMINATED"), LastErrorCode, TEXT("PIE_TERMINATED"));
	TestEqual(TEXT("Tracker should be Stopped"), PIEState.GetState(), ECortexPIEState::Stopped);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEStateScopedEndPreservesForeignInputTest,
	"Cortex.Editor.PIEState.ScopedEndRetiresOnlyOwnedInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEStateScopedEndPreservesForeignInputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;
	PIEState.SetState(ECortexPIEState::Playing);

	// Session-wide state a surviving foreign instance still depends on.
	const TSharedRef<FThreadSafeBool> TokenBefore = PIEState.GetInputCancelToken();
	bool bSessionInputCallbackFired = false;
	FString SessionInputErrorCode;
	PIEState.RegisterPendingInputCallback([&bSessionInputCallbackFired, &SessionInputErrorCode](FCortexCommandResult Result)
	{
		bSessionInputCallbackFired = true;
		SessionInputErrorCode = Result.ErrorCode;
	});

	// An owned continuous run whose own deferred caller is provably this session's.
	bool bOwnedRunCallbackFired = false;
	FString OwnedRunErrorCode;
	const uint32 OwnedRunCallbackId = PIEState.RegisterPendingInputCallback([&bOwnedRunCallbackFired, &OwnedRunErrorCode](FCortexCommandResult Result)
	{
		bOwnedRunCallbackFired = true;
		OwnedRunErrorCode = Result.ErrorCode;
	});
	PIEState.TrackContinuousInputRun(nullptr, nullptr, /*bTimed=*/true, 0.5f, /*bHasCallback=*/true, OwnedRunCallbackId);

	FCortexScopedPIESnapshot ScopedEnding;
	ScopedEnding.Phase = ECortexScopedPIEPhase::Ending;
	ScopedEnding.RequestSerial = 7;
	ScopedEnding.bScopedEndQueued = true;
	PIEState.HandleScopedPIELifecycle(ScopedEnding);

	TestTrue(TEXT("A scoped owned end completes the owned run's own pending caller"), bOwnedRunCallbackFired);
	TestEqual(TEXT("The owned run caller reports OperationCancelled"), OwnedRunErrorCode, FString(TEXT("OperationCancelled")));

	TestFalse(TEXT("A scoped owned end must NOT flip the session-wide cancel token"), *TokenBefore);
	TestTrue(TEXT("The session-wide cancel token identity must be unchanged"),
		PIEState.GetInputCancelToken().Get() == TokenBefore.Get());
	TestFalse(TEXT("A scoped owned end must NOT complete session-wide pending input callbacks (they may belong to a foreign owner)"),
		bSessionInputCallbackFired);
	TestEqual(TEXT("The global editor state stays Playing while a foreign instance survives"),
		PIEState.GetState(), ECortexPIEState::Playing);

	// A genuine global end still retires the surviving owner exactly once.
	PIEState.OnPIEEnded();

	TestTrue(TEXT("A global end completes the session-wide input callback"), bSessionInputCallbackFired);
	TestEqual(TEXT("The globally cancelled callback reports OperationCancelled"),
		SessionInputErrorCode, FString(TEXT("OperationCancelled")));
	TestEqual(TEXT("A global end returns the state machine to Stopped"),
		PIEState.GetState(), ECortexPIEState::Stopped);

	return true;
}
