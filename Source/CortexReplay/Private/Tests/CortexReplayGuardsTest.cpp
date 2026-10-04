#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"

#include <limits>

namespace
{
TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> MakeTarget(
	const FString& RootTag, const FString& TargetTag)
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::Slate;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Identity->RootTag = RootTag;
	Identity->TargetTag = TargetTag;
	return TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
}

FCortexReplayEvent MakePress(ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event;
	Event.Input.Kind = Kind;
	Event.Input.Key = Key;
	return Event;
}

FCortexReplayEvent MakeGuardedPress(ECortexEditorUICoverage Coverage,
	const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>& Target,
	const FVector2D& LocalPosition)
{
	FCortexReplayEvent Event = MakePress(ECortexEditorPhysicalInputKind::PointerDown,
		EKeys::LeftMouseButton);
	FCortexReplayInteractionGuard Guard;
	Guard.ExpectedPose = FCortexEditorPhysicalInputPlayerPose();
	Guard.UICoverage = Coverage;
	switch (Coverage)
	{
	case ECortexEditorUICoverage::Supported:
		Guard.UITarget = Target;
		Guard.ExpectedLocalPosition = LocalPosition;
		break;
	case ECortexEditorUICoverage::Unavailable:
		Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::AnonymousSlate;
		break;
	default:
		break;
	}
	Event.Guard = Guard;
	return Event;
}

FCortexEditorPhysicalInputUIObservation MakeObservation(ECortexEditorUIObservationState State,
	const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>& ActualTarget,
	const FVector2D& LocalPosition)
{
	FCortexEditorPhysicalInputUIObservation Observation;
	Observation.State = State;
	Observation.ActualTarget = ActualTarget;
	Observation.LocalPosition = LocalPosition;
	return Observation;
}

/** A tagged, resolvable Slate target used by every supported-coverage case. */
struct FGuardTargetProbe
{
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Target = MakeTarget(
		TEXT("GuardRoot"), TEXT("GuardControl"));
	FVector2D ExpectedLocal = FVector2D(0.25, 0.5);
};

/** The fixed partial-coverage recording the plan describes: one supported and one unavailable press. */
FCortexReplaySnapshot MakePartialCoverageRecording(const TArray<FCortexReplayEvent>& Events)
{
	FCortexReplaySnapshot Recording;
	Recording.Events = Events;
	Recording.Metadata.GuardCoverage.UISupportedPresses = 1;
	Recording.Metadata.GuardCoverage.UIUnavailablePresses = 1;
	Recording.Metadata.GuardCoverage.UINotApplicablePresses = 0;
	return Recording;
}
}

// ---------------------------------------------------------------------------
// Unguarded edges never consult pose or UI.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsUnguardedEdgesReadyTest,
	"Cortex.Replay.Guards.UnguardedEdgesReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsUnguardedEdgesReadyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FCortexEditorPhysicalInputPlayerPose Pose;
	const FCortexEditorPhysicalInputUIObservation NoObservation;

	const FCortexReplayEvent KeyUp = MakePress(
		ECortexEditorPhysicalInputKind::KeyUp, EKeys::W);
	const FCortexReplayEvent Move = MakePress(
		ECortexEditorPhysicalInputKind::PointerMove, EKeys::MouseX);
	const FCortexReplayEvent Relative = MakePress(
		ECortexEditorPhysicalInputKind::RelativeMove, EKeys::MouseX);
	const FCortexReplayEvent Wheel = MakePress(
		ECortexEditorPhysicalInputKind::Wheel, EKeys::MouseWheelAxis);
	FCortexReplayEvent Repeat = MakePress(
		ECortexEditorPhysicalInputKind::KeyDown, EKeys::W);
	Repeat.Input.bRepeat = true;

	const FCortexReplayEvent* const UnguardedEdges[] = {&KeyUp, &Move, &Relative, &Wheel, &Repeat};
	for (const FCortexReplayEvent* Event : UnguardedEdges)
	{
		const FCortexReplayGuardDecision Decision =
			FCortexReplayGuardEvaluator::Evaluate(*Event, Pose, NoObservation, false);
		TestTrue(TEXT("Unguarded edge is ready"), Decision.State == ECortexReplayGuardDecisionState::Ready);
	}

	return true;
}

