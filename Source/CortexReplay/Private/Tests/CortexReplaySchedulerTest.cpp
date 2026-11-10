#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"

namespace
{
FCortexReplayTimedEvent MakeKeyEvent(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayTimedEvent Timed;
	Timed.TimeSeconds = TimeSeconds;
	Timed.Event.Sequence = Sequence;
	Timed.Event.Input.Kind = Kind;
	Timed.Event.Input.Key = Key;
	return Timed;
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

	void Setup(FCortexReplayTimedEvent& Timed)
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
		Timed.Event.Guard = Guard;

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

/**
 * Steps engine frames the way production does - one preparation and at most one commit per frame -
 * until the run completes or blocks on a UI wait. Returns the first failing result.
 */
FCortexCommandResult StepFramesUntilWaitOrComplete(FCortexReplayScheduler& Scheduler,
	TFunctionRef<double()> Clock,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> Evaluate,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch)
{
	for (;;)
	{
		const FCortexCommandResult Stepped = PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch);
		if (!Stepped.bSuccess || Scheduler.IsComplete() || Scheduler.GetWaitingSequence() != INDEX_NONE)
		{
			return Stepped;
		}
	}
}
}

// ---------------------------------------------------------------------------
// Events sharing one frame dispatch in stored sequence order within that frame's one preparation.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerSameTimeSequenceOrderingTest,
	"Cortex.Replay.Scheduler.SameTimeSequenceOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerSameTimeSequenceOrderingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	TArray<FCortexReplayTimedEvent> Events;
	Events.Add(MakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W));
	Events.Add(MakeKeyEvent(1, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::E));
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, Events);
	FCortexReplayTestFixture::ApplyCadenceFrames(Recording, {0.0, 0.0});
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	const FCortexCommandResult Result = PrepareAndCommit(Scheduler, [&Elapsed]() { return Elapsed; },
		MakeUnguardedEvaluator(Snapshot),
		[&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); });
	TestTrue(TEXT("Both same-frame events accepted"), Result.bSuccess);
	TestEqual(TEXT("One preparation dispatched both events"), Log.Sequences.Num(), 2);
	TestEqual(TEXT("One frame committed"), Scheduler.GetCompletedFrameCount(), 1);
	if (Log.Sequences.Num() == 2)
	{
		TestEqual(TEXT("Stored order retained first"), Log.Sequences[0], 0);
		TestEqual(TEXT("Stored order retained second"), Log.Sequences[1], 1);
		TestTrue(TEXT("Distinct key identity retained second"), Log.Kinds[1] == ECortexEditorPhysicalInputKind::KeyDown);
	}

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
	FCortexReplayTimedEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::PointerDown, EKeys::LeftMouseButton);
	FSchedulerGuardProbe Probe;
	Probe.Setup(Press);

	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.3;
	FCortexReplayTestFixture::ApplyCadenceFrames(Recording, {0.0});
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
		PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Blocked press retained"), Scheduler.GetWaitingSequence(), 0);

	// Two pending polls, each carrying its own evaluation work.
	Elapsed = 0.05;
	TestTrue(TEXT("First pending poll accepted"),
		PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	Elapsed = 0.10;
	TestTrue(TEXT("Second pending poll accepted"),
		PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Still waiting on the same press"), Scheduler.GetWaitingSequence(), 0);

	// The target becomes ready at an observed wait of 0.15 s.
	Probe.MakeReady();
	Elapsed = 0.15;
	TestTrue(TEXT("Fresh ready press accepted"),
		PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
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
	FCortexReplayTimedEvent Down = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::PointerDown, EKeys::LeftMouseButton);
	FCortexReplayTimedEvent Up = MakeKeyEvent(1, 0.05,
		ECortexEditorPhysicalInputKind::PointerUp, EKeys::LeftMouseButton);
	FCortexReplayTimedEvent DoubleClick = MakeKeyEvent(2, 0.10,
		ECortexEditorPhysicalInputKind::DoubleClick, EKeys::LeftMouseButton);
	FCortexReplayTimedEvent FinalUp = MakeKeyEvent(3, 0.15,
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
	FCortexReplayTestFixture::ApplyCadenceFrames(Recording, {0.0, 0.05, 0.10, 0.15});
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
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	// The first press and its release play on their own frames; the classified second press is
	// initially not ready, so the run waits without consuming it.
	TestTrue(TEXT("Frames up to the pending press accepted"),
		StepFramesUntilWaitOrComplete(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Second press retained pending"), Scheduler.GetWaitingSequence(), 2);
	TestEqual(TEXT("Pending press not consumed before readiness"), Log.Sequences.Num(), 2);

	Elapsed = 0.10;
	PendingUI.State = ECortexEditorUIObservationState::Ready;
	TestTrue(TEXT("Classified press dispatched once ready, and the run plays on"),
		StepFramesUntilWaitOrComplete(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestTrue(TEXT("The run completed"), Scheduler.IsComplete());

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
// completion, and only an explicit Commit advances it. Frames without input are ready at once.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameTailCommitTest,
	"Cortex.Replay.Scheduler.FrameTailCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameTailCommitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayTimedEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	const double Deltas[] = { 0.1, 0.1, 0.1 };
	const int32 EventFrames[] = { 0 };
	FCortexReplayTestFixture::ApplyFrameStream(Recording, Deltas, EventFrames);
	TestEqual(TEXT("One input frame and two empty frames"), Recording.Frames.Num(), 3);
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
	TestTrue(TEXT("Input frame prepares"),
		Scheduler.PrepareFrame(0, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Input frame is ready"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestEqual(TEXT("Preparing does not count as committed"), Scheduler.GetCompletedFrameCount(), 0);
	TestEqual(TEXT("The input frame is the frame in preparation"), Scheduler.GetCurrentFrame().GetValue(), 0);
	TestFalse(TEXT("A frame other than the next one cannot be committed"), Scheduler.CommitFrame(1).bSuccess);

	TestTrue(TEXT("Input frame commits"), Scheduler.CommitFrame(0).bSuccess);
	TestEqual(TEXT("Commit advances the frontier"), Scheduler.GetCompletedFrameCount(), 1);
	TestFalse(TEXT("No frame stays in preparation after commit"), Scheduler.GetCurrentFrame().IsSet());

	TestTrue(TEXT("Empty frame prepares"),
		Scheduler.PrepareFrame(1, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Empty frame is ready"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestTrue(TEXT("Empty frame commits"), Scheduler.CommitFrame(1).bSuccess);
	TestEqual(TEXT("Only the input frame dispatched"), Log.Sequences, TArray<int32>({ 0 }));
	TestFalse(TEXT("An uncommitted tail keeps the run incomplete"), Scheduler.IsComplete());

	TestTrue(TEXT("Empty tail prepares"),
		Scheduler.PrepareFrame(2, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestFalse(TEXT("Completion is not claimed before the final commit"), Scheduler.IsComplete());
	TestTrue(TEXT("Empty tail commits"), Scheduler.CommitFrame(2).bSuccess);
	TestTrue(TEXT("Completion follows the final committed frame"), Scheduler.IsComplete());
	TestEqual(TEXT("Every frame is committed"), Scheduler.GetCompletedFrameCount(), 3);

	TestFalse(TEXT("A duplicate commit is refused"), Scheduler.CommitFrame(2).bSuccess);
	TestTrue(TEXT("A completed run polls as a no-op"),
		Scheduler.PrepareFrame(3, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);

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
	const FCortexReplayTimedEvent First = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	const FCortexReplayTimedEvent Second = MakeKeyEvent(1, 0.05,
		ECortexEditorPhysicalInputKind::KeyUp, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {First, Second});
	Recording.Metadata.DurationSeconds = 0.3;
	FCortexReplayTestFixture::ApplyCadenceFrames(Recording, {0.0, 0.05});
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	auto Evaluate = [](const FCortexReplayEvent&)
	{
		FCortexReplayGuardDecision Decision;
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = FCortexCommandRouter::Error(
			FString(TEXT("REPLAY_POSE_GUARD_FAILED")), FString(TEXT("pose mismatch")));
		return Decision;
	};

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	const FCortexCommandResult Failed = Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A guard failure fails the frame preparation"), Failed.bSuccess);
	TestEqual(TEXT("The guard failure keeps its own code"), Failed.ErrorCode,
		FString(TEXT("REPLAY_POSE_GUARD_FAILED")));
	TestEqual(TEXT("Nothing was dispatched by the failed frame"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("The failed frame did not advance the frontier"), Scheduler.GetCompletedFrameCount(), 0);

	// A repeated poll must fail the same way: the remaining range is frozen and the next frame is
	// never drained into.
	Elapsed = 0.2;
	const FCortexCommandResult Repeated = Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A repeated poll stays failed"), Repeated.bSuccess);
	TestEqual(TEXT("Still nothing delivered after the failure"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("Still no frontier movement after the failure"), Scheduler.GetCompletedFrameCount(), 0);
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
	FCortexReplayTimedEvent Press = MakeKeyEvent(0, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	FSchedulerGuardProbe Probe;
	Probe.Setup(Press);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Press});
	Recording.Metadata.DurationSeconds = 0.4;
	FCortexReplayTestFixture::ApplyCadenceFrames(Recording, {0.0});
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
	const double PerPressBudget = CortexReplayWaitBudget::MaxWaitPerPressSeconds;

	// A disabled Slate target is never ready, so the press spends its budget waiting on it.
	Elapsed = 0.0;
	TestTrue(TEXT("Wait admitted"),
		Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("A disabled target makes the press wait"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Waiting));
	TestEqual(TEXT("The blocked press stays pending"), Scheduler.GetWaitingSequence(), 0);
	TestEqual(TEXT("Nothing is dispatched while waiting"), Log.Sequences.Num(), 0);

	Elapsed = PerPressBudget * 0.9;
	TestTrue(TEXT("A wait inside its budget keeps waiting"),
		Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("Still waiting below the per-press budget"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Waiting));
	TestEqual(TEXT("A pending poll commits no wait budget"), Scheduler.GetAuthorizedWaitSeconds(), 0.0);

	Elapsed = PerPressBudget;
	const FCortexCommandResult TimedOut =
		Scheduler.PrepareFrame(0, Clock, Evaluate, Dispatch, Preparation);
	TestFalse(TEXT("A wait that reaches the per-press budget fails"), TimedOut.bSuccess);
	TestEqual(TEXT("The wait timeout reports REPLAY_UI_WAIT_TIMEOUT"), TimedOut.ErrorCode,
		FString(TEXT("REPLAY_UI_WAIT_TIMEOUT")));
	TestEqual(TEXT("The timed-out press was never dispatched"), Log.Sequences.Num(), 0);
	TestEqual(TEXT("The timed-out frame did not advance the frontier"),
		Scheduler.GetCompletedFrameCount(), 0);
	TestFalse(TEXT("The timed-out frame is not complete"), Scheduler.IsComplete());

	return true;
}

// ---------------------------------------------------------------------------
// Replay is frame-locked: a recorded frame's whole range is dispatched by the preparation of that
// frame, whatever the wall clock says. A frame whose timestamps are far ahead of the clock is not held
// back, and one far behind the clock is not a timing failure - the engine frame, not wall time, is
// what consumes recorded input.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerFrameLockedPreparationTest,
	"Cortex.Replay.Scheduler.FrameLockedPreparationIgnoresClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerFrameLockedPreparationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const FCortexReplayTimedEvent Down = MakeKeyEvent(0, 10.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	const FCortexReplayTimedEvent Look = MakeKeyEvent(1, 10.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::E);
	const FCortexReplayTimedEvent Up = MakeKeyEvent(2, 10.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W);
	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Down, Look, Up});
	const double Deltas[] = { 0.1, 0.1 };
	const int32 EventFrames[] = { 0, 0, 1 };
	FCortexReplayTestFixture::ApplyFrameStream(Recording, Deltas, EventFrames);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;
	double Elapsed = 0.0;
	auto Clock = [&Elapsed]() { return Elapsed; };
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	// The frame's events are timestamped ten seconds after the clock.
	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	TestTrue(TEXT("A frame ahead of the clock prepares"),
		Scheduler.PrepareFrame(0, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation).bSuccess);
	TestEqual(TEXT("A frame ahead of the clock is ready in one preparation"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestEqual(TEXT("Its whole range was dispatched"), Log.Sequences, TArray<int32>({ 0, 1 }));
	TestTrue(TEXT("The frame commits"), Scheduler.CommitFrame(0).bSuccess);

	// Twenty seconds behind the clock is not lateness: the frame is still dispatched whole.
	Elapsed = 30.0;
	const FCortexCommandResult Behind =
		Scheduler.PrepareFrame(1, Clock, MakeUnguardedEvaluator(Snapshot), Dispatch, Preparation);
	TestTrue(TEXT("A frame behind the clock is not a timing failure"), Behind.bSuccess);
	TestEqual(TEXT("A frame behind the clock is ready"), static_cast<int32>(Preparation),
		static_cast<int32>(ECortexReplayFramePreparation::Ready));
	TestEqual(TEXT("The late frame's range was dispatched in order"), Log.Sequences, TArray<int32>({ 0, 1, 2 }));
	return true;
}

// ---------------------------------------------------------------------------
// The clock only measures UI waits: an ordinary frame never reads it, so even a broken clock cannot
// fail a run that never waits; entering a wait reads it once and each poll twice (around the guard
// evaluation), and only those reads can raise REPLAY_TIMING_ERROR.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplaySchedulerClockReadOnlyForWaitsTest,
	"Cortex.Replay.Scheduler.ClockReadOnlyForWaits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplaySchedulerClockReadOnlyForWaitsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayTimedEvent Press = MakeKeyEvent(2, 0.0,
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	const FCortexReplayTimedEvent Plain = MakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::E);
	const FCortexReplayTimedEvent Release = MakeKeyEvent(1, 0.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::E);
	FSchedulerGuardProbe Probe;
	Probe.Setup(Press);

	FCortexReplaySnapshot Recording = Fixture.MakeRecording(1, false, {Plain, Release, Press});
	const double Deltas[] = { 0.1, 0.1 };
	const int32 EventFrames[] = { 0, 0, 1 };
	FCortexReplayTestFixture::ApplyFrameStream(Recording, Deltas, EventFrames);
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(MoveTemp(Recording));
	FCortexReplayScheduler Scheduler(Snapshot);
	FSchedulerDispatchLog Log;

	int32 ClockReads = 0;
	double ClockValue = 0.0;
	bool bBrokenClock = true;
	auto Clock = [&]()
	{
		++ClockReads;
		return bBrokenClock ? std::numeric_limits<double>::quiet_NaN() : ClockValue;
	};
	auto Evaluate = [&Snapshot, &Probe](const FCortexReplayEvent& Event)
	{
		return FCortexReplayGuardEvaluator::Evaluate(Event, Snapshot->InitialState.Pose, Probe.UI, true);
	};
	auto Dispatch = [&Log](const FCortexReplayEvent& Event) { return Log.Record(Event); };

	// Frame 0 holds two unguarded events: no wait, so the (broken) clock is never touched.
	TestTrue(TEXT("An ordinary frame does not depend on the clock"),
		PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("An ordinary frame never reads the clock"), ClockReads, 0);
	TestEqual(TEXT("Both events of the ordinary frame dispatched"), Log.Sequences.Num(), 2);

	// Frame 1's press is blocked: entering the wait is the first clock read, and a broken one fails.
	const FCortexCommandResult Broken = PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch);
	TestFalse(TEXT("A broken clock fails the wait it measures"), Broken.bSuccess);
	TestEqual(TEXT("The failure is REPLAY_TIMING_ERROR"), Broken.ErrorCode,
		FString(TEXT("REPLAY_TIMING_ERROR")));
	TestEqual(TEXT("Entering the wait read the clock once"), ClockReads, 1);
	TestEqual(TEXT("The press was not consumed"), Log.Sequences.Num(), 2);

	// With a sound clock the wait is entered (one read), then each poll reads twice.
	bBrokenClock = false;
	ClockReads = 0;
	TestTrue(TEXT("The wait is entered"), PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("Entering a wait reads the clock once"), ClockReads, 1);
	TestEqual(TEXT("The press is pending"), Scheduler.GetWaitingSequence(), 2);

	ClockValue = 0.05;
	ClockReads = 0;
	TestTrue(TEXT("A pending poll is accepted"), PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch).bSuccess);
	TestEqual(TEXT("A poll reads the clock around its evaluation"), ClockReads, 2);

	// A backward observation during a poll is a timing error.
	ClockValue = 0.01;
	const FCortexCommandResult Backward = PrepareAndCommit(Scheduler, Clock, Evaluate, Dispatch);
	TestFalse(TEXT("A backward clock fails the poll"), Backward.bSuccess);
	TestEqual(TEXT("The backward failure is REPLAY_TIMING_ERROR"), Backward.ErrorCode,
		FString(TEXT("REPLAY_TIMING_ERROR")));
	return true;
}
