#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"

/**
 * Pure, bounded guard decisions for one replayed event.
 *
 * The evaluator performs no engine work: the caller supplies the already-sampled pose and UI
 * observation, and the evaluation compares them against the event's recorded guard. It is the
 * single place where the pose tolerance (delegated to the shared session comparator) and the
 * 0.005 normalized local-position bound are turned into Ready/Wait/Error decisions.
 */
enum class ECortexReplayGuardDecisionState : uint8 { Ready, Wait, Error };

struct FCortexReplayGuardDecision
{
	ECortexReplayGuardDecisionState State = ECortexReplayGuardDecisionState::Ready;
	/** Populated only for State == Wait: the pending reason that must be re-polled. */
	ECortexEditorUIObservationState WaitReason = ECortexEditorUIObservationState::PointerPending;
	/** Populated only for State == Error. */
	FCortexCommandResult Error;
};

class FCortexReplayGuardEvaluator
{
public:
	/**
	 * Compares the recorded expected pose with the observed one through the shared session
	 * comparator (location 0.5cm, shortest quaternion angle 0.5 degrees, scale component 0.001)
	 * and remaps only the shared pose-mismatch code to REPLAY_POSE_GUARD_FAILED, preserving the
	 * bounded deviation details. Other shared failures are passed through unchanged.
	 */
	static FCortexCommandResult CheckPose(
		const FCortexEditorPhysicalInputPlayerPose& Expected,
		const FCortexEditorPhysicalInputPlayerPose& Actual);

	/**
	 * Pure decision for one event.
	 *
	 * Unguarded edges are Ready. A guarded press requires a finite ActualPose and compares the
	 * pose before UI coverage. NotApplicable/Unavailable skip only the UI identity check and keep
	 * the pose guard. Supported requires a valid expected selector and a ready observation already
	 * matched by the shared resolver; a normalized local error above 0.005 is
	 * REPLAY_UI_GUARD_FAILED. WrongTarget/Ambiguous/Unavailable error immediately;
	 * PointerPending/LayoutPending/TargetMissing/TargetDisabled/TargetNotHitTestable return Wait
	 * only when bCanWait is true, otherwise Error.
	 */
	static FCortexReplayGuardDecision Evaluate(const FCortexReplayEvent& Event,
		const FCortexEditorPhysicalInputPlayerPose& ActualPose,
		const FCortexEditorPhysicalInputUIObservation& UI, bool bCanWait);
};