// ---------------------------------------------------------------------------
// One-centimeter, rotation and scale boundaries through the real comparator.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsPoseBoundariesTest,
	"Cortex.Replay.Guards.PoseToleranceBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsPoseBoundariesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPhysicalInputPlayerPose Expected;
	Expected.PawnTransform = FTransform::Identity;
	Expected.ControlRotation = FRotator::ZeroRotator;

	TestTrue(TEXT("Identical pose accepted"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, Expected).bSuccess);

	// Location: the fixed tolerance is 0.5cm, so a one-centimeter shortfall fails.
	FCortexEditorPhysicalInputPlayerPose Translated = Expected;
	Translated.PawnTransform.AddToTranslation(FVector(1.0, 0.0, 0.0));
	const FCortexCommandResult OneCentimeter = FCortexReplayGuardEvaluator::CheckPose(Expected, Translated);
	TestFalse(TEXT("One centimeter shortfall fails"), OneCentimeter.bSuccess);
	TestEqual(TEXT("Pose mismatch remaps to the Replay guard code"), OneCentimeter.ErrorCode,
		FString(TEXT("REPLAY_POSE_GUARD_FAILED")));
	TestTrue(TEXT("Bounded deviation details are preserved"), OneCentimeter.ErrorDetails.IsValid());

	// Control rotation: the fixed tolerance is 0.5 degrees.
	FCortexEditorPhysicalInputPlayerPose Rotated = Expected;
	Rotated.ControlRotation = FRotator(0.0, 0.6, 0.0);
	TestFalse(TEXT("Rotation beyond 0.5 degrees fails"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, Rotated).bSuccess);
	Rotated.ControlRotation = FRotator(0.0, 0.4, 0.0);
	TestTrue(TEXT("Rotation within 0.5 degrees accepted"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, Rotated).bSuccess);

	// Pawn rotation uses the same shortest-quaternion angular bound.
	FCortexEditorPhysicalInputPlayerPose PawnRotated = Expected;
	PawnRotated.PawnTransform.SetRotation(FQuat(FRotator(0.0, 0.6, 0.0)));
	TestFalse(TEXT("Pawn rotation beyond 0.5 degrees fails"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, PawnRotated).bSuccess);

	// Scale: the fixed component tolerance is 0.001.
	FCortexEditorPhysicalInputPlayerPose Scaled = Expected;
	Scaled.PawnTransform.SetScale3D(FVector(1.002, 1.0, 1.0));
	TestFalse(TEXT("Scale delta of 0.002 fails"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, Scaled).bSuccess);
	Scaled.PawnTransform.SetScale3D(FVector(1.0005, 1.0, 1.0));
	TestTrue(TEXT("Scale delta within 0.001 accepted"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, Scaled).bSuccess);

	// Non-finite and degenerate poses are rejected outright, never treated as matching.
	FCortexEditorPhysicalInputPlayerPose NonFinite = Expected;
	NonFinite.PawnTransform.SetLocation(FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
	TestFalse(TEXT("Non-finite pose rejected"),
		FCortexReplayGuardEvaluator::CheckPose(Expected, NonFinite).bSuccess);

	return true;
}

// ---------------------------------------------------------------------------
// Evaluate compares the pose first for a guarded press and requires a finite ActualPose.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsEvaluatePoseFirstTest,
	"Cortex.Replay.Guards.EvaluatePoseMismatchRemap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsEvaluatePoseFirstTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent Event = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);

	FCortexEditorPhysicalInputPlayerPose Actual = Event.Guard->ExpectedPose;
	Actual.PawnTransform.AddToTranslation(FVector(1.0, 0.0, 0.0));

	// Even with a ready, matching UI observation, the pose is compared first.
	const FCortexReplayGuardDecision Decision = FCortexReplayGuardEvaluator::Evaluate(Event, Actual,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target, Probe.ExpectedLocal), true);
	TestTrue(TEXT("Mismatched pose errors"), Decision.State == ECortexReplayGuardDecisionState::Error);
	TestEqual(TEXT("Specific pose failure"), Decision.Error.ErrorCode,
		FString(TEXT("REPLAY_POSE_GUARD_FAILED")));

	// A non-finite observed pose can never satisfy a guarded press.
	FCortexEditorPhysicalInputPlayerPose NonFinite = Event.Guard->ExpectedPose;
	NonFinite.PawnTransform.SetLocation(FVector(0.0, std::numeric_limits<double>::quiet_NaN(), 0.0));
	const FCortexReplayGuardDecision NonFiniteDecision = FCortexReplayGuardEvaluator::Evaluate(Event,
		NonFinite, MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target,
			Probe.ExpectedLocal), true);
	TestTrue(TEXT("Non-finite pose errors"), NonFiniteDecision.State == ECortexReplayGuardDecisionState::Error);

	return true;
}

