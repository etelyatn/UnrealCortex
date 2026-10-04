#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexReplayTypes.h"

class FCortexReplayLibrary
{
public:
	explicit FCortexReplayLibrary(const FString& ProjectRoot);
	FCortexCommandResult ReserveId(int32& OutId);
	FCortexCommandResult List(bool bAIOnly, TArray<FCortexReplayMetadata>& Out) const;
	FCortexCommandResult ListPage(bool bAIOnly, int32 AfterId, int32 PageSize,
		TArray<FCortexReplayMetadata>& Out, bool& bHasMore) const;
	FCortexCommandResult Load(int32 Id, bool bAIOnly,
		TSharedPtr<const FCortexReplaySnapshot>& Out) const;
	FCortexCommandResult Publish(const FCortexReplaySnapshot& Recording);
	FCortexCommandResult SaveMetadata(int32 Id, const FString& Name,
		const FString& Description, bool bAIEnabled);
	FCortexCommandResult Delete(int32 Id);

private:
	/** Reads and strictly validates library.json's next_recording_id. Caller holds the authoring lock. */
	FCortexCommandResult ReadValidatedNextId(int64& OutNextId) const;

	/** Durably and atomically commits the next_recording_id counter. Caller holds the authoring lock. */
	FCortexCommandResult CommitCounterAtomically(int64 NextValue);

	FString ProjectRoot;
};
