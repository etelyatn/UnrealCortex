// PIE request-generation integration acceptance for the Cortex editor domain.
//
// The isolated tracker cases in CortexEditorPIEStateTest.cpp drive FCortexEditorPIEState with
// synthetic state assignments and manual delegate broadcasts. These cases close the remaining gap
// by driving the *real* engine lifecycle:
//
//  1. Genuine rejection (public command route): `start_pie` queues a real UEditorEngine play
//     request; the engine cancel entrypoint clears it in the same latent step, before any editor
//     tick can dequeue it. The engine never creates a world, and the pending caller completes
//     exactly once with PIE_TERMINATED while the tracker returns to Stopped.
//  2. Full round trip (public command route): start -> stop -> new-window start, each through the
//     handler, with every deferred caller completed by the engine's own PIE lifecycle delegates.
//     UEditorEngine::EndPlayMap() broadcasts CancelPIE unconditionally after EndPIE, so the
//     new-window start is admitted before that deferred cleanup runs and must survive it.
//  3. Stale cancellation with successor ownership: a real session is torn down synchronously with
//     GEditor->EndPlayMap() and a successor is admitted in the same latent step. The successor's
//     *operation-owned* cancellation token is captured between that admission and the stale
//     cleanup ticker, then re-checked (same object, not cancelled) after the successor is Playing
//     and after it owns a live continuous Enhanced Input run with a real consumed value.
//
// Case 3 drives the actual private operation entrypoints (FCortexEditorPIEOps::StartPIE and
// FCortexEditorInputOps::InjectInputContinuous) against a test-owned FCortexEditorPIEState. That
// tracker is the one that owns every operation performed in the case, so the observed token is the
// operation-owned identity rather than a parallel observer's model; the handler's own tracker is
// deliberately absent so only one tracker is bound to the engine delegates. No production edit, no
// new accessor, no SetState() modelling: every state read below is an engine transition or a
// callback the engine actually invoked.
//
// Everything in the anonymous namespace is module-unique: native unity builds merge anonymous
// namespaces across this module's test translation units.

#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexEditorPIEState.h"
#include "CortexTypes.h"
#include "HAL/ThreadSafeBool.h"
#include "Operations/CortexEditorInputOps.h"
#include "Operations/CortexEditorPIEOps.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "Tests/AutomationCommon.h"

namespace
{
const TCHAR* const CortexPIEGenerationCommandStartPie = TEXT("start_pie");
const TCHAR* const CortexPIEGenerationCommandStopPie = TEXT("stop_pie");
const TCHAR* const CortexPIEGenerationCommandGetPIEState = TEXT("get_pie_state");

const TCHAR* const CortexPIEGenerationModeSelectedViewport = TEXT("selected_viewport");
const TCHAR* const CortexPIEGenerationModeNewWindow = TEXT("new_window");
const TCHAR* const CortexPIEGenerationModeStop = TEXT("stop");

const TCHAR* const CortexPIEGenerationStateStopped = TEXT("stopped");
const TCHAR* const CortexPIEGenerationStateStarting = TEXT("starting");
const TCHAR* const CortexPIEGenerationStatePlaying = TEXT("playing");

// Upper bound for a real PIE start/stop round trip in the automation environment. Each wait arms
// this deadline on its own first update, so it measures the wait itself, never the time a command
// spent queued behind earlier commands.
constexpr double CortexPIEGenerationLifecycleTimeoutSeconds = 20.0;

TSharedPtr<FJsonObject> MakeCortexPIEGenerationStartParams(const FString& Mode)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("mode"), Mode);
	return Params;
}

// Handler-route tracker state, read through the public command surface.
FString CortexPIEGenerationReadState(const TSharedRef<FCortexEditorCommandHandler>& Handler)
{
	const FCortexCommandResult StateResult =
		Handler->Execute(CortexPIEGenerationCommandGetPIEState, MakeShared<FJsonObject>());
	if (!StateResult.bSuccess || !StateResult.Data.IsValid())
	{
		return TEXT("<unavailable>");
	}
	FString State;
	StateResult.Data->TryGetStringField(TEXT("state"), State);
	return State;
}

