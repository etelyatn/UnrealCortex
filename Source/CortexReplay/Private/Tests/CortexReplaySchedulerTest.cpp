#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"

namespace
{
FCortexReplayEvent MakeKeyEvent(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event;
	Event.Sequence = Sequence;
	Event.TimeSeconds = TimeSeconds;
	Event.Input.Kind = Kind;
	Event.Input.Key = Key;
	return Event;
}

FCortexReplayEvent MakePointerPressEvent(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event = MakeKeyEvent(Sequence, TimeSeconds, Kind, Key);
	return Event;
}

/** A supported tagged-Slate guard whose observation the caller can flip between disabled/ready. */
struct FSchedulerGuardProbe
{
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Target;
	FCortexEditorPhysicalInputUIObservation UI;
	double ExpectedLocalX = 0.25;
	double ExpectedLocalY = 0.5;

	void MakeReady()
	{
		UI.State = ECortexEditorUIObservationState::Ready;
	}

	void Setup(FCortexReplayEvent& Event)
	{
		if (!Target.IsValid())
		{
			TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
				MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
			Identity->RootKind = ECortexEditorUIRootKind::Slate;
			Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
			Identity->RootTag = TEXT("SchedulerRoot");
			Identity->TargetTag = TEXT("SchedulerControl");
			// Supported selectors must carry a trustworthy digest.
			Identity->IdentitySha256 = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(*Identity);
			Target = Identity;
		}

		FCortexReplayInteractionGuard Guard;
		Guard.UICoverage = ECortexEditorUICoverage::Supported;
		Guard.UITarget = Target;
		Guard.ExpectedLocalPosition = FVector2D(ExpectedLocalX, ExpectedLocalY);
		Event.Guard = Guard;

		UI.State = ECortexEditorUIObservationState::TargetDisabled;
		UI.ActualTarget = Target;
		UI.LocalPosition = FVector2D(ExpectedLocalX, ExpectedLocalY);
	}
};

/** Records the classification identity of every dispatched event, not only a count. */
struct FSchedulerDispatchLog
{
	TArray<int32> Sequences;
	TArray<ECortexEditorPhysicalInputKind> Kinds;

	FCortexCommandResult Record(const FCortexReplayEvent& Event)
	{
		Sequences.Add(Event.Sequence);
		Kinds.Add(Event.Input.Kind);
		return FCortexCommandRouter::Success(nullptr);
	}
};

TFunction<FCortexReplayGuardDecision(const FCortexReplayEvent&)> MakeUnguardedEvaluator(
	const TSharedPtr<FCortexReplaySnapshot>& Snapshot)
{
	return [Snapshot](const FCortexReplayEvent& Event)
	{
		const FCortexEditorPhysicalInputUIObservation UI;
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, false);
	};
}
}

// ---------------------------------------------------------------------------
// Two events share time 0; after a 0.301s gap the second is overdue by 101ms and errors.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerSameTimeLatenessTest,
	"Cortex.Replay.Scheduler.SameTimeLatenessAcrossAdvances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerSameTimeLatenessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Down;
	Down.Sequence = 0;
	Down.TimeSeconds = 0.0;
	Down.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Down.Input.Key = EKeys::W;
	FCortexReplayEvent Up = Down;
	Up.Sequence = 1;
	Up.TimeSeconds = 0.2;
	Up.Input.Kind = ECortexEditorPhysicalInputKind::KeyUp;
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Down, Up});
	Recording.Metadata.DurationSeconds = 0.4;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	TArray<int32> Dispatched;
	auto Dispatch = [&Dispatched](const FCortexReplayEvent& Event)
	{
		Dispatched.Add(Event.Sequence);
		return FCortexCommandRouter::Success(nullptr);
	};
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Evaluate = [&Snapshot](const FCortexReplayEvent& Event)
	{
		const FCortexEditorPhysicalInputUIObservation UI;
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, false);
	};
	TestTrue(TEXT("Initial due event accepted"), AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	Elapsed = 0.301;
	const FCortexCommandResult Late = AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch);
	TestFalse(TEXT("Event scheduled at 0.2 is too late"), Late.bSuccess);
	TestEqual(TEXT("Late event reports REPLAY_TIMING_ERROR"), Late.ErrorCode,
		FString(TEXT("REPLAY_TIMING_ERROR")));
	TestEqual(TEXT("Late event not dispatched"), Dispatched.Num(), 1);
	if (Dispatched.Num() == 1)
	{
		TestEqual(TEXT("Only the time-zero sequence ran"), Dispatched[0], 0);
	}
	TestFalse(TEXT("A timing failure never completes the run"), Scheduler.IsComplete());

	return true;
}

