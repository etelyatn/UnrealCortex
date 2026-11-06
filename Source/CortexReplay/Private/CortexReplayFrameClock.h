#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "Templates/Function.h"

/**
 * The engine clock of one replay run.
 *
 * A run consumes recorded frame k on exactly one engine frame that runs on the recorded application
 * delta. The clock owns everything that makes that possible and nothing else:
 *  - admission: refuses an editor whose application time is already driven by another owner;
 *  - the Slate throttle: held disabled from Begin until Release, because a throttled editor skips the
 *    PIE world tick and so would change the number of ticks an engine frame consumes;
 *  - fixed-step ownership: the human's fixed delta is sampled whenever ownership is taken and restored
 *    whenever it is handed back, but only while the step is still the one this clock wrote;
 *  - pacing: application time never runs ahead of wall time, so the frame waits (idle-attributed)
 *    until wall time reaches its application time; a host that is already later falls behind;
 *  - per-frame verification: the owned world ticked exactly as often as the recording did, on the
 *    recorded real delta and pause state.
 *
 * Wall time, application time and the wait are injectable so pacing, lag and ownership are testable
 * without a rendered owned PIE. Game thread only.
 */
class FCortexReplayFrameClock
{
public:
	struct FHost
	{
		/** Monotonic wall-clock seconds. */
		TFunction<double()> WallSeconds;
		/** The engine's current application time, as FApp::GetCurrentTime(). */
		TFunction<double()> AppCurrentSeconds;
		/** Blocks (or yields) toward the given positive remaining wall seconds. May return early. */
		TFunction<void(double RemainingSeconds)> WaitFor;
	};

	/** The engine's own wall clock, application time and sleep/spin wait. */
	static FHost MakeEngineHost();

	FCortexReplayFrameClock();
	explicit FCortexReplayFrameClock(FHost InHost);
	~FCortexReplayFrameClock();

	FCortexReplayFrameClock(const FCortexReplayFrameClock&) = delete;
	FCortexReplayFrameClock& operator=(const FCortexReplayFrameClock&) = delete;

	/**
	 * Refuses (INCOMPATIBLE_PREREQUISITES) while fixed stepping, benchmarking, a fixed engine frame
	 * rate, a custom time step or `t.OverrideFPS` already owns application timing: taking it over would
	 * replace the human's timing, and handing it back could not tell the two owners apart.
	 */
	static FCortexCommandResult CheckAdmission();

	/** Admits the run (CheckAdmission) and disables the Slate throttle. Pair with Release. */
	FCortexCommandResult Begin();

	/**
	 * Hands the whole clock back: fixed stepping and the Slate throttle. Idempotent, and safe on every
	 * terminal path.
	 */
	void Release();

	/** Hands the application clock back to the editor for a frame that runs on its own clock. */
	void ReleaseFixedStep();

	/**
	 * Fails with REPLAY_TIMEBASE_LOST when another owner took the application clock away: it switched
	 * fixed stepping, replaced the step or installed a custom time step. The other owner's state is left
	 * as it set it, except that a human delta this clock still owns is restored when only fixed stepping
	 * was switched off. Ownership is dropped on failure.
	 */
	FCortexCommandResult VerifyOwnership();

	/**
	 * Paces and starts one engine frame on the recorded application delta: waits until wall time reaches
	 * the frame's application time, then fixes the step at DeltaSeconds. Records the lag it started with.
	 */
	FCortexCommandResult BeginPacedFrame(double DeltaSeconds);

	/** Forgets the owned world ticks observed so far; call when a frame is armed. */
	void BeginFrameObservation();

	/** Records one owned world tick of the armed engine frame. */
	void ObserveWorldTick(float RealDeltaSeconds, bool bPaused);

	/**
	 * Verifies the armed engine frame against the recording: the owned world ticked exactly once when the
	 * recorded frame did and not at all when it did not, on the recorded real delta and pause state.
	 * Otherwise REPLAY_TIMEBASE_LOST. A recorded frame without a world tick can therefore only be
	 * replayed while the replay world also does not tick; a ticking replay world fails instead of
	 * silently simulating time the recording never did.
	 */
	FCortexCommandResult VerifyFrame(const FCortexReplayFrame& Frame) const;

	bool OwnsFixedStep() const { return bOwnsFixedStep; }

	/** How far wall time was past the last paced frame's application time when it started; >= 0. */
	double GetBehindSeconds() const { return BehindSeconds; }

private:
	FHost Host;
	bool bThrottleDisabled = false;
	/** True while fixed application stepping belongs to this clock. */
	bool bOwnsFixedStep = false;
	/** The human's fixed delta, sampled whenever ownership is taken. */
	double HumanFixedDeltaSeconds = 0.0;
	/** The fixed delta this clock last wrote; any other value means another owner replaced the step. */
	double AppliedFixedDeltaSeconds = 0.0;
	double BehindSeconds = 0.0;
	int32 ObservedTicks = 0;
	float ObservedRealDeltaSeconds = 0.0f;
	bool bObservedPaused = false;
};