// Operation-route tracker state, read from the test-owned tracker itself.
FString CortexPIEGenerationReadOperationState(const TSharedPtr<FCortexEditorPIEState>& PIEState)
{
	return PIEState.IsValid()
		? FCortexEditorPIEState::StateToString(PIEState->GetState())
		: FString(TEXT("<unavailable>"));
}

// Resolves the live PIE world's Enhanced Input subsystem and reads the real consumer value the
// action currently holds, i.e. what UEnhancedPlayerInput::GetActionValue reports during PIE.
bool CortexPIEGenerationTryReadActionValue(
	UInputAction* Action,
	UEnhancedInputLocalPlayerSubsystem*& OutSubsystem,
	UEnhancedPlayerInput*& OutPlayerInput,
	FVector& OutValue)
{
	OutSubsystem = nullptr;
	OutPlayerInput = nullptr;
	OutValue = FVector::ZeroVector;

	if (GEditor == nullptr || GEditor->PlayWorld == nullptr || Action == nullptr)
	{
		return false;
	}

	APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController();
	if (PlayerController == nullptr)
	{
		return false;
	}

	ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (LocalPlayer == nullptr)
	{
		return false;
	}

	OutSubsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (OutSubsystem == nullptr)
	{
		return false;
	}

	OutPlayerInput = OutSubsystem->GetPlayerInput();
	if (OutPlayerInput == nullptr)
	{
		return false;
	}

	OutValue = OutPlayerInput->GetActionValue(Action).Get<FVector>();
	return true;
}

// Rooted transient Axis3D action that the command/operation layer resolves by canonical path.
struct FCortexPIEGenerationInputFixture
{
	UInputAction* Action = nullptr;
	FString ActionPath;

