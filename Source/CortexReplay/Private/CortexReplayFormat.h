#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"

class FJsonObject;

/**
 * The CortexReplay recording format (metadata.json / initial_state.json / inputs.jsonl /
 * frames.jsonl): serialization, strict parsing and validation. It owns every invariant a recording
 * must satisfy and knows nothing about where recordings are stored.
 */
namespace CortexReplayFormat
{
/**
 * Recording-side format version. Format 3 records the engine frames that consumed input after the
 * capture epoch. library.json has its own, independent schema version in the library facade.
 */
constexpr int32 RecordingFormatVersion = 3;

// ---------------------------------------------------------------------------
// JSON primitives (also used by the library.json manifest)
// ---------------------------------------------------------------------------

FString SerializeCanonicalJson(const TSharedRef<FJsonObject>& Object);
bool DeserializeJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject);
bool TryReadJsonInt32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 MinValue, int32 MaxValue,
	int32& OutValue, FString& OutError);
bool TryReadJsonInt64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64 MinValue, int64 MaxValue,
	int64& OutValue, FString& OutError);

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

FString SerializeInitialState(const FCortexReplayInitialState& InitialState);
/** One canonical inputs.jsonl row, without its terminator. */
FString SerializeEventRow(const FCortexReplayEvent& Event);
/** One canonical frames.jsonl row, without its terminator. */
FString SerializeFrameRow(const FCortexReplayFrame& Frame);
/** metadata.json: the immutable fields plus the editable name, description and AI flag. */
FString SerializeMetadata(const FCortexReplayMetadata& Metadata, const FString& InitialStateSha256,
	const FString& InputsSha256);
/** The immutable metadata subset covered by the recording snapshot digest. */
FString SerializeImmutableMetadata(const FCortexReplayMetadata& Metadata, const FString& InitialStateSha256,
	const FString& InputsSha256);

// ---------------------------------------------------------------------------
// Parsing (each failure carries a human-readable reason)
// ---------------------------------------------------------------------------

bool ParseMetadata(const FString& Text, int32 ExpectedId, FCortexReplayMetadata& OutMetadata, FString& OutError);
bool ParseInitialState(const FString& Text, int32 ExpectedId, FCortexReplayInitialState& OutInitialState,
	FString& OutError);
/** Streams, hashes, parses and validates inputs.jsonl. Coverage is computed from the parsed guards. */
bool ParseInputsStream(const FString& Path, TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage, FString& OutSha256, FString& OutError);
/** Streams, hashes, parses and validates frames.jsonl against the event count and timing block. */
bool ParseFramesStream(const FString& Path, int32 EventCount, const FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames, FString& OutSha256, FString& OutError);

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool ValidateMetadataFields(const FCortexReplayMetadata& Metadata, FString& OutError);
bool ValidateInitialState(const FCortexReplayInitialState& InitialState, int32 ExpectedId, FString& OutError);
/** Contiguous sequences, non-decreasing times, eligible keys, press guards; reports the guard coverage. */
bool ValidateEvents(const TArray<FCortexReplayEvent>& Events, FCortexReplayGuardCoverage& OutCoverage,
	FString& OutError);
/** Contiguous frames, every event attributed exactly once, deltas in (0, max], timing.frame_count. */
bool ValidateFrameSet(const TArray<FCortexReplayFrame>& Frames, int32 EventCount,
	const FCortexReplayTiming& Timing, FString& OutError);
/** duration_seconds must equal the sum of the frame application deltas. */
bool ValidateDuration(double DurationSeconds, const TArray<FCortexReplayFrame>& Frames, FString& OutError);

/** Press-guard coverage of an event stream; the single implementation capture and validation share. */
FCortexReplayGuardCoverage ComputeGuardCoverage(const TArray<FCortexReplayEvent>& Events);
bool CoverageEquals(const FCortexReplayGuardCoverage& Left, const FCortexReplayGuardCoverage& Right);
double SumFrameDeltaSeconds(const TArray<FCortexReplayFrame>& Frames);

bool IsValidRecordingName(const FString& Name);
bool IsValidRecordingDescription(const FString& Description);
}
