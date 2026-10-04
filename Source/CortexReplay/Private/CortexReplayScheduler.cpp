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

FCortexCommandResult FCortexReplayScheduler::Advance(
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch)
{
	// A rejected clock observation (non-finite, negative or backward) is a timing failure, never
	// a wait.
	auto ValidateObservation = [this](double Value) -> bool
	{
		return FMath::IsFinite(Value) && Value >= 0.0 && Value >= LastObservedElapsed;
	};

	double Elapsed = ReadElapsedSeconds();
	if (!ValidateObservation(Elapsed))
	{
		return TimingError(
			NextEvent < Snapshot->Events.Num() ? Snapshot->Events[NextEvent].Sequence : INDEX_NONE,
			FMath::IsFinite(Elapsed) ? Elapsed - LastObservedElapsed : 0.0);
	}
	LastObservedElapsed = Elapsed;

	if (bComplete)
	{
		return SchedulerSuccess();
	}

	while (NextEvent < Snapshot->Events.Num())
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
			// Repeated polls re-evaluate only the blocked guard.
			const FCortexReplayGuardDecision Decision = EvaluateGuard(Event);
			const double AfterEvaluation = ReadElapsedSeconds();
			if (!ValidateObservation(AfterEvaluation))
			{
				return TimingError(Event.Sequence, 0.0);
			}
			Elapsed = AfterEvaluation;
			LastObservedElapsed = Elapsed;

			if (Decision.State == ECortexReplayGuardDecisionState::Error)
			{
				return Decision.Error;
			}

			if (Decision.State == ECortexReplayGuardDecisionState::Wait)
			{
				const double Measured = Elapsed - WaitStartElapsed;
				if (Measured > SCHEDULER_MAX_WAIT_PER_PRESS_SECONDS
					|| AuthorizedWaitSeconds + Measured > SCHEDULER_MAX_WAIT_TOTAL_SECONDS)
				{
					return FCortexCommandRouter::Error(
						FString(CortexReplayErrorCodes::ReplayUIWaitTimeout),
						FString::Printf(TEXT("UI readiness wait exceeded its budget at event %d"),
							Event.Sequence));
				}
				WaitReason = Decision.WaitReason;
				return SchedulerSuccess();
			}

			// Ready: enforce both budgets before committing the measured duration.
			const double Measured = Elapsed - WaitStartElapsed;
			if (Measured > SCHEDULER_MAX_WAIT_PER_PRESS_SECONDS
				|| AuthorizedWaitSeconds + Measured > SCHEDULER_MAX_WAIT_TOTAL_SECONDS)
			{
				return FCortexCommandRouter::Error(
					FString(CortexReplayErrorCodes::ReplayUIWaitTimeout),
					FString::Printf(TEXT("UI readiness wait exceeded its budget at event %d"),
						Event.Sequence));
			}
			AuthorizedWaitSeconds += Measured;
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
			// A wait starts only when the full per-press budget is still available.
			if (SCHEDULER_MAX_WAIT_PER_PRESS_SECONDS > SCHEDULER_MAX_WAIT_TOTAL_SECONDS - AuthorizedWaitSeconds
				|| SCHEDULER_MAX_WAIT_TOTAL_SECONDS - AuthorizedWaitSeconds <= 0.0)
			{
				return FCortexCommandRouter::Error(
					FString(CortexReplayErrorCodes::ReplayUIWaitTimeout),
					FString::Printf(TEXT("UI readiness budget exhausted at event %d"),
						Event.Sequence));
			}
			WaitingSequence = Event.Sequence;
			WaitReason = Decision.WaitReason;
			WaitStartElapsed = Elapsed;
			return SchedulerSuccess();
		}

		const double BeforeDispatch = ReadElapsedSeconds();
		if (!ValidateObservation(BeforeDispatch))
		{
			return TimingError(Event.Sequence, 0.0);
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

	// Trailing completion: every event dispatched and the recorded trailing duration elapsed.
	if (NextEvent >= Snapshot->Events.Num())
	{
		const double FinalElapsed = ReadElapsedSeconds();
		if (!ValidateObservation(FinalElapsed))
		{
			return TimingError(INDEX_NONE, 0.0);
		}
		LastObservedElapsed = FinalElapsed;
		if (FinalElapsed >= Snapshot->Metadata.DurationSeconds + AuthorizedWaitSeconds)
		{
			bComplete = true;
		}
	}

	return SchedulerSuccess();
}

int32 FCortexReplayScheduler::GetDispatchedCount() const
{
	return DispatchedCount;
}

bool FCortexReplayScheduler::IsComplete() const
{
	return bComplete;
}

double FCortexReplayScheduler::GetAuthorizedWaitSeconds() const
{
	return AuthorizedWaitSeconds;
}

int32 FCortexReplayScheduler::GetWaitingSequence() const
{
	return WaitingSequence;
}

ECortexEditorUIObservationState FCortexReplayScheduler::GetWaitReason() const
{
	return WaitReason;
}
