#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"

/**
 * Payload codec of one recording stream (inputs.jsonl.gz / frames.jsonl.gz): LF-terminated compact
 * rows, gzip-compressed, bounded in both compressed and uncompressed size. Knows nothing about where
 * the bytes are stored.
 */
namespace CortexReplayPayload
{
/** Per-stream size limit, compressed and uncompressed. */
constexpr int64 MaxStreamBytes = 256ll * 1024 * 1024;

/** Rows + LF → gzip. LIMIT_EXCEEDED when raw or compressed bytes exceed MaxBytes; STORAGE_FAILURE on zlib failure. */
FCortexCommandResult EncodeEvents(TConstArrayView<FCortexReplayEvent> Events, TArray<uint8>& OutGz,
	int64 MaxBytes = MaxStreamBytes);
FCortexCommandResult EncodeFrames(TConstArrayView<FCortexReplayFrame> Frames, TArray<uint8>& OutGz,
	int64 MaxBytes = MaxStreamBytes);

/**
 * gzip → framing → rows. INVALID_RECORDING: Gz above MaxBytes, decompression failure/limit, CR, blank
 * or whitespace-only row, missing final LF. UNSUPPORTED_RECORDING_FORMAT: row or set validation.
 */
FCortexCommandResult DecodeEvents(TConstArrayView<uint8> Gz, TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage, int64 MaxBytes = MaxStreamBytes);
FCortexCommandResult DecodeFrames(TConstArrayView<uint8> Gz, int32 EventCount, const FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames, int64 MaxBytes = MaxStreamBytes);
} // namespace CortexReplayPayload