	void Create(FAutomationTestBase& Test)
	{
		const FString Name = FString::Printf(
			TEXT("CortexPIEGenerationAction_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Action = NewObject<UInputAction>(GetTransientPackage(), FName(*Name));
		if (Action == nullptr)
		{
			Test.AddError(TEXT("Failed to create the transient PIE generation input action fixture"));
			return;
		}
		Action->ValueType = EInputActionValueType::Axis3D;
		Action->AddToRoot();
		ActionPath = Action->GetPathName();
	}

	void Reset()
	{
		if (IsValid(Action))
		{
			if (Action->IsRooted())
			{
				Action->RemoveFromRoot();
			}
			Action->MarkAsGarbage();
		}
		Action = nullptr;
	}

	~FCortexPIEGenerationInputFixture()
	{
		Reset();
	}
};

struct FCortexPIEGenerationDeferredCapture
{
	int32 Count = 0;
	FCortexCommandResult Last;
};

// Retains the operation-owned cancellation token captured for the successor. The alias keeps the
// exact observed object alive (a replaced token is a different object, so identity - not value -
// is the assertion), and the recorded address lets the post-boundary check prove no replacement.
struct FCortexPIEGenerationTokenObservation
{
	TSharedPtr<FThreadSafeBool> Token;
	FThreadSafeBool* Address = nullptr;
	bool bCaptured = false;
	bool bCancelledAtCapture = false;

	void Capture(const TSharedRef<FThreadSafeBool>& InToken)
	{
		// TSharedPtr is constructible from TSharedRef and shares the same FThreadSafeBool, which
		// keeps the observed object alive even if the tracker replaces its own reference.
		Token = TSharedPtr<FThreadSafeBool>(InToken);
		// TSharedPtr::Get() already yields the raw pointer (TSharedRef::Get() would yield a
		// reference; the identity comparisons below take the address of that reference).
		Address = Token.Get();
		bCancelledAtCapture = InToken.Get();
		bCaptured = true;
	}
};

/** Runs Step once on the current frame, then completes. */
class FCortexPIEGenerationStep : public IAutomationLatentCommand
{
public:
	explicit FCortexPIEGenerationStep(TFunction<void()> InStep)
		: Step(MoveTemp(InStep))
	{
	}

	virtual bool Update() override
	{
		Step();
		return true;
	}

private:
	TFunction<void()> Step;
};

/**
 * Deadline-bounded wait for an observable lifecycle transition. The deadline is armed on the first
 * update rather than at construction (commands are constructed when queued, so a constructor-armed
 * deadline can already be expired by the time the command runs). The predicate is only inspected,
 * never retried against a mutated target, and a timeout reports a single failure so a genuine
 * engine regression cannot hang the run.
 */
class FCortexPIEGenerationWaitForCondition : public IAutomationLatentCommand
{
public:
	FCortexPIEGenerationWaitForCondition(
		FAutomationTestBase* InTest,
		TFunction<bool()> InPredicate,
		FString InDescription,
		double InTimeoutSeconds = CortexPIEGenerationLifecycleTimeoutSeconds)
		: Test(InTest)
		, Predicate(MoveTemp(InPredicate))
		, Description(MoveTemp(InDescription))
		, TimeoutSeconds(InTimeoutSeconds)
	{
	}

	virtual bool Update() override
	{
		if (!bDeadlineArmed)
		{
			bDeadlineArmed = true;
			DeadlineStartTime = FPlatformTime::Seconds();
		}

		if (Predicate())
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - DeadlineStartTime) >= TimeoutSeconds)
		{
			Test->AddError(FString::Printf(
				TEXT("Timed out after %.1fs waiting for %s"), TimeoutSeconds, *Description));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TFunction<bool()> Predicate;
	FString Description;
	double TimeoutSeconds;
	bool bDeadlineArmed = false;
	double DeadlineStartTime = 0.0;
};

class FCortexPIEGenerationWaitForPlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexPIEGenerationWaitForPlaying(FAutomationTestBase* InTest)
		: Test(InTest)
	{
	}

	virtual bool Update() override
	{
		if (!bDeadlineArmed)
		{
			bDeadlineArmed = true;
			DeadlineStartTime = FPlatformTime::Seconds();
		}

		if (GEditor != nullptr &&
			GEditor->PlayWorld != nullptr &&
			GEditor->PlayWorld->HasBegunPlay() &&
			GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - DeadlineStartTime) >= CortexPIEGenerationLifecycleTimeoutSeconds)
		{
			Test->AddError(TEXT("Timed out waiting for a real PIE world to begin play"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	bool bDeadlineArmed = false;
	double DeadlineStartTime = 0.0;
};

/**
 * Runs an action once and observes it after a fixed number of world ticks. UE ticks the PIE world
 * before the global tickables that drive latent commands, so a continuous value queued by a latent
 * step is consumed on a subsequent world tick - the same fixed barrier the existing
 * continuous-input cases use.
 */
class FCortexPIEGenerationRunThenObserve : public IAutomationLatentCommand
{
public:
	FCortexPIEGenerationRunThenObserve(TFunction<void()> InRun, TFunction<void()> InObserve, int32 InWorldTicks)
		: Run(MoveTemp(InRun))
		, Observe(MoveTemp(InObserve))
		, FramesRemaining(InWorldTicks)
	{
	}

	virtual bool Update() override
	{
		if (!bRan)
		{
			bRan = true;
			Run();
			return false;
		}
		if (--FramesRemaining > 0)
		{
			return false;
		}
		Observe();
		return true;
	}

private:
	bool bRan = false;
	TFunction<void()> Run;
	TFunction<void()> Observe;
	int32 FramesRemaining;
};

void AddCortexPIEGenerationStep(TFunction<void()> Step)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexPIEGenerationStep>(MoveTemp(Step)));
}

void AddCortexPIEGenerationRunThenObserve(TFunction<void()> Run, TFunction<void()> Observe, int32 WorldTicks = 2)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexPIEGenerationRunThenObserve>(MoveTemp(Run), MoveTemp(Observe), WorldTicks));
}

void AddCortexPIEGenerationWaitForCapture(
	FAutomationTestBase* Test,
	const TSharedPtr<FCortexPIEGenerationDeferredCapture>& Capture,
	const FString& Description)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexPIEGenerationWaitForCondition>(
			Test,
			[Capture]() { return Capture->Count > 0; },
			Description));
}

/**
 * Settles the engine into a genuinely stopped state, at case start and at case cleanup.
 *
 * A queued play request is cancelled first because RequestEndPlayMap() is a no-op without a live
 * world: a request admitted by a step that failed before its world started would otherwise run PIE
 * after the test finished. The following wait arms its deadline on first update and requires both
 * the world to be gone and the tracker to be Stopped, so a cancelled-but-pending request's deferred
 * cleanup is also waited out. The tracker source and any fixture stay captured until that wait
 * completes.
 */