// ---------------------------------------------------------------------------
// NotApplicable and Unavailable skip only the UI identity check and keep the pose guard.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsNotApplicableUnavailableTest,
	"Cortex.Replay.Guards.NotApplicableAndUnavailableSkipUI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsNotApplicableUnavailableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexEditorPhysicalInputPlayerPose Pose;

	// NotApplicable: a pending observation must not block a key press.
	{
		const FCortexReplayEvent Event = MakeGuardedPress(ECortexEditorUICoverage::NotApplicable,
			nullptr, FVector2D::ZeroVector);
		const FCortexReplayGuardDecision Decision = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
			MakeObservation(ECortexEditorUIObservationState::PointerPending, Probe.Target,
				FVector2D(0.9, 0.9)), false);
		TestTrue(TEXT("NotApplicable ignores UI identity"), Decision.State == ECortexReplayGuardDecisionState::Ready);
	}

	// Unavailable: keep the pose guard, do not claim UI protection.
	{
		const FCortexReplayEvent Event = MakeGuardedPress(ECortexEditorUICoverage::Unavailable,
			nullptr, FVector2D::ZeroVector);
		const FCortexReplayGuardDecision Decision = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
			MakeObservation(ECortexEditorUIObservationState::WrongTarget, Probe.Target,
				FVector2D(0.9, 0.9)), false);
		TestTrue(TEXT("Unavailable skips UI identity"), Decision.State == ECortexReplayGuardDecisionState::Ready);

		FCortexEditorPhysicalInputPlayerPose Drifted = Pose;
		Drifted.PawnTransform.AddToTranslation(FVector(1.0, 0.0, 0.0));
		const FCortexReplayGuardDecision PoseFailure = FCortexReplayGuardEvaluator::Evaluate(Event,
			Drifted, MakeObservation(ECortexEditorUIObservationState::WrongTarget, Probe.Target,
				FVector2D(0.9, 0.9)), false);
		TestTrue(TEXT("Unavailable still enforces the pose"), PoseFailure.State == ECortexReplayGuardDecisionState::Error);
		TestEqual(TEXT("Unavailable pose failure is the pose code"), PoseFailure.Error.ErrorCode,
			FString(TEXT("REPLAY_POSE_GUARD_FAILED")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Supported coverage requires a valid expected selector and a ready matching observation.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsSupportedReadyTest,
	"Cortex.Replay.Guards.SupportedRequiresReadyObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsSupportedReadyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent Event = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);
	const FCortexEditorPhysicalInputPlayerPose Pose = Event.Guard->ExpectedPose;

	const FCortexReplayGuardDecision Ready = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target, Probe.ExpectedLocal), true);
	TestTrue(TEXT("Ready matching observation accepted"), Ready.State == ECortexReplayGuardDecisionState::Ready);

	// A supported press without an expected selector can never be admitted.
	const FCortexReplayEvent MissingSelector = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, nullptr, FVector2D::ZeroVector);
	const FCortexReplayGuardDecision Missing = FCortexReplayGuardEvaluator::Evaluate(MissingSelector,
		MissingSelector.Guard->ExpectedPose,
		MakeObservation(ECortexEditorUIObservationState::Ready, nullptr, FVector2D::ZeroVector), true);
	TestTrue(TEXT("Missing expected selector errors"), Missing.State == ECortexReplayGuardDecisionState::Error);
	TestEqual(TEXT("Missing selector uses the UI guard code"), Missing.Error.ErrorCode,
		FString(TEXT("REPLAY_UI_GUARD_FAILED")));

	return true;
}

