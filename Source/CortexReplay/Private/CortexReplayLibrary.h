#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexReplayTypes.h"

class FCortexReplayLibrary
{
public:
	explicit FCortexReplayLibrary(const FString& ProjectRoot);
	FCortexCommandResult ReserveId(int32& OutId);
	/** Full-validation listing: recordings that fail to load (or AI eligibility) are omitted. */
	FCortexCommandResult List(bool bAIOnly, TArray<FCortexReplayMetadata>& Out) const;
	/**
	 * Metadata-only listing of every numeric recording directory. No payload is parsed or hashed;
	 * a directory whose metadata cannot be used is still reported, with LoadError explaining why.
	 */
	FCortexCommandResult ListEntries(TArray<FCortexReplayLibraryEntry>& Out) const;
	FCortexCommandResult ListPage(bool bAIOnly, int32 AfterId, int32 PageSize,
		TArray<FCortexReplayMetadata>& Out, bool& bHasMore) const;
	FCortexCommandResult Load(int32 Id, bool bAIOnly,
		TSharedPtr<const FCortexReplaySnapshot>& Out) const;
	FCortexCommandResult Publish(const FCortexReplaySnapshot& Recording);
	FCortexCommandResult SaveMetadata(int32 Id, const FString& Name,
		const FString& Description, bool bAIEnabled);
	/** Removes the recording directory under the authoring lock, whether or not it still loads. */
	FCortexCommandResult Delete(int32 Id);

	/**
	 * Monotonic in-memory revision of this library instance, bumped by every successful
	 * Publish/Delete/SaveMetadata. Lets an owner detect recording-set/metadata changes without
	 * re-reading the filesystem.
	 */
	int64 GetRevision() const;

private:
	/** Reads and strictly validates library.json's next_recording_id. Caller holds the authoring lock. */
	FCortexCommandResult ReadValidatedNextId(int64& OutNextId) const;

	/** Durably and atomically commits the next_recording_id counter. Caller holds the authoring lock. */
	FCortexCommandResult CommitCounterAtomically(int64 NextValue);

	FString ProjectRoot;

	/** Bumped on every successful mutation; see GetRevision(). */
	int64 Revision = 0;
};