void AddCortexPIEGenerationTeardown(
	FAutomationTestBase* Test,
	const FString& ContextLabel,
	TFunction<FString()> ReadState,
	TFunction<void()> ExtraCleanup = TFunction<void()>())
{
	AddCortexPIEGenerationStep([]()
	{
		if (GEditor == nullptr)
		{
			return;
		}
		if (GEditor->IsPlaySessionRequestQueued())
		{
			GEditor->CancelRequestPlaySession();
		}
		if (GEditor->PlayWorld != nullptr)
		{
			GEditor->RequestEndPlayMap();
		}
	});

	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexPIEGenerationWaitForCondition>(
			Test,
			[ReadState]()
			{
				return (GEditor == nullptr || GEditor->PlayWorld == nullptr) &&
					ReadState() == FString(CortexPIEGenerationStateStopped);
			},
			TEXT("genuine PIE teardown with the tracker back in Stopped")));

	AddCortexPIEGenerationStep(
		[Test, ContextLabel, ReadState, ExtraCleanup = MoveTemp(ExtraCleanup)]() mutable
		{
			const FString What = FString::Printf(
				TEXT("%s: the real teardown leaves the tracker Stopped"), *ContextLabel);
			Test->TestEqual(*What, ReadState(), FString(CortexPIEGenerationStateStopped));
			if (ExtraCleanup)
			{
				ExtraCleanup();
			}
		});
}
}

// =============================================================================================
// 1. Genuine cancellation of a still-queued engine play request (public command route)
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEGenerationGenuineCancelTest,
	"Cortex.Editor.PIE.Generation.GenuineCancelTerminatesQueuedStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEGenerationGenuineCancelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexPIEGenerationDeferredCapture> StartCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();

	// Leave no session or queued request behind from a sibling test.
	AddCortexPIEGenerationTeardown(this, TEXT("precondition"),
		[Handler]() { return CortexPIEGenerationReadState(Handler); });

	AddCortexPIEGenerationStep([this, Handler, StartCapture, StartResult]()
	{
		if (GEditor == nullptr)
		{
			AddError(TEXT("GEditor is unavailable for the PIE cancellation case"));
			return;
		}
		if (GEditor->PlayWorld != nullptr)
		{
			AddError(TEXT("PIE must be stopped before the PIE cancellation case begins"));
			return;
		}

		const TSharedPtr<FJsonObject> Params =
			MakeCortexPIEGenerationStartParams(CortexPIEGenerationModeSelectedViewport);
		*StartResult = Handler->Execute(
			CortexPIEGenerationCommandStartPie,
			Params,
			[StartCapture](FCortexCommandResult Result)
			{
				++StartCapture->Count;
				StartCapture->Last = MoveTemp(Result);
			});

		TestTrue(TEXT("start_pie admits the request as a deferred response"), StartResult->bIsDeferred);
		TestEqual(TEXT("the tracker enters Starting for the admitted request"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStateStarting));
		TestTrue(TEXT("the engine holds a real queued play request"),
			GEditor->IsPlaySessionRequestQueued());

		// Genuine rejection of the queued request: the engine cancel entrypoint runs in the same
		// latent step, before any editor tick can dequeue the request into a running session.
		GEditor->CancelRequestPlaySession();

		TestFalse(TEXT("the engine cancel clears the queued play request"),
			GEditor->IsPlaySessionRequestQueued());
	});

	AddCortexPIEGenerationWaitForCapture(this, StartCapture,
		TEXT("the cancelled start request's deferred callback"));

	AddCortexPIEGenerationStep([this, Handler, StartCapture]()
	{
		TestEqual(TEXT("the cancelled start completes exactly once"), StartCapture->Count, 1);
		TestFalse(TEXT("the cancelled start reports failure"), StartCapture->Last.bSuccess);
		TestEqual(TEXT("the cancelled start is reported as PIE_TERMINATED"),
			StartCapture->Last.ErrorCode, FString(CortexErrorCodes::PIETerminated));
		TestEqual(TEXT("the tracker returns to Stopped after the rejection"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStateStopped));
		TestTrue(TEXT("no PIE world was created for the cancelled request"),
			GEditor == nullptr || GEditor->PlayWorld == nullptr);
		TestFalse(TEXT("no active PIE session was started"),
			GEditor != nullptr && GEditor->IsPlayingSessionInEditor());
	});

	// Exactly-once guard: the deferred cleanup ticker must already have run harmlessly.
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.4f));
	AddCortexPIEGenerationStep([this, StartCapture]()
	{
		TestEqual(TEXT("the cancelled start callback never re-fires"), StartCapture->Count, 1);
	});

	AddCortexPIEGenerationTeardown(this, TEXT("cleanup"),
		[Handler]() { return CortexPIEGenerationReadState(Handler); });

	return true;
}

