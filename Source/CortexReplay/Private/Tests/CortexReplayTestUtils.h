#pragma once

#include "CoreMinimal.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTypes.h"

/** Test-only: an event plus the capture time the fixture uses to place it on the 60 Hz cadence. */
struct FCortexReplayTimedEvent
{
	FCortexReplayEvent Event;
	double TimeSeconds = 0.0;
};

/**
 * Test-only fixture for CortexReplay library automation tests.
 *
 * Owns a unique Saved directory per instance so tests never touch the real shared
 * library. MakeRecording supplies a valid default initial pose/class/schema and
 * prerequisites, fills missing guards for press rows with that pose/not_applicable,
 * computes coverage and a duration covering the final event time. It must not repair
 * explicitly malformed data supplied by negative-case tests.
 */
class FCortexReplayTestFixture
{
public:
	FCortexReplayTestFixture();
	~FCortexReplayTestFixture();

	const FString& GetProjectRoot() const;
	FCortexReplaySnapshot MakeRecording(int32 Id, bool bAIEnabled,
		TConstArrayView<FCortexReplayTimedEvent> Events);
	FString GetInputsPath(int32 Id) const;
	FString GetInitialStatePath(int32 Id) const;

	/**
	 * Rebuilds a recording's frames as a realistic 60 Hz format-3 stream: frames 0..N-1, each with a
	 * world tick, covering the larger of the recorded duration and the last event. Each event belongs
	 * to the first frame whose input boundary is at or after its time, and the duration becomes the
	 * sum of the frame deltas. Call after every metadata/event mutation and before constructing the
	 * snapshot.
	 */
	static void ApplyCadenceFrames(FCortexReplaySnapshot& Recording, TConstArrayView<double> EventTimes);

	/**
	 * Rebuilds a recording's frames as a capture lays them out: frame i has application delta
	 * FrameDeltas[i] and a matching world tick (not paused). Event j belongs to frame EventFrames[j]
	 * (non-decreasing). The recorded duration becomes the sum of the deltas. Event times are unchanged.
	 */
	static void ApplyFrameStream(FCortexReplaySnapshot& Recording,
		TConstArrayView<double> FrameDeltas, TConstArrayView<int32> EventFrames);

private:
	FString ProjectRoot;
};

/**
 * Drives the frame API from a synthetic clock by exactly one step: prepares the next uncommitted
 * frame and commits it only when it is ready. Returns the first failing result; Success when the frame
 * is still waiting or the run is complete. Callers loop per frame themselves.
 */
FCortexCommandResult PrepareAndCommit(FCortexReplayScheduler& Scheduler,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch);
