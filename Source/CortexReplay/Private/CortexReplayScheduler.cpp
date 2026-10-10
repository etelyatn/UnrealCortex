#include "CortexReplayScheduler.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"

#include "Dom/JsonObject.h"

namespace
{
/** Fixed lateness allowance; there is no added tolerance and no configurable override. */
constexpr double SCHEDULER_LATENESS_ALLOWANCE_SECONDS = 0.100;

/** Fixed readiness budgets. */
constexpr double SCHEDULER_MAX_WAIT_PER_PRESS_SECONDS = 1.0;
constexpr double SCHEDULER_MAX_WAIT_TOTAL_SECONDS = 5.0;

FCortexCommandResult SchedulerSuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}
}

FCortexReplayScheduler::FCortexReplayScheduler(TSharedRef<const FCortexReplaySnapshot> InSnapshot)
	: Snapshot(MoveTemp(InSnapshot))
{
}

FCortexCommandResult FCortexReplayScheduler::TimingError(int32 Sequence, double LatenessSeconds) const
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetNumberField(TEXT("sequence"), Sequence);
	Details->SetNumberField(TEXT("lateness_ms"), LatenessSeconds * 1000.0);
	return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayTimingError),
		FString::Printf(TEXT("Replay event %d is overdue by %.3f ms"),
			Sequence, LatenessSeconds * 1000.0),
		Details);
}