// =============================================================================================
// 2. Real start -> stop -> new-window start through the public command route
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEGenerationStartStopNewWindowTest,
	"Cortex.Editor.PIE.Generation.StartStopNewWindowRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEGenerationStartStopNewWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexPIEGenerationDeferredCapture> StartCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexPIEGenerationDeferredCapture> StopCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexPIEGenerationDeferredCapture> RestartCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> RestartResult = MakeShared<FCortexCommandResult>();

	AddCortexPIEGenerationTeardown(this, TEXT("precondition"),
		[Handler]() { return CortexPIEGenerationReadState(Handler); });

	// --- Phase 1: real start through the handler ---------------------------------------------
	AddCortexPIEGenerationStep([this, Handler, StartCapture, StartResult]()
	{
		if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
		{
			AddError(TEXT("the start/stop round trip must begin with no running PIE world"));
			return;
		}
		*StartResult = Handler->Execute(
			CortexPIEGenerationCommandStartPie,
			MakeCortexPIEGenerationStartParams(CortexPIEGenerationModeSelectedViewport),
			[StartCapture](FCortexCommandResult Result)
			{
				++StartCapture->Count;
				StartCapture->Last = MoveTemp(Result);
			});
		TestTrue(TEXT("the real start request is deferred"), StartResult->bIsDeferred);
	});
	AddCortexPIEGenerationWaitForCapture(this, StartCapture, TEXT("the start request's success callback"));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEGenerationWaitForPlaying(this));
	AddCortexPIEGenerationStep([this, Handler, StartCapture]()
	{
		TestEqual(TEXT("the engine produced exactly one start callback"), StartCapture->Count, 1);
		TestTrue(TEXT("the start callback reports success"), StartCapture->Last.bSuccess);
		TestEqual(TEXT("the tracker is Playing after the engine's PostPIEStarted"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStatePlaying));
		TestTrue(TEXT("a live PIE world exists"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
		TestTrue(TEXT("the engine reports an active PIE session"),
			GEditor != nullptr && GEditor->IsPlayingSessionInEditor());
	});

	// --- Phase 2: real stop through the handler ----------------------------------------------
	AddCortexPIEGenerationStep([this, Handler, StopCapture, StopResult]()
	{
		*StopResult = Handler->Execute(
			CortexPIEGenerationCommandStopPie,
			MakeShared<FJsonObject>(),
			[StopCapture](FCortexCommandResult Result)
			{
				++StopCapture->Count;
				StopCapture->Last = MoveTemp(Result);
			});
		TestTrue(TEXT("the real stop request is deferred"), StopResult->bIsDeferred);
	});
	AddCortexPIEGenerationWaitForCapture(this, StopCapture, TEXT("the stop request's success callback"));
	AddCortexPIEGenerationStep([this, Handler, StopCapture]()
	{
		TestEqual(TEXT("the engine produced exactly one stop callback"), StopCapture->Count, 1);
		TestTrue(TEXT("the stop callback reports success"), StopCapture->Last.bSuccess);
		TestEqual(TEXT("the tracker is Stopped after the engine's EndPIE"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStateStopped));
		TestTrue(TEXT("the PIE world is gone"), GEditor == nullptr || GEditor->PlayWorld == nullptr);
		TestFalse(TEXT("the engine reports no active PIE session"),
			GEditor != nullptr && GEditor->IsPlayingSessionInEditor());
	});

	// --- Phase 3: real new-window start -------------------------------------------------------
	// The stop above left a CancelPIE cleanup queued by EndPlayMap (broadcast after EndPIE). This
	// request is admitted in the same latent pass that observed the teardown, so it must survive
	// that cleanup and complete on its own PostPIEStarted rather than on PIE_TERMINATED.
	AddCortexPIEGenerationStep([this, Handler, RestartCapture, RestartResult]()
	{
		*RestartResult = Handler->Execute(
			CortexPIEGenerationCommandStartPie,
			MakeCortexPIEGenerationStartParams(CortexPIEGenerationModeNewWindow),
			[RestartCapture](FCortexCommandResult Result)
			{
				++RestartCapture->Count;
				RestartCapture->Last = MoveTemp(Result);
			});
		TestTrue(TEXT("the new-window start is deferred"), RestartResult->bIsDeferred);
		TestEqual(TEXT("the new-window request begins a fresh generation"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStateStarting));
	});
	AddCortexPIEGenerationWaitForCapture(this, RestartCapture,
		TEXT("the new-window start request's success callback"));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEGenerationWaitForPlaying(this));
	AddCortexPIEGenerationStep([this, Handler, RestartCapture]()
	{
		TestEqual(TEXT("the new-window start completes exactly once"), RestartCapture->Count, 1);
		TestTrue(TEXT("the stale cancellation did not terminate the new-window start"),
			RestartCapture->Last.bSuccess);
		TestEqual(TEXT("the new-window session reaches Playing"),
			CortexPIEGenerationReadState(Handler), FString(CortexPIEGenerationStatePlaying));
		TestTrue(TEXT("an active PIE world exists after the new-window start"),
			GEditor != nullptr && GEditor->PlayWorld != nullptr &&
			GEditor->PlayWorld->HasBegunPlay());
		TestTrue(TEXT("the engine reports the successor session as active"),
			GEditor != nullptr && GEditor->IsPlayingSessionInEditor());
	});

	AddCortexPIEGenerationTeardown(this, TEXT("cleanup"),
		[Handler]() { return CortexPIEGenerationReadState(Handler); });

	return true;
}

