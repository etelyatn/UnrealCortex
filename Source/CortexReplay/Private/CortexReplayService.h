#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FJsonObject;
class FCortexReplayScheduler;
struct FCortexReplayRunRecord;
class UWorld;
class SWidget;

/**
 * Single owner of native capture orchestration, replay scheduling and run finalization.
 *
 * The service owns its library, run store, current physical-input session and its one backend
 * ticker. Window/socket/client lifetime grants no shutdown authority: releasing a caller reference
 * or dropping the transport connection never stops an admitted run; destroying the backend owner
 * does.
 *
 * GetCurrentOperation, GetLastCaptureResult and GetLastRunForRecording are native human-UI status
 * only and are not registered through transport. IsRecordInUse covers preparation, capture,
 * playback, wait and finalization, not only Recording.
 */
class FCortexReplayService
{
public:
	explicit FCortexReplayService(const FString& ProjectRoot);
	~FCortexReplayService();

	/** Human capture on an owned fresh PIE session for the saved editor map. */
	FCortexCommandResult StartCapture(const FString& SavedEditorMapAssetPath);

	/** Stops capture; bAbnormal keeps the record incomplete/error instead of publishing it. */
	FCortexCommandResult StopCapture(bool bAbnormal);

	/** Admits a replay run for a recording and returns its accepted identity synchronously. */
	FCortexCommandResult StartReplay(int32 Id, ECortexReplayOrigin Origin);

	/** Read-only run status; a terminal execution Error is still a successful query. */
	FCortexCommandResult GetRun(const FGuid& Id, bool bAIOnly) const;

	/**
	 * Cancels an admitted run. The response is the run's state right after the request: Finalizing
	 * while owned teardown is still in flight, or the terminal state once it completed. A finalized
	 * run returns its existing terminal state unchanged.
	 */
	FCortexCommandResult CancelReplay(const FGuid& Id, bool bAIOnly);

	FCortexCommandResult ListRecordings(int32 AfterId, int32 PageSize) const;

	/**
	 * Human-library listing from metadata only: one entry per recording directory, with the load
	 * error of every directory that cannot be loaded. Payloads are not read or hashed.
	 */
	FCortexCommandResult ListHumanRecordingEntries(TArray<FCortexReplayLibraryEntry>& Out) const;

	/**
	 * Monotonic revision of the owned library, bumped by successful publish/delete/metadata saves.
	 * The human window uses it to rebuild rows when a recording appears or changes without
	 * re-reading the filesystem every frame.
	 */
	int64 GetLibraryRevision() const;

	/** Nullable human-only status of the current capture/replay operation. */
	FCortexCommandResult GetCurrentOperation() const;

	/** Latest retained terminal playback summary for a recording, or a successful null summary. */
	FCortexCommandResult GetLastRunForRecording(int32 Id) const;

	/**
	 * Latest retained terminal capture summary (published or failed), or a successful null summary
	 * when no capture has completed. Human-UI status only, never registered through transport, and
	 * never reports an active capture: an immutable publication failure releases the operation and
	 * is retained here instead of looping. Cleared when the next capture is admitted.
	 */
	FCortexCommandResult GetLastCaptureResult() const;

	FCortexCommandResult GetRecording(int32 Id, bool bAIOnly) const;
	FCortexCommandResult SaveMetadata(int32 Id, const FString& Name,
		const FString& Description, bool bAIEnabled);
	FCortexCommandResult DeleteRecording(int32 Id);

	/** True while preparation, capture, playback, wait or finalization owns the record. */
	bool IsRecordInUse(int32 Id) const;

	/** Backend-owner shutdown: neutralizes owned input, ends owned PIE and drops delegates/ticker. */
	void Shutdown();

	/**
	 * Module-unload barrier: Shutdown, then completes any finalization now instead of on a later
	 * engine frame. On return no ticker, frame delegate or callback into this module remains; an
	 * owned PIE still ending is kept alive by CortexEditor until its teardown completes.
	 */
	void ShutdownForModuleUnload();

	/** True when no ticker, frame delegate, session or capture observer of this service is installed. */
	bool IsQuiescent() const;

	/**
	 * Test hook: sets the hands-off countdown length for this service's later runs. Tests that
	 * measure replay behavior pass zero; production keeps the default.
	 */
	void SetCountdownSecondsForTesting(double Seconds);

	/** Test hook: the active run's game-view overlay, or null when none is shown. */
	TSharedPtr<SWidget> GetRunOverlayForTesting() const;

private:
	/**
	 * Enters Finalizing exactly once, rejects new admission, freezes dispatch and detaches capture,
	 * runs the original-target cleanup, ends only a matching owned session and retains ownership
	 * until IsOwnedPIEEnded() confirms teardown before persisting the terminal result.
	 */
	FCortexCommandResult Finalize(ECortexReplayState TerminalState,
		const FCortexCommandResult& ExecutionResult);

	/** Serializes one run record into the wire shape shared by get_run and the recovery summaries. */
	static TSharedRef<FJsonObject> BuildRunData(const FCortexReplayRunRecord& Record,
		bool bLive, const FCortexReplayScheduler* Scheduler);

	struct FImpl;
	/**
	 * Shared so the finalization ticker can keep the backend (and its teardown observation) alive
	 * after this service object is destroyed; the ticker releases it once teardown is complete.
	 */
	TSharedPtr<FImpl> Impl;
};