// ---------------------------------------------------------------------------
// The consumer advances the clock to 0.101 during the first dispatch; the second same-time
// event must be rejected with its fresh lateness even though both were due when Advance began.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerDelayInsideOneAdvanceTest,
	"Cortex.Replay.Scheduler.DelayInsideOneAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerDelayInsideOneAdvanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Down;
	Down.Sequence = 0;
	Down.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Down.Input.Key = EKeys::W;
	FCortexReplayEvent Up = Down;
	Up.Sequence = 1;
	Up.Input.Kind = ECortexEditorPhysicalInputKind::KeyUp;
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Down, Up});
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	double Elapsed = 0.0;
	TArray<int32> Dispatched;
	const FCortexCommandResult Result = AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; },
		[&Snapshot](const FCortexReplayEvent& Event)
		{
			const FCortexEditorPhysicalInputUIObservation UI;
			return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, false);
		},
		[&Elapsed, &Dispatched](const FCortexReplayEvent& Event)
		{
			Dispatched.Add(Event.Sequence);
			Elapsed = 0.101;
			return FCortexCommandRouter::Success(nullptr);
		});
	TestFalse(TEXT("Later same-time event rejected after slow consumer"), Result.bSuccess);
	TestEqual(TEXT("Timing error retained"), Result.ErrorCode, FString(TEXT("REPLAY_TIMING_ERROR")));
	TestEqual(TEXT("Only first sequence dispatched"), Dispatched.Num(), 1);
	if (Dispatched.Num() == 1) { TestEqual(TEXT("First sequence identity"), Dispatched[0], 0); }
	TestTrue(TEXT("Timing details exist"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("Exact overdue sequence"),
			Result.ErrorDetails->GetIntegerField(TEXT("sequence")), 1);
		TestEqual(TEXT("Fresh lateness in milliseconds"),
			Result.ErrorDetails->GetNumberField(TEXT("lateness_ms")), 101.0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// The fixed 0.100s lateness constant is inclusive: exactly 0.100 dispatches, greater fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerExactLatenessBoundaryTest,
	"Cortex.Replay.Scheduler.ExactLatenessBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerExactLatenessBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	auto Evaluate = [&Snapshot](const FCortexReplayEvent& Event)
	{
		const FCortexEditorPhysicalInputUIObservation UI;
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, false);
	};

	// Exactly the representable constant 0.100 is still on time.
	{
		FCortexReplayScheduler Scheduler(Snapshot);
		int32 Dispatches = 0;
		double Elapsed = 0.100;
		const FCortexCommandResult Result = AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; },
			Evaluate,
			[&Dispatches](const FCortexReplayEvent&)
			{
				++Dispatches;
				return FCortexCommandRouter::Success(nullptr);
			});
		TestTrue(TEXT("Exactly 0.100 is within the fixed lateness constant"), Result.bSuccess);
		TestEqual(TEXT("Boundary event dispatched"), Dispatches, 1);
	}

	// Any later observation is overdue and must fail without dispatching.
	{
		FCortexReplayScheduler Scheduler(Snapshot);
		int32 Dispatches = 0;
		double Elapsed = 0.101;
		const FCortexCommandResult Result = AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; },
			Evaluate,
			[&Dispatches](const FCortexReplayEvent&)
			{
				++Dispatches;
				return FCortexCommandRouter::Success(nullptr);
			});
		TestFalse(TEXT("More than 0.100 late fails"), Result.bSuccess);
		TestEqual(TEXT("Overdue boundary event not dispatched"), Dispatches, 0);
		TestFalse(TEXT("No sequence consumed"), Scheduler.GetDispatchedCount() != 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Events sharing one time dispatch in stored sequence order within a single Advance.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerSameTimeSequenceOrderingTest,
	"Cortex.Replay.Scheduler.SameTimeSequenceOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerSameTimeSequenceOrderingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	TArray<FCortexReplayEvent> Events;
	Events.Add(MakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W));
	FCortexReplayEvent Second = MakeKeyEvent(1, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::E);
	Events.Add(Second);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, Events);
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	const FCortexCommandResult Result = AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; },
		MakeUnguardedEvaluator(Snapshot),
		[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); });
	TestTrue(TEXT("Both same-time events accepted"), Result.bSuccess);
	TestEqual(TEXT("Both same-time events dispatched"), Log.Sequences.Num(), 2);
	if (Log.Sequences.Num() == 2)
	{
		TestEqual(TEXT("Stored order retained first"), Log.Sequences[0], 0);
		TestEqual(TEXT("Stored order retained second"), Log.Sequences[1], 1);
		TestTrue(TEXT("Distinct key identity retained second"), Log.Kinds[1] == ECortexEditorPhysicalInputKind::KeyDown);
	}

	return true;
}