// =============================================================================================
// 3. Stale cancellation must preserve the successor's operation-owned session and input
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEGenerationStaleCancelTest,
	"Cortex.Editor.PIE.Generation.StaleCancelKeepsSuccessorSessionAndInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEGenerationStaleCancelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// The tracker under test owns every operation driven below (start_pie, inject_input_continuous
	// through the real operation entrypoints), so the observed cancellation token is the
	// operation-owned identity. No command handler exists in this case, so exactly one tracker is
	// bound to the engine delegates.
	const TSharedPtr<FCortexEditorPIEState> PIEState = MakeShared<FCortexEditorPIEState>();
	PIEState->BindDelegates();

	const TSharedPtr<FCortexPIEGenerationInputFixture> Fixture = MakeShared<FCortexPIEGenerationInputFixture>();
	Fixture->Create(*this);

	const TSharedPtr<FCortexPIEGenerationDeferredCapture> PredecessorCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexPIEGenerationDeferredCapture> SuccessorCapture =
		MakeShared<FCortexPIEGenerationDeferredCapture>();
	const TSharedPtr<FCortexCommandResult> PredecessorResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> SuccessorResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> InjectResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> InjectStopResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<TWeakObjectPtr<UWorld>> PredecessorWorld = MakeShared<TWeakObjectPtr<UWorld>>();
	const TSharedPtr<FCortexPIEGenerationTokenObservation> TokenObservation =
		MakeShared<FCortexPIEGenerationTokenObservation>();

	const TFunction<FString()> ReadOperationState =
		[PIEState]() { return CortexPIEGenerationReadOperationState(PIEState); };

	AddCortexPIEGenerationTeardown(this, TEXT("precondition"), ReadOperationState);

	// --- Predecessor: a real PIE session admitted through the real operation ------------------
	AddCortexPIEGenerationStep([this, PIEState, PredecessorCapture, PredecessorResult]()
	{
		if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
		{
			AddError(TEXT("the stale-cancellation case must begin with no running PIE world"));
			return;
		}
		*PredecessorResult = FCortexEditorPIEOps::StartPIE(
			*PIEState,
			MakeCortexPIEGenerationStartParams(CortexPIEGenerationModeSelectedViewport),
			[PredecessorCapture](FCortexCommandResult Result)
			{
				++PredecessorCapture->Count;
				PredecessorCapture->Last = MoveTemp(Result);
			});
		TestTrue(TEXT("the predecessor start is deferred by the real operation"),
			PredecessorResult->bIsDeferred);
		TestEqual(TEXT("the operation tracker enters Starting after admission"),
			PIEState->GetState(), ECortexPIEState::Starting);
	});
	AddCortexPIEGenerationWaitForCapture(this, PredecessorCapture,
		TEXT("the predecessor start request's success callback"));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEGenerationWaitForPlaying(this));
	AddCortexPIEGenerationStep([this, PIEState, PredecessorCapture, PredecessorWorld]()
	{
		TestEqual(TEXT("the predecessor start completes exactly once"), PredecessorCapture->Count, 1);
		TestTrue(TEXT("the predecessor start succeeds"), PredecessorCapture->Last.bSuccess);
		TestEqual(TEXT("the operation tracker reaches Playing"),
			PIEState->GetState(), ECortexPIEState::Playing);
		TestTrue(TEXT("a live predecessor world exists"),
			GEditor != nullptr && GEditor->PlayWorld != nullptr);
		*PredecessorWorld = (GEditor != nullptr) ? GEditor->PlayWorld : nullptr;
	});

	// --- Stale cancellation and the successor, in one latent step -----------------------------
	// GEditor->EndPlayMap() synchronously tears the predecessor down and, at the end of its
	// teardown, broadcasts CancelPIE unconditionally (UEditorEngine::CancelRequestPlaySession).
	// That arms the tracker's deferred cleanup for the predecessor's generation. The successor is
	// admitted synchronously right after, so it runs under a newer generation; its operation-owned
	// cancellation token is captured here, after admission and before the cleanup ticker can run.
	AddCortexPIEGenerationStep(
		[this, PIEState, PredecessorWorld, SuccessorCapture, SuccessorResult, TokenObservation]()
	{
		if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
		{
			AddError(TEXT("the stale-cancellation step requires a live predecessor PIE world"));
			return;
		}

		GEditor->EndPlayMap();

		TestTrue(TEXT("the synchronous end destroys the predecessor world"),
			GEditor->PlayWorld == nullptr);
		TestFalse(TEXT("the engine reports no active session after the synchronous end"),
			GEditor->IsPlayingSessionInEditor());

		*SuccessorResult = FCortexEditorPIEOps::StartPIE(
			*PIEState,
			MakeCortexPIEGenerationStartParams(CortexPIEGenerationModeNewWindow),
			[SuccessorCapture](FCortexCommandResult Result)
			{
				++SuccessorCapture->Count;
				SuccessorCapture->Last = MoveTemp(Result);
			});

		TestTrue(TEXT("the successor start is deferred by the real operation"),
			SuccessorResult->bIsDeferred);
		TestEqual(TEXT("the successor begins a fresh generation (Starting)"),
			PIEState->GetState(), ECortexPIEState::Starting);
		TestTrue(TEXT("a fresh engine play request is queued for the successor"),
			GEditor->IsPlaySessionRequestQueued());

		TokenObservation->Capture(PIEState->GetInputCancelToken());
		TestTrue(TEXT("the successor's operation-owned token was captured"),
			TokenObservation->bCaptured);
		TestFalse(TEXT("the successor's token is not cancelled at admission"),
			TokenObservation->bCancelledAtCapture);
	});

	AddCortexPIEGenerationWaitForCapture(this, SuccessorCapture,
		TEXT("the successor start request's success callback"));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEGenerationWaitForPlaying(this));
	AddCortexPIEGenerationStep([this, PIEState, PredecessorWorld, SuccessorCapture, TokenObservation]()
	{
		TestEqual(TEXT("the successor start completes exactly once"), SuccessorCapture->Count, 1);
		TestTrue(TEXT("the stale cancellation did not terminate the successor start"),
			SuccessorCapture->Last.bSuccess);
		TestEqual(TEXT("the operation tracker reaches Playing"),
			PIEState->GetState(), ECortexPIEState::Playing);
		TestTrue(TEXT("the successor has a live, begun PIE world"),
			GEditor != nullptr && GEditor->PlayWorld != nullptr &&
			GEditor->PlayWorld->HasBegunPlay());
		TestTrue(TEXT("the predecessor world was genuinely destroyed"), PredecessorWorld->IsStale());
		TestTrue(TEXT("the successor is a genuinely new world, not the stale one"),
			GEditor != nullptr && GEditor->PlayWorld != PredecessorWorld->Get());

		// A failed earlier step can leave the token uncaptured: record that and return before
		// dereferencing it. The remaining cleanup commands stay queued and keep the tracker (and
		// the fixture) captured, so teardown still runs.
		TestTrue(TEXT("the successor's operation-owned token was captured for this observation"),
			TokenObservation->Token.IsValid());
		if (!TokenObservation->Token.IsValid())
		{
			return;
		}

		// Identity, not value: a replaced token would read false too, so compare the objects.
		TestTrue(TEXT("the stale cleanup did not replace the successor cancellation token"),
			&PIEState->GetInputCancelToken().Get() == TokenObservation->Address);
		TestFalse(TEXT("the stale cleanup did not cancel the successor cancellation token"),
			*TokenObservation->Token);
	});

	// Let the stale cleanup ticker certainly run before the successor exercises its input
	// ownership: a cleanup that had wrongly applied would have forced the tracker to Stopped.
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.4f));
	AddCortexPIEGenerationStep([this, PIEState, TokenObservation]()
	{
		TestEqual(TEXT("the successor is still Playing after the stale cleanup ran"),
			PIEState->GetState(), ECortexPIEState::Playing);
		TestTrue(TEXT("the successor's operation-owned token is still available"),
			TokenObservation->Token.IsValid());
		if (!TokenObservation->Token.IsValid())
		{
			return;
		}
		TestFalse(TEXT("the stale cleanup did not cancel the successor token"),
			*TokenObservation->Token);
	});

	// --- Successor continuous input through the same operation-owned tracker -------------------
	TFunction<void()> RunInject = [this, PIEState, Fixture, InjectResult]()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("action_name"), Fixture->ActionPath);
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("x"), 0.0);
		Value->SetNumberField(TEXT("y"), 1.0);
		Value->SetNumberField(TEXT("z"), 0.0);
		Params->SetObjectField(TEXT("value"), Value);

		*InjectResult = FCortexEditorInputOps::InjectInputContinuous(PIEState, Params, nullptr);
		TestTrue(TEXT("the successor accepts a continuous Axis3D run from its operation"),
			InjectResult->bSuccess);
		if (InjectResult->bSuccess && InjectResult->Data.IsValid())
		{
			TestTrue(TEXT("the successor injection reports it is injecting"),
				InjectResult->Data->GetBoolField(TEXT("injecting")));
		}
	};
	TFunction<void()> ObserveInject = [this, PIEState, Fixture, InjectResult, TokenObservation]()
	{
		if (!InjectResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!CortexPIEGenerationTryReadActionValue(Fixture->Action, Subsystem, PlayerInput, Value))
		{
			AddError(TEXT("the PIE Enhanced Input consumer is unavailable for the successor injection"));
			return;
		}
		TestTrue(TEXT("the successor owns the registered continuous injection"),
			Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("the successor consumes its own value (0,1,0) after two world ticks"),
			Value.Equals(FVector(0.0, 1.0, 0.0), 1e-4));
		TestEqual(TEXT("the successor injection keeps the action's Axis3D value type"),
			static_cast<int32>(PlayerInput->GetActionValue(Fixture->Action).GetValueType()),
			static_cast<int32>(EInputActionValueType::Axis3D));
		TestTrue(TEXT("the live run's operation token is still available"),
			TokenObservation->Token.IsValid());
		if (!TokenObservation->Token.IsValid())
		{
			return;
		}
		TestTrue(TEXT("the live run still runs under the same operation token object"),
			&PIEState->GetInputCancelToken().Get() == TokenObservation->Address);
		TestFalse(TEXT("the live run's operation token was never cancelled"),
			*TokenObservation->Token);
	};
	AddCortexPIEGenerationRunThenObserve(RunInject, ObserveInject, 2);

	// Stop the owned run explicitly; teardown would also clear it, but this asserts the successor
	// session can retire the run it owns after the stale cancellation.
	AddCortexPIEGenerationStep([this, PIEState, Fixture, InjectStopResult]()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("action_name"), Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), CortexPIEGenerationModeStop);
		*InjectStopResult = FCortexEditorInputOps::InjectInputContinuous(PIEState, Params, nullptr);
		TestTrue(TEXT("the successor can stop the injection it owns"), InjectStopResult->bSuccess);
	});

	AddCortexPIEGenerationTeardown(this, TEXT("cleanup"), ReadOperationState,
		[Fixture]() { Fixture->Reset(); });

	return true;
}
