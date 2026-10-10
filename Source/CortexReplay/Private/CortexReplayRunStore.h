#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"

/**
 * One immutable terminal run result.
 *
 * A record is written exactly once, when its run finalizes, and is never mutated afterwards.
 * The three identity digests are the values admitted with the snapshot; they are copied into the
 * record at admission and never recomputed from the current library files.
 */
struct FCortexReplayRunRecord
{
	FGuid Id;
	int32 RecordingId = 0;
	ECortexReplayOrigin Origin = ECortexReplayOrigin::AI;
	ECortexReplayState State = ECortexReplayState::Preparing;
	FString EditorInstanceId;
	FDateTime StartedAtUtc;
	FDateTime FinalizedAtUtc;
	FString RecordingSnapshotSha256;
	FString InitialStateSha256;
	FString InputsSha256;
	int32 DispatchedEvents = 0;
	int32 TotalEvents = 0;
	double AuthorizedWaitSeconds = 0.0;
	/**
	 * Frame progress of the admitted snapshot. CompletedFrames advances with every committed frame
	 * and must equal FrameCount for a Completed run; a failed run retains its partial verified
	 * progress. FramesSha256 is the admitted frame-stream digest.
	 */
	int32 CompletedFrames = 0;
	int32 FrameCount = 0;
	FString FramesSha256;
	/** True when the record was persisted before frame identity existed; full detail is unsupported. */
	bool bLegacyFormat = false;
	FCortexReplayGuardCoverage GuardCoverage;
	/** Valid only when State == ECortexReplayState::Error. */
	FCortexCommandResult ExecutionError;
};

/**
 * Durable local index of immutable terminal run results.
 *
 * Results live under `<ProjectRoot>/Saved/CortexReplay/Runs/`, one file per parsed run GUID, and
 * are written atomically. The active run itself is owned by the service; this store only carries
 * finalized history and reloads it on startup. Malformed, nonterminal or unreadable files are
 * ignored rather than resumed or fabricated as Completed.
 */
class FCortexReplayRunStore
{
public:
	explicit FCortexReplayRunStore(const FString& ProjectRoot);

	/** Loads retained valid terminal records into the in-memory index. */
	FCortexCommandResult Initialize();

	/** Atomically persists one immutable terminal record, indexed by parsed GUID and origin. */
	FCortexCommandResult SaveTerminal(const FCortexReplayRunRecord& Record);

	/** Retained terminal record for an exact parsed GUID and origin; RUN_NOT_FOUND when absent. */
	FCortexCommandResult Load(const FGuid& Id, ECortexReplayOrigin Origin,
		FCortexReplayRunRecord& Out) const;

	/**
	 * Latest retained terminal record for one recording across human and AI origins, or a
	 * successful result with bFound=false when none exists. Never reads the active run.
	 */
	FCortexCommandResult GetLatestForRecording(int32 RecordingId, bool& bFound,
		FCortexReplayRunRecord& Out) const;

	/**
	 * Retained terminal records for one origin finalized within WindowSeconds, newest first,
	 * capped at MaxCount.
	 */
	FCortexCommandResult ListRecent(ECortexReplayOrigin Origin, int32 MaxCount,
		double WindowSeconds, TArray<FCortexReplayRunRecord>& Out) const;

private:
	FString RunsDirectory() const;
	FString RecordPath(const FGuid& Id) const;
	FString RecordKey(const FGuid& Id, ECortexReplayOrigin Origin) const;

	FString ProjectRoot;
	TMap<FString, FCortexReplayRunRecord> Records;
};