// ---------------------------------------------------------------------------
// The monotonic clock alone schedules playback; recorded game-pause diagnostics never do.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerGamePauseIndependentTest,
	"Cortex.Replay.Scheduler.GamePauseIndependentMonotonicClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerGamePauseIndependentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent First = MakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	First.CaptureContext.bWorldPaused = true;
	First.CaptureContext.WorldTimeSeconds = 0.0;
	FCortexReplayEvent Second = MakeKeyEvent(1, 0.2, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W);
	Second.CaptureContext.bWorldPaused = true;
	// The recorded game time does not advance while paused; the monotonic clock still does.
	Second.CaptureContext.WorldTimeSeconds = 0.0;
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {First, Second});
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	TestTrue(TEXT("Paused capture diagnostics do not block the first event"),
		AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; }, MakeUnguardedEvaluator(Snapshot),
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	Elapsed = 0.2;
	TestTrue(TEXT("Paused capture diagnostics do not block the later event"),
		AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; }, MakeUnguardedEvaluator(Snapshot),
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	TestEqual(TEXT("Monotonic time dispatched both events"), Log.Sequences.Num(), 2);

	return true;
}

// ---------------------------------------------------------------------------
// The recorded trailing idle duration stays authoritative after the last event.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerTrailingIdleDurationTest,
	"Cortex.Replay.Scheduler.RecordedTrailingIdleDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerTrailingIdleDurationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.4;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	TestTrue(TEXT("Last event dispatched"),
		AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; }, MakeUnguardedEvaluator(Snapshot),
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	TestEqual(TEXT("Event dispatched"), Scheduler.GetDispatchedCount(), 1);
	TestFalse(TEXT("Not complete while trailing idle remains"), Scheduler.IsComplete());
	Elapsed = 0.3;
	TestTrue(TEXT("Idle poll accepted"),
		AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; }, MakeUnguardedEvaluator(Snapshot),
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	TestFalse(TEXT("Still incomplete before the recorded duration"), Scheduler.IsComplete());
	TestEqual(TEXT("No extra event dispatched while idle"), Scheduler.GetDispatchedCount(), 1);
	Elapsed = 0.4;
	TestTrue(TEXT("Trailing completion poll accepted"),
		AdvanceFrame(Scheduler, [&Elapsed]() { return Elapsed; }, MakeUnguardedEvaluator(Snapshot),
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	TestTrue(TEXT("Complete at the recorded duration"), Scheduler.IsComplete());

	return true;
}

// ---------------------------------------------------------------------------
// An authorized 0.5s readiness wait shifts every later deadline while the recorded trailing
// duration is retained relative to the committed offset.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerShiftedDeadlineTest,
	"Cortex.Replay.Scheduler.ShiftedDeadlineRetainsTrailingDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerShiftedDeadlineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Press;
	Press.Input.Kind = ECortexEditorPhysicalInputKind::PointerDown;
	Press.Input.Key = EKeys::LeftMouseButton;
	FCortexReplayEvent Up = Press;
	Up.Sequence = 1;
	Up.TimeSeconds = 0.25;
	Up.Input.Kind = ECortexEditorPhysicalInputKind::PointerUp;
	auto Recording = Fixture.MakeRecording(1, false, {Press, Up});
	Recording.Metadata.DurationSeconds = 0.5;
	auto Target = MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Target->RootKind = ECortexEditorUIRootKind::Slate;
	Target->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Target->RootTag = TEXT("ReadyRoot");
	Target->TargetTag = TEXT("ReadyControl");
	// Supported selectors must carry a trustworthy digest.
	Target->IdentitySha256 = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(*Target);
	auto& Guard = Recording.Events[0].Guard.GetValue();
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UITarget = Target;
	Guard.ExpectedLocalPosition = FVector2D(0.25, 0.5);
	Recording.Metadata.GuardCoverage.UISupportedPresses = 1;
	Recording.Metadata.GuardCoverage.UINotApplicablePresses = 0;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FCortexEditorPhysicalInputUIObservation UI;
	UI.State = ECortexEditorUIObservationState::TargetDisabled;
	UI.ActualTarget = Target;
	UI.LocalPosition = Snapshot->Events[0].Guard->ExpectedLocalPosition;
	double Elapsed = 0.0;
	TArray<int32> Dispatched;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Evaluate = [&Snapshot, &UI](const FCortexReplayEvent& Event)
	{
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, true);
	};
	auto Dispatch = [&Dispatched](const FCortexReplayEvent& Event)
	{
		Dispatched.Add(Event.Sequence);
		return FCortexCommandRouter::Success(nullptr);
	};
	TestTrue(TEXT("Readiness wait admitted"), AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Pending sequence retained"), Scheduler.GetWaitingSequence(), 0);
	TestEqual(TEXT("Disabled target receives nothing"), Dispatched.Num(), 0);
	Elapsed = 0.5;
	UI.State = ECortexEditorUIObservationState::Ready;
	TestTrue(TEXT("Fresh ready press accepted"), AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Measured wait offset"), Scheduler.GetAuthorizedWaitSeconds(), 0.5);
	TestEqual(TEXT("Later release not compressed"), Dispatched.Num(), 1);
	Elapsed = 0.75;
	TestTrue(TEXT("Release at shifted deadline"), AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Balanced stored edge sequence"), Dispatched.Num(), 2);
	TestFalse(TEXT("Trailing idle remains"), Scheduler.IsComplete());
	Elapsed = 1.0;
	TestTrue(TEXT("Shifted duration completed"), AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestTrue(TEXT("Complete after full duration"), Scheduler.IsComplete());

	return true;
}

// ---------------------------------------------------------------------------
// Every pending poll's evaluation interval is excluded from the committed wait, not only the
// final one, so accumulated evaluation work never shifts later deadlines as authorized wait.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerWaitExcludesEvaluationTest,
	"Cortex.Replay.Scheduler.WaitExcludesEveryEvaluationInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerWaitExcludesEvaluationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Press = MakePointerPressEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::PointerDown, EKeys::LeftMouseButton);
	FSchedulerGuardProbe Probe;
	Probe.Setup(Press);

	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.3;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);

	double Elapsed = 0.0;
	int32 GuardCalls = 0;
	// Simulated guard-evaluation work per poll; the first entry poll is not part of the wait.
	const double EvaluationWork[] = { 0.0, 0.02, 0.02, 0.01 };
	TArray<int32> Dispatched;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Evaluate = [&](const FCortexReplayEvent& Event)
	{
		const FCortexReplayGuardDecision Decision = FCortexReplayGuardEvaluator::Evaluate(
			Event, Snapshot->InitialState.Pose, Probe.UI, true);
		if (GuardCalls < static_cast<int32>(UE_ARRAY_COUNT(EvaluationWork)))
		{
			Elapsed += EvaluationWork[GuardCalls];
		}
		++GuardCalls;
		return Decision;
	};
	auto Dispatch = [&Dispatched](const FCortexReplayEvent& Event)
	{
		Dispatched.Add(Event.Sequence);
		return FCortexCommandRouter::Success(nullptr);
	};

	// The target is disabled, so the run enters its readiness wait at t=0.
	TestTrue(TEXT("Readiness wait admitted"),
		AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Blocked press retained"), Scheduler.GetWaitingSequence(), 0);

	// Two pending polls, each with evaluation work under the 100 ms allowance.
	Elapsed = 0.05;
	TestTrue(TEXT("First pending poll accepted"),
		AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	Elapsed = 0.10;
	TestTrue(TEXT("Second pending poll accepted"),
		AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Still waiting on the same press"), Scheduler.GetWaitingSequence(), 0);

	// The target becomes ready at an observed wait of 0.15 s.
	Probe.MakeReady();
	Elapsed = 0.15;
	TestTrue(TEXT("Fresh ready press accepted"),
		AdvanceFrame(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Press dispatched once ready"), Dispatched.Num(), 1);
	TestEqual(TEXT("Wait state cleared"), Scheduler.GetWaitingSequence(), INDEX_NONE);

	// 0.15 s observed minus 0.04 s of earlier evaluation work is the authorized wait.
	TestEqual(TEXT("Committed wait excludes every evaluation interval"),
		Scheduler.GetAuthorizedWaitSeconds(), 0.11, 1.0e-9);

	return true;
}

// ---------------------------------------------------------------------------
// A classified DoubleClick press is dispatched exactly once; the pending second press never
// becomes an extra Down and is never reclassified from the enlarged wall-clock interval.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerClassifiedDoubleClickOnceTest,
	"Cortex.Replay.Scheduler.ClassifiedDoubleClickDispatchedOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerClassifiedDoubleClickOnceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Down = MakePointerPressEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::PointerDown, EKeys::LeftMouseButton);
	FCortexReplayEvent Up = MakePointerPressEvent(1, 0.05,
		ECortexEditorPhysicalInputKind::PointerUp, EKeys::LeftMouseButton);
	FCortexReplayEvent DoubleClick = MakePointerPressEvent(2, 0.10,
		ECortexEditorPhysicalInputKind::DoubleClick, EKeys::LeftMouseButton);
	FCortexReplayEvent FinalUp = MakePointerPressEvent(3, 0.15,
		ECortexEditorPhysicalInputKind::PointerUp, EKeys::LeftMouseButton);

	FSchedulerGuardProbe Probe;
	Probe.Setup(Down);
	Probe.Setup(DoubleClick);
	// The first press is ready; the classified second press starts pending (disabled target).
	FCortexEditorPhysicalInputUIObservation ReadyUI = Probe.UI;
	ReadyUI.State = ECortexEditorUIObservationState::Ready;
	FCortexEditorPhysicalInputUIObservation PendingUI = Probe.UI;

	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Down, Up, DoubleClick, FinalUp});
	Recording.Metadata.DurationSeconds = 0.2;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Evaluate = [&Snapshot, &ReadyUI, &PendingUI](const FCortexReplayEvent& Event)
	{
		const FCortexEditorPhysicalInputUIObservation& UI =
			(Event.Sequence == 2) ? PendingUI : ReadyUI;
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, UI, true);
	};

	TestTrue(TEXT("First press accepted"),
		AdvanceFrame(Scheduler, Clock, Evaluate,
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	Elapsed = 0.10;
	// The classified second press is initially not ready, so the run waits without consuming it.
	TestTrue(TEXT("Pending second press waits"),
		AdvanceFrame(Scheduler, Clock, Evaluate,
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	TestEqual(TEXT("Second press retained pending"), Scheduler.GetWaitingSequence(), 2);
	TestEqual(TEXT("Pending press not consumed before readiness"), Log.Sequences.Num(), 2);
	PendingUI.State = ECortexEditorUIObservationState::Ready;
	TestTrue(TEXT("Classified press dispatched once ready"),
		AdvanceFrame(Scheduler, Clock, Evaluate,
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);
	Elapsed = 0.2;
	TestTrue(TEXT("Final release dispatched"),
		AdvanceFrame(Scheduler, Clock, Evaluate,
			[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); }).bSuccess);

	TestEqual(TEXT("Stored edge sequence dispatched once"), Log.Sequences.Num(), 4);
	if (Log.Sequences.Num() == 4)
	{
		TestTrue(TEXT("No invented second Down"), Log.Kinds[0] == ECortexEditorPhysicalInputKind::PointerDown);
		TestTrue(TEXT("PointerUp retained"), Log.Kinds[1] == ECortexEditorPhysicalInputKind::PointerUp);
		TestTrue(TEXT("Classified DoubleClick retained once"), Log.Kinds[2] == ECortexEditorPhysicalInputKind::DoubleClick);
		TestTrue(TEXT("Final PointerUp retained"), Log.Kinds[3] == ECortexEditorPhysicalInputKind::PointerUp);
	}

	return true;
}

// ---------------------------------------------------------------------------
// The committed frame frontier - not dispatched events and not the recorded duration - decides
// completion, and only an explicit Commit advances it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameTailCommitTest,
	"Cortex.Replay.Scheduler.FrameTailCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameTailCommitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.4;
	Fixture.ApplyCadenceFrames(Recording);
	TestEqual(TEXT("Cadence fixture has a warmup, an input frame and an empty tail"),
		Recording.Frames.Num(), 3);
	TestEqual(TEXT("The tail frame carries no input"), Recording.Frames.Last().EventCount, 0);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	TestFalse(TEXT("Nothing is complete before any frame commits"), Scheduler.IsComplete());
	TestFalse(TEXT("No frame is in preparation before the first poll"), Scheduler.GetCurrentFrame().IsSet());

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	TestTrue(TEXT("Warmup frame prepares"),
		Scheduler.PrepareFrame(0, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Warmup frame is ready"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestEqual(TEXT("Preparing does not count as committed"), Scheduler.GetCompletedFrameCount(), 0);
	TestEqual(TEXT("The warmup frame is the frame in preparation"), Scheduler.GetCurrentFrame().GetValue(), 0);

	TestTrue(TEXT("Warmup frame commits"), Scheduler.CommitFrame(0).bSuccess);
	TestEqual(TEXT("Commit advances the frontier"), Scheduler.GetCompletedFrameCount(), 1);
	TestFalse(TEXT("No frame stays in preparation after commit"), Scheduler.GetCurrentFrame().IsSet());

	TestTrue(TEXT("Input frame prepares"),
		Scheduler.PrepareFrame(1, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestTrue(TEXT("Input frame commits"), Scheduler.CommitFrame(1).bSuccess);
	TestEqual(TEXT("Both the warmup and the input frame dispatched their ranges"), Log.Sequences.Num(), 1);

	// The trailing frame carries no events, so only its own deadline can hold completion open.
	TestTrue(TEXT("Empty tail is not yet due"),
		Scheduler.PrepareFrame(2, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("An empty frame that is not due stays waiting"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Waiting));
	TestFalse(TEXT("An uncommitted empty tail keeps the run incomplete"), Scheduler.IsComplete());

	Elapsed = 0.4;
	TestTrue(TEXT("Empty tail prepares at its deadline"),
		Scheduler.PrepareFrame(2, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Empty tail is ready at its deadline"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestFalse(TEXT("Completion is not claimed before the final close"), Scheduler.IsComplete());
	TestTrue(TEXT("Empty tail commits"), Scheduler.CommitFrame(2).bSuccess);
	TestTrue(TEXT("Completion follows the final committed close"), Scheduler.IsComplete());
	TestEqual(TEXT("Every frame is committed"), Scheduler.GetCompletedFrameCount(), 3);

	const FCortexCommandResult DuplicateCommit = Scheduler.CommitFrame(2);
	TestFalse(TEXT("A duplicate commit is refused"), DuplicateCommit.bSuccess);
	TestTrue(TEXT("A completed run polls as a no-op"),
		Scheduler.PrepareFrame(3, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);

	return true;
}

// ---------------------------------------------------------------------------
// Lateness is enforced at the frame's own range inside the frame API: an overdue frame errors and
// its frontier does not move.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameBundleLatenessTest,
	"Cortex.Replay.Scheduler.FrameBundleLateness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameBundleLatenessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.4;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	TestTrue(TEXT("Warmup frame prepares"),
		Scheduler.PrepareFrame(0, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestTrue(TEXT("Warmup frame commits"), Scheduler.CommitFrame(0).bSuccess);

	// The input frame's only event is due at 0.0, so 101 ms of lateness is over the fixed allowance.
	Elapsed = 0.101;
	const FCortexCommandResult Late =
		Scheduler.PrepareFrame(1, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation);
	TestFalse(TEXT("A frame overdue beyond the fixed allowance fails"), Late.bSuccess);
	TestEqual(TEXT("Frame lateness reports REPLAY_TIMING_ERROR"), Late.ErrorCode,
		FString(TEXT("REPLAY_TIMING_ERROR")));
	TestEqual(TEXT("The late frame dispatched nothing"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("The late frame did not advance the frontier"), Scheduler.GetCompletedFrameCount(), 1);
	TestFalse(TEXT("The late frame is not complete"), Scheduler.IsComplete());

	const FCortexCommandResult LateCommit = Scheduler.CommitFrame(1);
	TestFalse(TEXT("An unprepared frame cannot be committed"), LateCommit.bSuccess);

	return true;
}

// ---------------------------------------------------------------------------
// A guard failure inside a frame freezes that frame's remaining range: nothing is replayed and the
// scheduler never drains the next frame.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameReentrantCancelTest,
	"Cortex.Replay.Scheduler.FrameReentrantCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameReentrantCancelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayEvent First = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplayEvent Second = MakeKeyEvent(1, 0.0,
		ECortexEditorPhysicalInputKind::KeyUp, EKeys::W);
	Second.TimeSeconds = 0.05;
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {First, Second});
	Recording.Metadata.DurationSeconds = 0.3;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	// The warmup frame carries no events, so the first guard evaluation happens on the input frame.
	auto Evaluate = [](const FCortexReplayEvent&)
	{
		FCortexReplayGuardDecision Decision;
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = FCortexCommandRouter::Error(
			FString(TEXT("REPLAY_POSE_GUARD_FAILED")), FString(TEXT("pose mismatch")));
		return Decision;
	};

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	TestTrue(TEXT("Warmup frame prepares"),
		Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestTrue(TEXT("Warmup frame commits"), Scheduler.CommitFrame(0).bSuccess);

	Elapsed = 0.0;
	const FCortexCommandResult Failed = Scheduler.PrepareFrame(1, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A guard failure fails the frame preparation"), Failed.bSuccess);
	TestEqual(TEXT("The guard failure keeps its own code"), Failed.ErrorCode,
		FString(TEXT("REPLAY_POSE_GUARD_FAILED")));
	TestEqual(TEXT("Nothing was dispatched by the failed frame"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("The failed frame did not advance the frontier"), Scheduler.GetCompletedFrameCount(), 1);

	// A repeated poll must fail the same way: the remaining range is frozen and the next frame is
	// never drained into.
	Elapsed = 0.2;
	const FCortexCommandResult Repeated = Scheduler.PrepareFrame(1, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A repeated poll stays failed"), Repeated.bSuccess);
	TestEqual(TEXT("Still nothing delivered after the failure"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("Still no frontier movement after the failure"), Scheduler.GetCompletedFrameCount(), 1);
	TestFalse(TEXT("The frozen run never completes"), Scheduler.IsComplete());

	return true;
}

// ---------------------------------------------------------------------------
// A blocked Slate press spends real time against a fixed per-press budget; a pending poll commits
// no budget, and reaching the budget times the run out instead of consuming the press.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameWaitBudgetTest,
	"Cortex.Replay.Scheduler.FrameWaitBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameWaitBudgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FSchedulerGuardProbe Probe;
	Probe.Setup(Press);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.4;
	Fixture.ApplyCadenceFrames(Recording);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Evaluate = [&Snapshot, &Probe](const FCortexReplayEvent& Event)
	{
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, Probe.UI, true);
	};
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	TestTrue(TEXT("Warmup frame prepares"),
		Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestTrue(TEXT("Warmup frame commits"), Scheduler.CommitFrame(0).bSuccess);

	// A disabled Slate target is never ready, so the press spends its budget waiting on it.
	Elapsed = 0.0;
	TestTrue(TEXT("Wait admitted"),
		Scheduler.PrepareFrame(1, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("A disabled target makes the press wait"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Waiting));
	TestEqual(TEXT("The blocked press stays pending"), Scheduler.GetWaitingSequence(), 0);
	TestEqual(TEXT("Nothing is dispatched while waiting"), Log.Sequences.Num(), 0);

	Elapsed = 0.9;
	TestTrue(TEXT("A wait inside its budget keeps waiting"),
		Scheduler.PrepareFrame(1, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Still waiting below the per-press budget"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Waiting));
	TestEqual(TEXT("A pending poll commits no wait budget"), Scheduler.GetAuthorizedWaitSeconds(), 0.0);

	Elapsed = 1.0;
	const FCortexCommandResult TimedOut =
		Scheduler.PrepareFrame(1, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A wait that reaches the per-press budget fails"), TimedOut.bSuccess);
	TestEqual(TEXT("The wait timeout reports REPLAY_UI_WAIT_TIMEOUT"), TimedOut.ErrorCode,
		FString(TEXT("REPLAY_UI_WAIT_TIMEOUT")));
	TestEqual(TEXT("The timed-out press was never dispatched"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("The timed-out frame did not advance the frontier"),
		Scheduler.GetCompletedFrameCount(), 1);
	TestFalse(TEXT("The timed-out frame is not complete"), Scheduler.IsComplete());

	return true;
}
