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

/**
 * The exact human-selected borrowed capture target.
 *
 * Holds weak exact handles only; resolved again before admission. Enumeration grants no ownership
 * and is never exposed over transport.
 */
struct FCortexReplayCaptureTargetChoice
{
	TWeakObjectPtr<UWorld> World;
	int32 LocalPlayerIndex = INDEX_NONE;
	FString MapAssetPath, ViewportLabel;
};

/**
 * Single owner of native capture orchestration, replay scheduling and run finalization.
 *
 * The service owns its library, run store, current physical-input session and its one backend
 * ticker. Window/socket/client lifetime grants no shutdown authority: releasing a caller reference
 * or dropping the transport connection never stops an admitted run; destroying the backend owner
 * does.
 *
 * GetCurrentOperation and GetLastRunForRecording are native human-UI status only and are not
 * registered through transport. IsRecordInUse covers preparation, capture, playback, wait and
 * finalization, not only Recording.
 */
class FCortexReplayService
{
public:
	explicit FCortexReplayService(const FString& ProjectRoot);
	~FCortexReplayService();

	/** Game-Thread enumeration of actual PIE local-player/viewport candidates; never picks the first. */
	FCortexCommandResult EnumerateHumanCaptureTargets(
		TArray<FCortexReplayCaptureTargetChoice>& Out) const;

	/** Human capture on an owned fresh PIE session for the saved editor map. */
	FCortexCommandResult StartCapture(const FString& SavedEditorMapAssetPath);

	/** Human capture on an explicitly borrowed existing target; never owns its shutdown. */
	FCortexCommandResult StartCaptureAtTarget(UWorld& World, int32 LocalPlayerIndex);

	/** Stops capture; bAbnormal keeps the record incomplete/error instead of publishing it. */
	FCortexCommandResult StopCapture(bool bAbnormal);

	/** Admits a replay run for a recording and returns its accepted identity synchronously. */
	FCortexCommandResult StartReplay(int32 Id, ECortexReplayOrigin Origin);

	/** Read-only run status; a terminal execution Error is still a successful query. */
	FCortexCommandResult GetRun(const FGuid& Id, bool bAIOnly) const;

	/** Cancels an admitted run; a finalized run returns its existing terminal state unchanged. */
	FCortexCommandResult CancelReplay(const FGuid& Id, bool bAIOnly);

	FCortexCommandResult ListRecordings(int32 AfterId, int32 PageSize) const;
	FCortexCommandResult ListHumanRecordings(TArray<FCortexReplayMetadata>& Out) const;

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

	FCortexCommandResult GetRecording(int32 Id, bool bAIOnly) const;
	FCortexCommandResult SaveMetadata(int32 Id, const FString& Name,
		const FString& Description, bool bAIEnabled);
	FCortexCommandResult DeleteRecording(int32 Id);

	/** True while preparation, capture, playback, wait or finalization owns the record. */
	bool IsRecordInUse(int32 Id) const;

	/** Backend-owner shutdown: neutralizes owned input, ends owned PIE and drops delegates/ticker. */
	void Shutdown();

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
