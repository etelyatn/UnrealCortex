#include "CortexReplayPayload.h"

#include "Containers/StringConv.h"
#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayFormat.h"
#include "CortexReplayGzip.h"

namespace
{
constexpr const TCHAR* EventsStreamName = TEXT("inputs.jsonl.gz");
constexpr const TCHAR* FramesStreamName = TEXT("frames.jsonl.gz");

FCortexCommandResult LimitExceeded(const TCHAR* Stream, const TCHAR* What, int64 MaxBytes)
{
	return FCortexCommandRouter::Error(CortexReplayErrorCodes::LimitExceeded,
		FString::Printf(TEXT("%s: %s exceed the %lld byte stream limit"), Stream, What, MaxBytes));
}

FCortexCommandResult InvalidRecording(const TCHAR* Stream, const FString& Reason)
{
	return FCortexCommandRouter::Error(CortexReplayErrorCodes::InvalidRecording,
		FString::Printf(TEXT("%s: %s"), Stream, *Reason));
}

FCortexCommandResult Ok()
{
	return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
}

/** Appends Row and its LF terminator; false (nothing appended past the limit check) when Raw would exceed MaxBytes. */
bool AppendRow(const FString& Row, int64 MaxBytes, TArray<uint8>& Raw)
{
	const FTCHARToUTF8 Utf8(*Row);
	if (static_cast<int64>(Raw.Num()) + Utf8.Length() + 1 > MaxBytes)
	{
		return false;
	}
	Raw.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Raw.Add('\n');
	return true;
}

FCortexCommandResult Compress(const TCHAR* Stream, TConstArrayView<uint8> Raw, TArray<uint8>& OutGz, int64 MaxBytes)
{
	FString Error;
	if (!CortexReplayGzip::Compress(Raw, OutGz, Error))
	{
		OutGz.Reset();
		return FCortexCommandRouter::Error(CortexReplayErrorCodes::StorageFailure,
			FString::Printf(TEXT("%s: %s"), Stream, *Error));
	}
	if (static_cast<int64>(OutGz.Num()) > MaxBytes)
	{
		OutGz.Reset();
		return LimitExceeded(Stream, TEXT("compressed bytes"), MaxBytes);
	}
	return Ok();
}

template <typename ItemType, typename SerializeType>
FCortexCommandResult Encode(const TCHAR* Stream, TConstArrayView<ItemType> Items, SerializeType Serialize,
	TArray<uint8>& OutGz, int64 MaxBytes)
{
	OutGz.Reset();
	TArray<uint8> Raw;
	for (const ItemType& Item : Items)
	{
		if (!AppendRow(Serialize(Item), MaxBytes, Raw))
		{
			return LimitExceeded(Stream, TEXT("uncompressed bytes"), MaxBytes);
		}
	}
	return Compress(Stream, Raw, OutGz, MaxBytes);
}

/** Decompresses and splits into rows. Failures are INVALID_RECORDING and leave OutRows empty. */
FCortexCommandResult DecodeRows(const TCHAR* Stream, TConstArrayView<uint8> Gz, int64 MaxBytes, TArray<FString>& OutRows)
{
	OutRows.Reset();
	if (static_cast<int64>(Gz.Num()) > MaxBytes)
	{
		return InvalidRecording(Stream, FString::Printf(TEXT("compressed size exceeds the %lld byte stream limit"), MaxBytes));
	}

	TArray<uint8> Raw;
	FString Error;
	if (!CortexReplayGzip::Decompress(Gz, MaxBytes, Raw, Error))
	{
		return InvalidRecording(Stream, Error);
	}
	if (Raw.Num() > 0 && Raw.Last() != '\n')
	{
		return InvalidRecording(Stream, TEXT("the final row is not terminated by LF"));
	}

	const uint8* const Data = Raw.GetData();
	int32 RowStart = 0;
	for (int32 Index = 0; Index < Raw.Num(); ++Index)
	{
		const uint8 Byte = Data[Index];
		if (Byte == '\r')
		{
			const int32 CrIndex = OutRows.Num();
			OutRows.Reset();
			return InvalidRecording(Stream, FString::Printf(TEXT("row %d contains a CR byte"), CrIndex));
		}
		if (Byte != '\n')
		{
			continue;
		}
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Data + RowStart), Index - RowStart);
		FString Row(Text.Length(), Text.Get());
		if (Row.TrimStartAndEnd().IsEmpty())
		{
			const int32 BlankIndex = OutRows.Num();
			OutRows.Reset();
			return InvalidRecording(Stream, FString::Printf(TEXT("row %d is blank"), BlankIndex));
		}
		OutRows.Add(MoveTemp(Row));
		RowStart = Index + 1;
	}
	return Ok();
}

FCortexCommandResult RowsFailed(const TCHAR* Stream, const FString& Reason)
{
	return FCortexCommandRouter::Error(CortexReplayErrorCodes::UnsupportedRecordingFormat,
		FString::Printf(TEXT("%s: %s"), Stream, *Reason));
}
} // namespace

FCortexCommandResult CortexReplayPayload::EncodeEvents(TConstArrayView<FCortexReplayEvent> Events, TArray<uint8>& OutGz,
	int64 MaxBytes)
{
	return Encode(EventsStreamName, Events, &CortexReplayFormat::SerializeEventRow, OutGz, MaxBytes);
}

FCortexCommandResult CortexReplayPayload::EncodeFrames(TConstArrayView<FCortexReplayFrame> Frames, TArray<uint8>& OutGz,
	int64 MaxBytes)
{
	return Encode(FramesStreamName, Frames, &CortexReplayFormat::SerializeFrameRow, OutGz, MaxBytes);
}

FCortexCommandResult CortexReplayPayload::DecodeEvents(TConstArrayView<uint8> Gz, TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage, int64 MaxBytes)
{
	OutEvents.Reset();
	OutCoverage = FCortexReplayGuardCoverage();
	TArray<FString> Rows;
	FCortexCommandResult Framed = DecodeRows(EventsStreamName, Gz, MaxBytes, Rows);
	if (!Framed.bSuccess)
	{
		return Framed;
	}
	FString Error;
	if (!CortexReplayFormat::ParseEventRows(Rows, OutEvents, OutCoverage, Error))
	{
		OutEvents.Reset();
		OutCoverage = FCortexReplayGuardCoverage();
		return RowsFailed(EventsStreamName, Error);
	}
	return Ok();
}

FCortexCommandResult CortexReplayPayload::DecodeFrames(TConstArrayView<uint8> Gz, int32 EventCount,
	const FCortexReplayTiming& Timing, TArray<FCortexReplayFrame>& OutFrames, int64 MaxBytes)
{
	OutFrames.Reset();
	TArray<FString> Rows;
	FCortexCommandResult Framed = DecodeRows(FramesStreamName, Gz, MaxBytes, Rows);
	if (!Framed.bSuccess)
	{
		return Framed;
	}
	FString Error;
	if (!CortexReplayFormat::ParseFrameRows(Rows, EventCount, Timing, OutFrames, Error))
	{
		OutFrames.Reset();
		return RowsFailed(FramesStreamName, Error);
	}
	return Ok();
}
