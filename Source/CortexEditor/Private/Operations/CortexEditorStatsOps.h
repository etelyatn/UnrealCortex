#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * editor.capture_stats: averages the engine stats call tree over N frames and answers once through the
 * deferred callback.
 *
 * Follows the engine's `stat dumpave` (FDumpMultiple in StatsCommand.cpp) without parsing the log. All stats
 * state is touched only on the stats pipe, as the engine's own stat commands do, and only plain copied
 * results cross to the game thread.
 */
class FCortexEditorStatsOps
{
public:
	/**
	 * Params: frames (int 1-600, default 60), filter (array of case-insensitive substrings matched against the
	 * stat's short name and description), max_depth (int 1-64, default 6), min_ms (number >= 0, default 0),
	 * timeout_seconds (number 1-300, default 30).
	 *
	 * Invalid fields -> INVALID_FIELD. A capture already running, STATS compiled out, stats disabled for the
	 * process, module shutting down, or no deferred callback -> INVALID_OPERATION. Otherwise the result is
	 * deferred and the callback completes exactly once: success, INVALID_OPERATION when the stats pipe reports
	 * raw-stats mode (stat dumpspam), TIMEOUT when the last frame does not arrive by the capture deadline, or
	 * EDITOR_NOT_AVAILABLE when the module shuts down mid-capture.
	 */
	static FCortexCommandResult CaptureStats(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);

	/**
	 * Module shutdown, game thread. Closes admission first, then completes a running capture and blocks until its
	 * stats-thread unsubscribe and a stats-pipe barrier have run, so no module code stays queued at unload.
	 */
	static void Shutdown();

	/** True while a capture is running (its deferred callback has not completed yet). Game thread. */
	static bool IsCaptureActive();
};