// ---------------------------------------------------------------------------
// The normalized local-position bound is exactly 0.005; a fresh same-identity point beyond it
// fails before the press even though the pointer still hits the same widget.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsLocalPositionBoundTest,
	"Cortex.Replay.Guards.LocalPositionBoundAndSameIdentityDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsLocalPositionBoundTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent Event = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);
	const FCortexEditorPhysicalInputPlayerPose Pose = Event.Guard->ExpectedPose;

	// Exactly the bound passes.
	const FCortexReplayGuardDecision AtBound = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target,
			FVector2D(Probe.ExpectedLocal.X + 0.005, Probe.ExpectedLocal.Y)), true);
	TestTrue(TEXT("Exactly 0.005 is accepted"), AtBound.State == ECortexReplayGuardDecisionState::Ready);

	// Any component beyond the bound is a UI guard failure, even on the same identity.
	const FCortexReplayGuardDecision Beyond = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target,
			FVector2D(Probe.ExpectedLocal.X + 0.006, Probe.ExpectedLocal.Y)), true);
	TestTrue(TEXT("Same-identity changed local point errors"), Beyond.State == ECortexReplayGuardDecisionState::Error);
	TestEqual(TEXT("Changed local point uses the UI guard code"), Beyond.Error.ErrorCode,
		FString(TEXT("REPLAY_UI_GUARD_FAILED")));

	// A second component beyond the bound fails identically.
	const FCortexReplayGuardDecision Moved = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target,
			FVector2D(Probe.ExpectedLocal.X, Probe.ExpectedLocal.Y + 0.02)), true);
	TestTrue(TEXT("Moved slider mismatch errors"), Moved.State == ECortexReplayGuardDecisionState::Error);

	return true;
}

// ---------------------------------------------------------------------------
// Wrong/ambiguous/unavailable observations error; pending observations wait only when allowed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsObservationStatesTest,
	"Cortex.Replay.Guards.ObservationStateDecisions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsObservationStatesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent Event = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);
	const FCortexEditorPhysicalInputPlayerPose Pose = Event.Guard->ExpectedPose;

	// Unequivocal mismatch states error immediately.
	const ECortexEditorUIObservationState MismatchStates[] = {
		ECortexEditorUIObservationState::WrongTarget,
		ECortexEditorUIObservationState::Ambiguous,
		ECortexEditorUIObservationState::Unavailable };
	for (const ECortexEditorUIObservationState State : MismatchStates)
	{
		const FCortexReplayGuardDecision Decision = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
			MakeObservation(State, Probe.Target, Probe.ExpectedLocal), true);
		TestTrue(TEXT("Mismatch state errors"), Decision.State == ECortexReplayGuardDecisionState::Error);
		TestEqual(TEXT("Mismatch uses the UI guard code"), Decision.Error.ErrorCode,
			FString(TEXT("REPLAY_UI_GUARD_FAILED")));
	}

	// Pending states wait when waiting is permitted and carry the pending reason.
	const ECortexEditorUIObservationState PendingStates[] = {
		ECortexEditorUIObservationState::PointerPending,
		ECortexEditorUIObservationState::LayoutPending,
		ECortexEditorUIObservationState::TargetMissing,
		ECortexEditorUIObservationState::TargetDisabled,
		ECortexEditorUIObservationState::TargetNotHitTestable };
	for (const ECortexEditorUIObservationState State : PendingStates)
	{
		const FCortexReplayGuardDecision Wait = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
			MakeObservation(State, Probe.Target, Probe.ExpectedLocal), true);
		TestTrue(TEXT("Pending state waits when allowed"), Wait.State == ECortexReplayGuardDecisionState::Wait);
		TestTrue(TEXT("Pending reason retained"), Wait.WaitReason == State);

		const FCortexReplayGuardDecision Error = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
			MakeObservation(State, Probe.Target, Probe.ExpectedLocal), false);
		TestTrue(TEXT("Pending state errors when waiting is not allowed"), Error.State == ECortexReplayGuardDecisionState::Error);
		TestEqual(TEXT("Refused wait uses the UI guard code"), Error.Error.ErrorCode,
			FString(TEXT("REPLAY_UI_GUARD_FAILED")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// A stale actionable hover is pending, and becomes ready only on fresh matching evidence.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsStaleHoverTest,
	"Cortex.Replay.Guards.StaleHoverPendingThenReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsStaleHoverTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent Event = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);
	const FCortexEditorPhysicalInputPlayerPose Pose = Event.Guard->ExpectedPose;

	// A stale hover (pending) is never a fresh wrong-target verdict.
	const FCortexReplayGuardDecision Stale = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::PointerPending, Probe.Target,
			Probe.ExpectedLocal), true);
	TestTrue(TEXT("Stale hover waits"), Stale.State == ECortexReplayGuardDecisionState::Wait);
	TestTrue(TEXT("Stale hover keeps the pending reason"), Stale.WaitReason == ECortexEditorUIObservationState::PointerPending);

	// The same press on fresh matching evidence is ready.
	const FCortexReplayGuardDecision Fresh = FCortexReplayGuardEvaluator::Evaluate(Event, Pose,
		MakeObservation(ECortexEditorUIObservationState::Ready, Probe.Target, Probe.ExpectedLocal), true);
	TestTrue(TEXT("Fresh matching evidence accepted"), Fresh.State == ECortexReplayGuardDecisionState::Ready);

	return true;
}

