#pragma once

#include "CoreMinimal.h"

#include "CortexTypes.h"

/**
 * Fixed user-facing wording for a replay that was interrupted. One source feeds the toast, the window
 * status and the AI-visible execution_error, so every audience reads the same cause.
 */
namespace CortexReplayInterruptionText
{
	/** The fixed title for an interruption cause produced by the editor's physical-input session. */
	FString MakeTitle(const FCortexCommandResult& Cause);

	/**
	 * Rewrites an interruption cause into the stored run error: code REPLAY_INTERRUPTED_BY_INPUT for foreign
	 * input (otherwise the incoming code), the fixed title as message, and the original details plus
	 * reason, stopped_at_frame, frame_count and stopped_at_seconds.
	 */
	FCortexCommandResult ToRunError(const FCortexCommandResult& Cause, int32 CompletedFrames, int32 FrameCount,
		double StoppedAtSeconds);

	/** "at 0:02.7, frame 164 of 2524". */
	FString FormatStopPoint(double StoppedAtSeconds, int32 CompletedFrames, int32 FrameCount);

	/** True when the run error was produced by the user's own physical input. */
	bool IsInputCause(const FCortexCommandResult& RunError);
}
