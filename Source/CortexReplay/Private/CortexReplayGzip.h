#pragma once

#include "CoreMinimal.h"

/** RFC 1952 gzip framing for recording streams, backed directly by the engine's zlib. */
namespace CortexReplayGzip
{
/** One RFC 1952 member, header mtime 0, default level. Empty Raw yields a valid empty-payload member. */
bool Compress(TConstArrayView<uint8> Raw, TArray<uint8>& OutGz, FString& OutError);

/**
 * Exactly one member, CRC32/ISIZE verified by zlib, every input byte consumed, output never above
 * MaxOutputBytes (grown incrementally; ISIZE never sizes an allocation). Never logs.
 */
bool Decompress(TConstArrayView<uint8> Gz, int64 MaxOutputBytes, TArray<uint8>& OutRaw, FString& OutError);
} // namespace CortexReplayGzip