FCortexCommandResult FCortexReplayScheduler::PrepareFrame(int32 FrameIndex,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch,
	ECortexReplayFramePreparation& OutPreparation)
{
	OutPreparation = ECortexReplayFramePreparation::Waiting;

	// A rejected clock observation (non-finite, negative or backward) is a timing failure, never
	// a wait.
	auto ValidateObservation = [this](double Value) -> bool
	{
		return FMath::IsFinite(Value) && Value >= 0.0 && Value >= LastObservedElapsed;
	};
	auto WaitTimeoutError = [](int32 Sequence)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayUIWaitTimeout),
			FString::Printf(TEXT("UI readiness wait exceeded its budget at event %d"), Sequence));
	};

	double Elapsed = ReadElapsedSeconds();
	if (!ValidateObservation(Elapsed))
	{
		return TimingError(
			NextEvent < Snapshot->Events.Num() ? Snapshot->Events[NextEvent].Sequence : INDEX_NONE,
			FMath::IsFinite(Elapsed) ? Elapsed - LastObservedElapsed : 0.0);
	}
	LastObservedElapsed = Elapsed;

	if (CompletedFrames >= Snapshot->Frames.Num())
	{
		// Every frame is committed: the run is complete and further polls are no-ops.
		OutPreparation = ECortexReplayFramePreparation::Ready;
		return SchedulerSuccess();
	}

	if (!Snapshot->Frames.IsValidIndex(FrameIndex))
	{
		return FCortexCommandRouter::Error(CortexReplayErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Frame %d is not part of the admitted snapshot"), FrameIndex));
	}
	if (FrameIndex != CompletedFrames)
	{
		return FCortexCommandRouter::Error(CortexReplayErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Frame %d is not the next unprepared frame (%d)"), FrameIndex, CompletedFrames));
	}
	if (PreparingFrame != FrameIndex)
	{
		PreparingFrame = FrameIndex;
		bPreparationComplete = false;
	}

	const FCortexReplayFrame& Frame = Snapshot->Frames[FrameIndex];
	// Widen before clamping: the frame range is validated at load, and the scheduler still never
	// reads past the stored events.
	const int64 FrameEnd64 = static_cast<int64>(Frame.FirstSequence) + static_cast<int64>(Frame.EventCount);
	const int32 FrameEnd = static_cast<int32>(FMath::Min<int64>(FrameEnd64, static_cast<int64>(Snapshot->Events.Num())));

	while (NextEvent < FrameEnd)
	{
		const FCortexReplayEvent& Event = Snapshot->Events[NextEvent];
		const double EffectiveDeadline = Event.TimeSeconds + AuthorizedWaitSeconds;

		// Re-read the clock on every iteration so a slow dispatch cannot hide lateness.
		Elapsed = ReadElapsedSeconds();
		if (!ValidateObservation(Elapsed))
		{
			return TimingError(Event.Sequence, 0.0);
		}
		LastObservedElapsed = Elapsed;

		if (WaitingSequence != INDEX_NONE)
		{
			// Repeated polls re-evaluate only the blocked guard. The evaluation interval itself is
			// never authorized wait time.
			const double BeforeEvaluation = Elapsed;
			const FCortexReplayGuardDecision Decision = EvaluateGuard(Event);
			const double AfterEvaluation = ReadElapsedSeconds();
			if (!ValidateObservation(AfterEvaluation))
			{
				return TimingError(Event.Sequence, 0.0);
			}
			Elapsed = AfterEvaluation;
			LastObservedElapsed = Elapsed;

			// Accidental evaluation latency over the fixed allowance is a timing failure, never a wait.
			const double EvaluationSeconds = AfterEvaluation - BeforeEvaluation;
			if (EvaluationSeconds > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
			{
				return TimingError(Event.Sequence,
					EvaluationSeconds - SCHEDULER_LATENESS_ALLOWANCE_SECONDS);
			}

			if (Decision.State == ECortexReplayGuardDecisionState::Error)
			{
				return Decision.Error;
			}

			// The wait duration excludes the evaluation intervals of every pending poll, not only
			// the current one; the per-press limit is the lesser of the fixed per-press budget and
			// the remaining cumulative run budget.
			const double RemainingRunBudget = SCHEDULER_MAX_WAIT_TOTAL_SECONDS - AuthorizedWaitSeconds;
			const double PerPressLimit = FMath::Min(SCHEDULER_MAX_WAIT_PER_PRESS_SECONDS, RemainingRunBudget);
			const double Measured = FMath::Max(0.0,
				(BeforeEvaluation - WaitStartElapsed) - WaitEvaluationSeconds);

			if (Decision.State == ECortexReplayGuardDecisionState::Wait)
			{
				// A pending poll at a reached limit times out; its own evaluation time is only
				// excluded for later polls because it happens after the observation point.
				WaitEvaluationSeconds += EvaluationSeconds;
				if (Measured >= PerPressLimit)
				{
					return WaitTimeoutError(Event.Sequence);
				}
				WaitReason = Decision.WaitReason;
				return SchedulerSuccess();
			}

			// Ready at exactly the limit is still accepted.
			if (Measured > PerPressLimit)
			{
				return WaitTimeoutError(Event.Sequence);
			}
			AuthorizedWaitSeconds += FMath::Max(0.0, Measured);
			WaitingSequence = INDEX_NONE;
			WaitReason = ECortexEditorUIObservationState::Ready;

			// The shifted deadline uses the freshly committed offset.
			const double ShiftedDeadline = Event.TimeSeconds + AuthorizedWaitSeconds;
			if (Elapsed - ShiftedDeadline > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
			{
				return TimingError(Event.Sequence, Elapsed - ShiftedDeadline);
			}

			const double BeforeDispatch = ReadElapsedSeconds();
			if (!ValidateObservation(BeforeDispatch))
			{
				return TimingError(Event.Sequence, 0.0);
			}
			// Dispatch latency after the ready observation is not authorized wait time either.
			if (BeforeDispatch - AfterEvaluation > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
			{
				return TimingError(Event.Sequence, BeforeDispatch - AfterEvaluation);
			}
			if (BeforeDispatch - ShiftedDeadline > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
			{
				return TimingError(Event.Sequence, BeforeDispatch - ShiftedDeadline);
			}
			LastObservedElapsed = BeforeDispatch;

			const FCortexCommandResult DispatchResult = Dispatch(Event);
			if (!DispatchResult.bSuccess)
			{
				return DispatchResult;
			}
			++DispatchedCount;
			++NextEvent;
			continue;
		}

		// Not waiting: lateness is checked against the committed offset first.
		if (Elapsed - EffectiveDeadline > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
		{
			return TimingError(Event.Sequence, Elapsed - EffectiveDeadline);
		}

		if (Elapsed < EffectiveDeadline)
		{
			// Not yet due: everything later is later still.
			break;
		}

		const double BeforeEvaluation = ReadElapsedSeconds();
		if (!ValidateObservation(BeforeEvaluation))
		{
			return TimingError(Event.Sequence, 0.0);
		}
		Elapsed = BeforeEvaluation;
		LastObservedElapsed = Elapsed;

		const FCortexReplayGuardDecision Decision = EvaluateGuard(Event);
		const double AfterEvaluation = ReadElapsedSeconds();
		if (!ValidateObservation(AfterEvaluation))
		{
			return TimingError(Event.Sequence, 0.0);
		}
		Elapsed = AfterEvaluation;
		LastObservedElapsed = Elapsed;

		// Slow evaluation is a timing failure before the run can enter a wait.
		if (Elapsed - EffectiveDeadline > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
		{
			return TimingError(Event.Sequence, Elapsed - EffectiveDeadline);
		}

		if (Decision.State == ECortexReplayGuardDecisionState::Error)
		{
			return Decision.Error;
		}

		if (Decision.State == ECortexReplayGuardDecisionState::Wait)
		{
			// Admit a wait whenever positive cumulative budget remains; the per-press limit applied
			// while waiting is the lesser of 1 s and the remaining run budget.
			if (SCHEDULER_MAX_WAIT_TOTAL_SECONDS - AuthorizedWaitSeconds <= 0.0)
			{
				return WaitTimeoutError(Event.Sequence);
			}
			WaitingSequence = Event.Sequence;
			WaitReason = Decision.WaitReason;
			WaitStartElapsed = Elapsed;
			WaitEvaluationSeconds = 0.0;
			return SchedulerSuccess();
		}

		const double BeforeDispatch = ReadElapsedSeconds();
		if (!ValidateObservation(BeforeDispatch))
		{
			return TimingError(Event.Sequence, 0.0);
		}
		// Accidental dispatch latency after the ready evaluation is not authorized wait time.
		if (BeforeDispatch - AfterEvaluation > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
		{
			return TimingError(Event.Sequence, BeforeDispatch - AfterEvaluation);
		}
		if (BeforeDispatch - EffectiveDeadline > SCHEDULER_LATENESS_ALLOWANCE_SECONDS)
		{
			return TimingError(Event.Sequence, BeforeDispatch - EffectiveDeadline);
		}
		LastObservedElapsed = BeforeDispatch;

		const FCortexCommandResult DispatchResult = Dispatch(Event);
		if (!DispatchResult.bSuccess)
		{
			return DispatchResult;
		}
		++DispatchedCount;
		++NextEvent;
	}

	// The frame's event range is prepared, but the frame is Ready only once its own input deadline
	// has passed as well: a frame with no events still occupies its recorded span, so completion
	// follows the committed frame deadline rather than the last dispatched event.
	if (NextEvent >= FrameEnd)
	{
		const double FrameElapsed = ReadElapsedSeconds();
		if (!ValidateObservation(FrameElapsed))
		{
			const int32 LastFrameSequence =
				(Frame.EventCount > 0 && Snapshot->Events.IsValidIndex(FrameEnd - 1))
					? Snapshot->Events[FrameEnd - 1].Sequence
					: INDEX_NONE;
			return TimingError(LastFrameSequence, 0.0);
		}
		LastObservedElapsed = FrameElapsed;
		if (FrameElapsed >= Frame.InputDeadlineSeconds + AuthorizedWaitSeconds)
		{
			bPreparationComplete = true;
			OutPreparation = ECortexReplayFramePreparation::Ready;
		}
	}

	return SchedulerSuccess();
}

FCortexCommandResult FCortexReplayScheduler::CommitFrame(int32 FrameIndex)
{
	if (!Snapshot->Frames.IsValidIndex(FrameIndex)
		|| FrameIndex != CompletedFrames
		|| PreparingFrame != FrameIndex
		|| !bPreparationComplete)
	{
		return FCortexCommandRouter::Error(CortexReplayErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Frame %d cannot be committed: it is not the current completed preparation"), FrameIndex));
	}

	++CompletedFrames;
	PreparingFrame = INDEX_NONE;
	bPreparationComplete = false;
	return SchedulerSuccess();
}

int32 FCortexReplayScheduler::GetDispatchedCount() const
{
	return DispatchedCount;
}

int32 FCortexReplayScheduler::GetCompletedFrameCount() const
{
	return CompletedFrames;
}

TOptional<int32> FCortexReplayScheduler::GetCurrentFrame() const
{
	return PreparingFrame == INDEX_NONE ? TOptional<int32>() : TOptional<int32>(PreparingFrame);
}

bool FCortexReplayScheduler::IsComplete() const
{
	// Completion requires every frame committed, not merely every event dispatched.
	return CompletedFrames >= Snapshot->Frames.Num();
}

double FCortexReplayScheduler::GetAuthorizedWaitSeconds() const
{
	return AuthorizedWaitSeconds;
}

double FCortexReplayScheduler::GetCurrentWaitSeconds() const
{
	return WaitingSequence != INDEX_NONE
		? FMath::Max(0.0, LastObservedElapsed - WaitStartElapsed)
		: 0.0;
}

int32 FCortexReplayScheduler::GetWaitingSequence() const
{
	return WaitingSequence;
}

ECortexEditorUIObservationState FCortexReplayScheduler::GetWaitReason() const
{
	return WaitReason;
}
