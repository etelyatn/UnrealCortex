#include "CortexReplayScheduler.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"

#include "Dom/JsonObject.h"

namespace
{
FCortexCommandResult SchedulerSuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}
}

FCortexReplayScheduler::FCortexReplayScheduler(TSharedRef<const FCortexReplaySnapshot> InSnapshot)
	: Snapshot(MoveTemp(InSnapshot))
{
}

FCortexCommandResult FCortexReplayScheduler::ClockError(int32 Sequence) const
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetNumberField(TEXT("sequence"), Sequence);
	return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayTimingError),
		FString::Printf(TEXT("The replay clock produced an invalid observation at event %d"), Sequence),
		Details);
}

FCortexCommandResult FCortexReplayScheduler::PrepareFrame(int32 FrameIndex,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch,
	ECortexReplayFramePreparation& OutPreparation)
{
	OutPreparation = ECortexReplayFramePreparation::Waiting;

	// The clock only measures readiness waits and is read only when a wait is entered or polled. A
	// rejected observation (non-finite, negative or backward) is a timing failure, never a wait.
	auto ReadClock = [this, &ReadElapsedSeconds](int32 Sequence, double& OutElapsed) -> FCortexCommandResult
	{
		OutElapsed = ReadElapsedSeconds();
		if (!FMath::IsFinite(OutElapsed) || OutElapsed < 0.0 || OutElapsed < LastObservedElapsed)
		{
			return ClockError(Sequence);
		}
		LastObservedElapsed = OutElapsed;
		return SchedulerSuccess();
	};
	auto WaitTimeoutError = [](int32 Sequence)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayUIWaitTimeout),
			FString::Printf(TEXT("UI readiness wait exceeded its budget at event %d"), Sequence));
	};

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

	// Frame-locked: the whole range belongs to the engine frame preparing it, so no event waits for a
	// wall-clock timestamp and no amount of wall-clock delay is a failure.
	while (NextEvent < FrameEnd)
	{
		const FCortexReplayEvent& Event = Snapshot->Events[NextEvent];
		const bool bPolling = WaitingSequence != INDEX_NONE;

		double BeforeEvaluation = 0.0;
		if (bPolling)
		{
			const FCortexCommandResult Observed = ReadClock(Event.Sequence, BeforeEvaluation);
			if (!Observed.bSuccess)
			{
				return Observed;
			}
		}
		const FCortexReplayGuardDecision Decision = EvaluateGuard(Event);
		double AfterEvaluation = 0.0;
		if (bPolling)
		{
			const FCortexCommandResult Observed = ReadClock(Event.Sequence, AfterEvaluation);
			if (!Observed.bSuccess)
			{
				return Observed;
			}
		}

		if (Decision.State == ECortexReplayGuardDecisionState::Error)
		{
			return Decision.Error;
		}

		if (bPolling)
		{
			// Repeated polls re-evaluate only the blocked guard. The wait duration excludes the
			// evaluation intervals of every pending poll; the per-press limit is the lesser of the fixed
			// per-press budget and the remaining cumulative run budget.
			const double EvaluationSeconds = AfterEvaluation - BeforeEvaluation;
			const double RemainingRunBudget = CortexReplayWaitBudget::MaxAuthorizedWaitSeconds - AuthorizedWaitSeconds;
			const double PerPressLimit = FMath::Min(CortexReplayWaitBudget::MaxWaitPerPressSeconds, RemainingRunBudget);
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
			AuthorizedWaitSeconds += Measured;
			WaitingSequence = INDEX_NONE;
			WaitReason = ECortexEditorUIObservationState::Ready;
		}
		else if (Decision.State == ECortexReplayGuardDecisionState::Wait)
		{
			// Admit a wait whenever positive cumulative budget remains; the per-press limit applied
			// while waiting is the lesser of 1 s and the remaining run budget.
			if (CortexReplayWaitBudget::MaxAuthorizedWaitSeconds - AuthorizedWaitSeconds <= 0.0)
			{
				return WaitTimeoutError(Event.Sequence);
			}
			double WaitStart = 0.0;
			const FCortexCommandResult Observed = ReadClock(Event.Sequence, WaitStart);
			if (!Observed.bSuccess)
			{
				return Observed;
			}
			WaitingSequence = Event.Sequence;
			WaitReason = Decision.WaitReason;
			WaitStartElapsed = WaitStart;
			WaitEvaluationSeconds = 0.0;
			return SchedulerSuccess();
		}

		const FCortexCommandResult DispatchResult = Dispatch(Event);
		if (!DispatchResult.bSuccess)
		{
			return DispatchResult;
		}
		++DispatchedCount;
		++NextEvent;
	}

	bPreparationComplete = true;
	OutPreparation = ECortexReplayFramePreparation::Ready;
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
