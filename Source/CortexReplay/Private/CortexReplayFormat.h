#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"

class FJsonObject;

/**
 * The CortexReplay recording format (metadata.json / initial_state.json / inputs.jsonl.gz /
 * frames.jsonl.gz rows): serialization, strict parsing and validation. It owns every invariant a recording
 * must satisfy and knows nothing about where recordings are stored.
 */
namespace CortexReplayFormat
{
/**
 * Recording-side format version. Format 4 writes compact rows (defaults omitted, no per-row
 * schema_version/sequence/frame_index/first_sequence/time_seconds) of the engine frames that
 * consumed input after the capture epoch. library.json has its own, independent schema version in
 * the library facade.
 */
constexpr int32 RecordingFormatVersion = CortexReplayRecordingFormatVersion;

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
/** One compact inputs row (every default omitted), without its terminator. */
FString SerializeEventRow(const FCortexReplayEvent& Event);
/** One compact frames row (every default omitted), without its terminator. */
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
/** Parses rows in order (Sequence = row index) then runs ValidateEvents. */
bool ParseEventRows(TConstArrayView<FString> Rows, TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage, FString& OutError);
/** Parses rows in order (FrameIndex/FirstSequence derived, int64-widened) then runs ValidateFrameSet. */
bool ParseFrameRows(TConstArrayView<FString> Rows, int32 EventCount, const FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames, FString& OutError);

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool ValidateMetadataFields(const FCortexReplayMetadata& Metadata, FString& OutError);
bool ValidateInitialState(const FCortexReplayInitialState& InitialState, int32 ExpectedId, FString& OutError);
/** Contiguous sequences, eligible keys, press guards; reports the guard coverage. */
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
