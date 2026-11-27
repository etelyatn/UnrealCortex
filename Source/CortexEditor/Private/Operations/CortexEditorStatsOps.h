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
	 * process, or no deferred callback -> INVALID_OPERATION. Otherwise the result is deferred and the callback
	 * completes exactly once: success, TIMEOUT at the capture deadline, or EDITOR_NOT_AVAILABLE when the module
	 * shuts down mid-capture.
	 */
	static FCortexCommandResult CaptureStats(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);

	/** Module shutdown: completes a running capture, unsubscribes from the stats thread and drains the stats pipe. Game thread. */
	static void Shutdown();

	/** True while a capture is running (its deferred callback has not completed yet). Game thread. */
	static bool IsCaptureActive();
};