// ---------------------------------------------------------------------------
// Partial coverage never extends support: an unavailable press stays ready, but a supported
// press still requires its own fresh matching identity.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsPartialCoverageTest,
	"Cortex.Replay.Guards.PartialCoverageNeverExtendsSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsPartialCoverageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGuardTargetProbe Probe;
	const FCortexReplayEvent SupportedPress = MakeGuardedPress(
		ECortexEditorUICoverage::Supported, Probe.Target, Probe.ExpectedLocal);
	const FCortexReplayEvent UnavailablePress = MakeGuardedPress(
		ECortexEditorUICoverage::Unavailable, nullptr, FVector2D::ZeroVector);
	const FCortexReplaySnapshot Recording =
		MakePartialCoverageRecording({SupportedPress, UnavailablePress});

	// The recording is partial, but the exact unavailable press still skips only UI identity.
	const FCortexReplayGuardDecision UnavailableDecision = FCortexReplayGuardEvaluator::Evaluate(
		Recording.Events[1], Recording.Events[1].Guard->ExpectedPose,
		MakeObservation(ECortexEditorUIObservationState::WrongTarget, Probe.Target,
			FVector2D(0.9, 0.9)), true);
	TestTrue(TEXT("Partial coverage keeps unavailable press ready"), UnavailableDecision.State == ECortexReplayGuardDecisionState::Ready);

	// A partial grant never lets a supported press skip its own identity check.
	const FCortexReplayGuardDecision Enforced = FCortexReplayGuardEvaluator::Evaluate(
		Recording.Events[0], Recording.Events[0].Guard->ExpectedPose,
		MakeObservation(ECortexEditorUIObservationState::WrongTarget, Probe.Target,
			FVector2D(0.9, 0.9)), true);
	TestTrue(TEXT("Partial coverage does not extend support"), Enforced.State == ECortexReplayGuardDecisionState::Error);

	return true;
}

// ---------------------------------------------------------------------------
// A mismatched press is never handed to dispatch (scheduler integration).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGuardsSchedulerSuppressionTest,
	"Cortex.Replay.Guards.MismatchedPressNeverDispatched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGuardsSchedulerSuppressionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayEvent Press;
	Press.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Press.Input.Key = EKeys::E;
	const auto Snapshot = MakeShared<FCortexReplaySnapshot>(
		Fixture.MakeRecording(1, false, {Press}));
	FCortexEditorPhysicalInputPlayerPose Actual = Snapshot->InitialState.Pose;
	Actual.PawnTransform.AddToTranslation(FVector(1.0, 0.0, 0.0));
	FCortexReplayScheduler Scheduler(Snapshot);
	int32 Dispatches = 0;
	const FCortexCommandResult Result = Scheduler.Advance([]() { return 0.0; },
		[&Actual](const FCortexReplayEvent& Event)
		{
			const FCortexEditorPhysicalInputUIObservation UI;
			return FCortexReplayGuardEvaluator::Evaluate(Event, Actual, UI, false);
		},
		[&Dispatches](const FCortexReplayEvent&)
		{
			++Dispatches;
			return FCortexCommandRouter::Success(nullptr);
		});
	TestFalse(TEXT("One centimeter shortfall fails"), Result.bSuccess);
	TestEqual(TEXT("Specific pose failure"), Result.ErrorCode,
		FString(TEXT("REPLAY_POSE_GUARD_FAILED")));
	TestEqual(TEXT("Interaction suppressed"), Dispatches, 0);
	TestEqual(TEXT("No sequence consumed"), Scheduler.GetDispatchedCount(), 0);

	return true;
}
