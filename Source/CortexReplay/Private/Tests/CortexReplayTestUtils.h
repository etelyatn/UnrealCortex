#pragma once

#include "CoreMinimal.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTypes.h"

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
	FCortexReplaySnapshot MakeRecording(int32 Id, bool bAIEnabled, const TArray<FCortexReplayEvent>& Events);
	FString GetInputsPath(int32 Id) const;
	FString GetInitialStatePath(int32 Id) const;

	/**
	 * Rebuilds a recording's frames so they express the recorded input cadence instead of the
	 * degenerate format-2 shape: an empty warmup frame at 0, one frame per distinct event time, and
	 * a trailing empty frame ending at Metadata.DurationSeconds. Call after every metadata/event
	 * mutation and before constructing the snapshot.
	 */
	static void ApplyCadenceFrames(FCortexReplaySnapshot& Recording);

private:
	FString ProjectRoot;
};

/**
 * Drives the frame API from a synthetic clock: repeatedly prepares the next uncommitted frame and
 * commits it once its whole event range is prepared and its deadline has passed, stopping at the
 * first frame that is not yet due. Returns the first failing result.
 */
FCortexCommandResult AdvanceFrame(FCortexReplayScheduler& Scheduler,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch);
