#include "Misc/AutomationTest.h"

#include "CortexReplayGzip.h"

namespace
{
/** Python gzip.compress(b'hello\n', 9, mtime=0): a standard member this module did not produce. */
TArray<uint8> MakeHelloVector()
{
	return TArray<uint8>({0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xe7, 0x02, 0x00,
		0x20, 0x30, 0x3a, 0x36, 0x06, 0x00, 0x00, 0x00});
}

TArray<uint8> MakeBytes(const char* Text)
{
	const FAnsiStringView View(Text);
	return TArray<uint8>(reinterpret_cast<const uint8*>(View.GetData()), View.Len());
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGzipRoundTripTest, "Cortex.Replay.Gzip.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGzipRoundTripTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Raw = MakeBytes("hello\n");
	TArray<uint8> Gz;
	FString Error;
	TestTrue(TEXT("Compress succeeds"), CortexReplayGzip::Compress(Raw, Gz, Error));
	if (Gz.Num() >= 8)
	{
		TestTrue(TEXT("gzip magic"), Gz[0] == 0x1f && Gz[1] == 0x8b);
		TestTrue(TEXT("header mtime is zero"), Gz[4] == 0 && Gz[5] == 0 && Gz[6] == 0 && Gz[7] == 0);
	}
	else
	{
		AddError(TEXT("Compressed output is shorter than a gzip header"));
	}

	TArray<uint8> Back;
	TestTrue(TEXT("Decompress succeeds"), CortexReplayGzip::Decompress(Gz, 1024, Back, Error));
	TestEqual(TEXT("Round trip equals input"), Back, Raw);

	const TArray<uint8> Line = MakeBytes("{\"app_delta_seconds\":0.016666799783706665}\n");
	TArray<uint8> Big;
	while (Big.Num() < 70000)
	{
		Big.Append(Line);
	}
	TArray<uint8> BigGz;
	TArray<uint8> BigBack;
	TestTrue(TEXT("Big compress succeeds"), CortexReplayGzip::Compress(Big, BigGz, Error));
	TestTrue(TEXT("Repetitive data compresses well"), BigGz.Num() < 2000);
	TestTrue(TEXT("Big decompress succeeds"), CortexReplayGzip::Decompress(BigGz, Big.Num(), BigBack, Error));
	TestEqual(TEXT("Big round trip equals input"), BigBack, Big);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGzipEmptyPayloadTest, "Cortex.Replay.Gzip.EmptyPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGzipEmptyPayloadTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Gz;
	FString Error;
	TestTrue(TEXT("Compress of empty input succeeds"), CortexReplayGzip::Compress(TConstArrayView<uint8>(), Gz, Error));
	TestTrue(TEXT("Empty payload still yields a member"), Gz.Num() > 0);

	TArray<uint8> Raw;
	TestTrue(TEXT("Decompress succeeds"), CortexReplayGzip::Decompress(Gz, 16, Raw, Error));
	TestEqual(TEXT("Payload is empty"), Raw.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGzipReadsStandardGzipTest, "Cortex.Replay.Gzip.ReadsStandardGzip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGzipReadsStandardGzipTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Raw;
	FString Error;
	TestTrue(TEXT("Standard gzip decodes"), CortexReplayGzip::Decompress(MakeHelloVector(), 1024, Raw, Error));
	TestEqual(TEXT("Decoded text"), Raw, MakeBytes("hello\n"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGzipRejectsDamageTest, "Cortex.Replay.Gzip.RejectsDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGzipRejectsDamageTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Good = MakeHelloVector();

	TArray<uint8> Truncated = Good;
	Truncated.Pop();
	TArray<uint8> BadCrc = Good;
	BadCrc[19] ^= 0xff;
	TArray<uint8> Trailing = Good;
	Trailing.Add(0x00);
	TArray<uint8> TwoMembers = Good;
	TwoMembers.Append(Good);

	const TPair<const TCHAR*, TArray<uint8>> Cases[] = {
		{TEXT("missing last byte"), Truncated},
		{TEXT("corrupt CRC"), BadCrc},
		{TEXT("trailing byte"), Trailing},
		{TEXT("second member"), TwoMembers},
		{TEXT("not gzip"), MakeBytes("not gzip")},
		{TEXT("empty input"), TArray<uint8>()},
	};
	for (const TPair<const TCHAR*, TArray<uint8>>& Case : Cases)
	{
		TArray<uint8> Raw;
		FString Error;
		TestFalse(FString::Printf(TEXT("Rejects %s"), Case.Key), CortexReplayGzip::Decompress(Case.Value, 1024, Raw, Error));
		TestFalse(FString::Printf(TEXT("Reports an error for %s"), Case.Key), Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayGzipRespectsOutputLimitTest, "Cortex.Replay.Gzip.RespectsOutputLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayGzipRespectsOutputLimitTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Raw;
	FString Error;
	TestFalse(TEXT("Limit below payload size rejects"), CortexReplayGzip::Decompress(MakeHelloVector(), 5, Raw, Error));
	TestFalse(TEXT("Rejection carries an error"), Error.IsEmpty());
	TestEqual(TEXT("Rejected output is left empty"), Raw.Num(), 0);
	TestFalse(TEXT("Limit zero rejects a non-empty payload"), CortexReplayGzip::Decompress(MakeHelloVector(), 0, Raw, Error));
	TestEqual(TEXT("Zero-limit output is left empty"), Raw.Num(), 0);
	TestTrue(TEXT("Limit equal to payload size accepts"), CortexReplayGzip::Decompress(MakeHelloVector(), 6, Raw, Error));
	TestEqual(TEXT("Accepted output holds exactly the payload"), Raw.Num(), 6);
	return true;
}
