#include "CortexReplayGzip.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace
{
/** Inflate output growth step; keeps allocation proportional to real output, never to a header claim. */
constexpr int64 InflateChunkBytes = 64 * 1024;

/** zlib's avail_in/avail_out are 32-bit; feed at most this much per call. */
constexpr int64 MaxZlibSpan = MAX_uint32;

FString DescribeZlib(const TCHAR* Operation, int ZlibResult, const z_stream& Stream)
{
	const FString Detail = Stream.msg != nullptr ? FString(ANSI_TO_TCHAR(Stream.msg)) : FString(TEXT("no detail"));
	return FString::Printf(TEXT("gzip %s failed (zlib code %d: %s)"), Operation, ZlibResult, *Detail);
}
} // namespace

bool CortexReplayGzip::Compress(TConstArrayView<uint8> Raw, TArray<uint8>& OutGz, FString& OutError)
{
	OutGz.Reset();
	OutError.Reset();

	if (static_cast<int64>(Raw.Num()) > MaxZlibSpan)
	{
		OutError = TEXT("gzip compress input exceeds the 4 GiB zlib span");
		return false;
	}

	z_stream Stream = {};
	const int InitResult = deflateInit2(&Stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
	if (InitResult != Z_OK)
	{
		OutError = DescribeZlib(TEXT("init"), InitResult, Stream);
		return false;
	}

	const uLong Bound = deflateBound(&Stream, static_cast<uLong>(Raw.Num()));
	// deflateBound does not cover the gzip wrapper with default settings on every zlib; add headroom.
	const int64 Capacity = static_cast<int64>(Bound) + 64;
	if (Capacity > static_cast<int64>(MAX_int32))
	{
		deflateEnd(&Stream);
		OutError = TEXT("gzip compress output cannot fit in a byte array");
		return false;
	}
	OutGz.SetNumUninitialized(static_cast<int32>(Capacity));

	// zlib never writes through next_in; the cast only satisfies its non-const Bytef pointer.
	Stream.next_in = const_cast<Bytef*>(Raw.GetData());
	Stream.avail_in = static_cast<uInt>(Raw.Num());
	Stream.next_out = OutGz.GetData();
	Stream.avail_out = static_cast<uInt>(OutGz.Num());

	const int DeflateResult = deflate(&Stream, Z_FINISH);
	const int64 Written = static_cast<int64>(Stream.total_out);
	if (DeflateResult != Z_STREAM_END)
	{
		OutError = DescribeZlib(TEXT("compress"), DeflateResult, Stream);
		deflateEnd(&Stream);
		OutGz.Reset();
		return false;
	}
	deflateEnd(&Stream);
	OutGz.SetNum(static_cast<int32>(Written), EAllowShrinking::Yes);
	return true;
}

bool CortexReplayGzip::Decompress(TConstArrayView<uint8> Gz, int64 MaxOutputBytes, TArray<uint8>& OutRaw, FString& OutError)
{
	OutRaw.Reset();
	OutError.Reset();

	if (Gz.Num() == 0)
	{
		OutError = TEXT("gzip stream is empty");
		return false;
	}
	if (MaxOutputBytes < 0)
	{
		OutError = TEXT("gzip output limit is negative");
		return false;
	}

	z_stream Stream = {};
	const int InitResult = inflateInit2(&Stream, 16 + MAX_WBITS);
	if (InitResult != Z_OK)
	{
		OutError = DescribeZlib(TEXT("init"), InitResult, Stream);
		return false;
	}

	// zlib never writes through next_in; the cast only satisfies its non-const Bytef pointer.
	Stream.next_in = const_cast<Bytef*>(Gz.GetData());
	Stream.avail_in = static_cast<uInt>(Gz.Num());

	int InflateResult = Z_OK;
	while (InflateResult != Z_STREAM_END)
	{
		const int64 Used = OutRaw.Num();
		const int64 Remaining = MaxOutputBytes - Used;
		// The array only ever grows within the remaining allowance. At the limit, one more output byte is
		// detected in stack scratch so the bounded array is never extended past MaxOutputBytes.
		const int64 Headroom = FMath::Min(InflateChunkBytes, Remaining);
		if (Used + Headroom > static_cast<int64>(MAX_int32))
		{
			OutError = TEXT("gzip output exceeds the addressable buffer size");
			break;
		}
		uint8 OverflowProbe = 0;
		if (Headroom > 0)
		{
			OutRaw.SetNumUninitialized(static_cast<int32>(Used + Headroom), EAllowShrinking::No);
			Stream.next_out = OutRaw.GetData() + Used;
			Stream.avail_out = static_cast<uInt>(Headroom);
		}
		else
		{
			Stream.next_out = &OverflowProbe;
			Stream.avail_out = 1;
		}

		InflateResult = inflate(&Stream, Z_NO_FLUSH);
		if (Headroom > 0)
		{
			const int64 Produced = Headroom - static_cast<int64>(Stream.avail_out);
			OutRaw.SetNum(static_cast<int32>(Used + Produced), EAllowShrinking::No);
		}
		else if (Stream.avail_out == 0)
		{
			OutError = FString::Printf(TEXT("gzip output exceeds the %lld byte limit"), MaxOutputBytes);
			break;
		}
		if (InflateResult == Z_STREAM_END)
		{
			break;
		}
		if (InflateResult == Z_BUF_ERROR && Stream.avail_in == 0)
		{
			OutError = TEXT("gzip stream is truncated");
			break;
		}
		if (InflateResult != Z_OK)
		{
			OutError = DescribeZlib(TEXT("decompress"), InflateResult, Stream);
			break;
		}
		if (Stream.avail_in == 0 && Stream.avail_out != 0)
		{
			// All input consumed, output not full, and no stream end: the member is incomplete.
			OutError = TEXT("gzip stream is truncated");
			break;
		}
	}

	const bool bEnded = InflateResult == Z_STREAM_END;
	const uInt Leftover = Stream.avail_in;
	inflateEnd(&Stream);

	if (!OutError.IsEmpty())
	{
		OutRaw.Reset();
		return false;
	}
	if (!bEnded)
	{
		OutError = TEXT("gzip stream did not reach its end");
		OutRaw.Reset();
		return false;
	}
	if (Leftover != 0)
	{
		OutError = FString::Printf(TEXT("gzip stream has %u trailing bytes after the member"), static_cast<uint32>(Leftover));
		OutRaw.Reset();
		return false;
	}
	OutRaw.Shrink();
	return true;
}
