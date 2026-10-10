#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "Templates/Function.h"

/**
 * Monotonic event scheduler for one admitted replay snapshot.
 *
 * The scheduler owns no engine state. It is driven by the caller's monotonic elapsed-time
 * provider and dispatches stored events in their recorded order. Game time and the recorded
 * capture diagnostics never schedule playback; only the monotonic clock does.
 *
 * Lateness rule: an event is overdue by more than the fixed 0.100s constant when
 * `Elapsed - (Event.TimeSeconds + AuthorizedWaitSeconds) > 0.100`; there is no added tolerance or
 * configurable allowance. Elapsed time is re-read before guard evaluation, after evaluation,
 * immediately before every dispatch and at trailing completion.
 *
 * One blocked UI wait is allowed at a time. A permitted pending guard decision enters the wait
 * state without consuming the event; a later Advance re-polls only that guard. On Ready the
 * measured wait must be <= 1s and the run total <= 5s before the measured duration is committed
 * to the authorized offset, after which every later event and the trailing duration use it.
 */
class FCortexReplayScheduler
{
public:
	explicit FCortexReplayScheduler(TSharedRef<const FCortexReplaySnapshot> Snapshot);

	/**
	 * Prepares exactly the requested frame's `[FirstSequence, EventCount)` event range, in order,
	 * and never drains a later frame. Returns success while the frame is still being prepared
	 * (OutPreparation == Waiting, e.g. blocked on a permitted UI wait) and a REPLAY_TIMING_ERROR /
	 * guard error when the run must stop. A frame with no events is immediately Ready.
	 *
	 * The caller's callables are invoked synchronously on the calling (Game) thread.
	 */
	FCortexCommandResult PrepareFrame(int32 FrameIndex,
		TFunctionRef<double()> ReadElapsedSeconds,
		TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
		TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch,
		ECortexReplayFramePreparation& OutPreparation);

	/**
	 * Commits a frame whose preparation completed. Refused when the frame is not the current
	 * unprepared frame, when preparation is still outstanding, or when it was already committed.
	 */
	FCortexCommandResult CommitFrame(int32 FrameIndex);

	/** Number of events handed to Dispatch so far. */
	int32 GetDispatchedCount() const;

	/** Number of frames committed so far. */
	int32 GetCompletedFrameCount() const;

	/** Frame currently being prepared, or INDEX_NONE when no frame is in preparation. */
	TOptional<int32> GetCurrentFrame() const;

	/** True once every frame of the snapshot has been committed. */
	bool IsComplete() const;

	/** Measured authorized wait offset committed so far (total, capped at 5 seconds). */
	double GetAuthorizedWaitSeconds() const;

	/**
	 * Duration of the in-progress blocked-event wait, measured from its observation start; 0 when
	 * no wait is active. This is reported separately from the committed authorized offset so a
	 * slow evaluation is never counted as authorized wait time.
	 */
	double GetCurrentWaitSeconds() const;

	/** Sequence currently blocked on a UI wait, or INDEX_NONE when not waiting. */
	int32 GetWaitingSequence() const;

	/** Pending reason for GetWaitingSequence, valid only while a wait is active. */
	ECortexEditorUIObservationState GetWaitReason() const;

private:
	/** Builds REPLAY_TIMING_ERROR with the event sequence and lateness_ms details. */
	FCortexCommandResult TimingError(int32 Sequence, double LatenessSeconds) const;

	/** Immutable admitted snapshot; shared and never copied per tick. */
	TSharedRef<const FCortexReplaySnapshot> Snapshot;

	/** Index of the next stored event to consider. */
	int32 NextEvent = 0;
	int32 DispatchedCount = 0;

	/** Frame currently in preparation, or INDEX_NONE when none is outstanding. */
	int32 PreparingFrame = INDEX_NONE;
	/** True once the outstanding preparation has dispatched its whole event range. */
	bool bPreparationComplete = false;
	/** Frames committed so far. */
	int32 CompletedFrames = 0;

	/** Active UI wait state. */
	int32 WaitingSequence = INDEX_NONE;
	ECortexEditorUIObservationState WaitReason = ECortexEditorUIObservationState::Ready;
	double WaitStartElapsed = 0.0;
	/**
	 * Guard-evaluation time consumed by earlier pending polls of the current wait. It is never
	 * authorized wait time, so it is subtracted before the measured wait is committed.
	 */
	double WaitEvaluationSeconds = 0.0;
	double AuthorizedWaitSeconds = 0.0;
	double LastObservedElapsed = 0.0;
};
