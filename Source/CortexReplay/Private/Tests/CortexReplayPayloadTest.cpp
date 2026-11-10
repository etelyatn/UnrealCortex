#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayGzip.h"
#include "CortexReplayPayload.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "InputCoreTypes.h"

namespace
{
constexpr EAutomationTestFlags PayloadTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

TArray<uint8> MakePayloadBytes(const char* Text)
{
	const FAnsiStringView View(Text);
	return TArray<uint8>(reinterpret_cast<const uint8*>(View.GetData()), View.Len());
}

TArray<uint8> MakeGz(TConstArrayView<uint8> Raw)
{
	TArray<uint8> Gz;
	FString Error;
	CortexReplayGzip::Compress(Raw, Gz, Error);
	return Gz;
}

FCortexReplayTimedEvent MakeTimedKey(ECortexEditorPhysicalInputKind Kind, double TimeSeconds)
{
	FCortexReplayTimedEvent Timed;
	Timed.Event.Input.Kind = Kind;
	Timed.Event.Input.Key = EKeys::SpaceBar;
	Timed.TimeSeconds = TimeSeconds;
	return Timed;
}

FCortexReplaySnapshot MakeTwoKeySnapshot(FCortexReplayTestFixture& Fixture)
{
	const TArray<FCortexReplayTimedEvent> Events = {MakeTimedKey(ECortexEditorPhysicalInputKind::KeyDown, 0.0),
		MakeTimedKey(ECortexEditorPhysicalInputKind::KeyUp, 0.05)};
	return Fixture.MakeRecording(1, false, Events);
}

FCortexCommandResult DecodeEventsFromRaw(const char* Text, TArray<FCortexReplayEvent>& OutEvents)
{
	FCortexReplayGuardCoverage Coverage;
	return CortexReplayPayload::DecodeEvents(MakeGz(MakePayloadBytes(Text)), OutEvents, Coverage);
}
bool PayloadModifiersEqual(const FModifierKeysState& A, const FModifierKeysState& B)
{
	return A.IsLeftShiftDown() == B.IsLeftShiftDown() && A.IsRightShiftDown() == B.IsRightShiftDown()
		&& A.IsLeftControlDown() == B.IsLeftControlDown() && A.IsRightControlDown() == B.IsRightControlDown()
		&& A.IsLeftAltDown() == B.IsLeftAltDown() && A.IsRightAltDown() == B.IsRightAltDown()
		&& A.IsLeftCommandDown() == B.IsLeftCommandDown() && A.IsRightCommandDown() == B.IsRightCommandDown()
		&& A.AreCapsLocked() == B.AreCapsLocked();
}

void ExpectPayloadEventEqual(FAutomationTestBase& Test, int32 Index, const FCortexReplayEvent& Actual, const FCortexReplayEvent& Expected)
{
	const FString Label = FString::Printf(TEXT("Event %d"), Index);
	Test.TestEqual(*(Label + TEXT(" sequence")), Actual.Sequence, Index);
	Test.TestTrue(*(Label + TEXT(" kind")), Actual.Input.Kind == Expected.Input.Kind);
	Test.TestEqual(*(Label + TEXT(" key")), Actual.Input.Key.ToString(), Expected.Input.Key.ToString());
	Test.TestTrue(*(Label + TEXT(" modifiers")), PayloadModifiersEqual(Actual.Input.Modifiers, Expected.Input.Modifiers));
	Test.TestEqual(*(Label + TEXT(" repeat")), Actual.Input.bRepeat, Expected.Input.bRepeat);
	Test.TestTrue(*(Label + TEXT(" viewport position")), Actual.Input.ViewportPosition == Expected.Input.ViewportPosition);
	Test.TestTrue(*(Label + TEXT(" delta")), Actual.Input.Delta == Expected.Input.Delta);
	Test.TestTrue(*(Label + TEXT(" wheel delta")), Actual.Input.WheelDelta == Expected.Input.WheelDelta);
	Test.TestEqual(*(Label + TEXT(" guard presence")), Actual.Guard.IsSet(), Expected.Guard.IsSet());
	if (!Actual.Guard.IsSet() || !Expected.Guard.IsSet())
	{
		return;
	}
	const FCortexReplayInteractionGuard& A = *Actual.Guard;
	const FCortexReplayInteractionGuard& E = *Expected.Guard;
	Test.TestTrue(*(Label + TEXT(" guard pawn transform")), A.ExpectedPose.PawnTransform.Equals(E.ExpectedPose.PawnTransform, 0.0));
	Test.TestTrue(*(Label + TEXT(" guard control rotation")), A.ExpectedPose.ControlRotation == E.ExpectedPose.ControlRotation);
	Test.TestTrue(*(Label + TEXT(" guard coverage")), A.UICoverage == E.UICoverage);
	Test.TestTrue(*(Label + TEXT(" guard reason")), A.UIUnavailableReason == E.UIUnavailableReason);
	Test.TestEqual(*(Label + TEXT(" guard target presence")), A.UITarget.IsValid(), E.UITarget.IsValid());
	if (A.UITarget.IsValid() && E.UITarget.IsValid())
	{
		Test.TestEqual(*(Label + TEXT(" guard identity digest")), A.UITarget->IdentitySha256, E.UITarget->IdentitySha256);
		Test.TestEqual(*(Label + TEXT(" guard identity root")), A.UITarget->RootClassPath, E.UITarget->RootClassPath);
	}
	Test.TestTrue(*(Label + TEXT(" guard local position")), A.ExpectedLocalPosition == E.ExpectedLocalPosition);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadRoundTripTest, "Cortex.Replay.Payload.RoundTrip", PayloadTestFlags)
bool FCortexReplayPayloadRoundTripTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	const FCortexReplaySnapshot Source = MakeTwoKeySnapshot(Fixture);

	TArray<uint8> EventsGz;
	TArray<uint8> FramesGz;
	TestTrue(TEXT("EncodeEvents succeeds"), CortexReplayPayload::EncodeEvents(Source.Events, EventsGz).bSuccess);
	TestTrue(TEXT("EncodeFrames succeeds"), CortexReplayPayload::EncodeFrames(Source.Frames, FramesGz).bSuccess);

	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	const FCortexCommandResult EventsResult = CortexReplayPayload::DecodeEvents(EventsGz, Events, Coverage);
	TestTrue(*FString::Printf(TEXT("DecodeEvents succeeds (%s)"), *EventsResult.ErrorMessage), EventsResult.bSuccess);
	TArray<FCortexReplayFrame> Frames;
	const FCortexCommandResult FramesResult =
		CortexReplayPayload::DecodeFrames(FramesGz, Events.Num(), Source.Metadata.Timing, Frames);
	TestTrue(*FString::Printf(TEXT("DecodeFrames succeeds (%s)"), *FramesResult.ErrorMessage), FramesResult.bSuccess);

	if (!TestEqual(TEXT("Event count"), Events.Num(), Source.Events.Num()))
	{
		return true;
	}
	for (int32 Index = 0; Index < Events.Num(); ++Index)
	{
		ExpectPayloadEventEqual(*this, Index, Events[Index], Source.Events[Index]);
	}
	TestEqual(TEXT("Pose presses"), Coverage.PosePresses, Source.Metadata.GuardCoverage.PosePresses);
	TestEqual(TEXT("UI supported presses"), Coverage.UISupportedPresses, Source.Metadata.GuardCoverage.UISupportedPresses);
	TestEqual(TEXT("UI unavailable presses"), Coverage.UIUnavailablePresses, Source.Metadata.GuardCoverage.UIUnavailablePresses);
	TestEqual(TEXT("UI not applicable presses"), Coverage.UINotApplicablePresses, Source.Metadata.GuardCoverage.UINotApplicablePresses);

	if (!TestEqual(TEXT("Frame count"), Frames.Num(), Source.Frames.Num()))
	{
		return true;
	}
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		TestEqual(TEXT("Frame index"), Frames[Index].FrameIndex, Source.Frames[Index].FrameIndex);
		TestEqual(TEXT("Frame delta"), Frames[Index].AppDeltaSeconds, Source.Frames[Index].AppDeltaSeconds);
		TestEqual(TEXT("Frame first sequence"), Frames[Index].FirstSequence, Source.Frames[Index].FirstSequence);
		TestEqual(TEXT("Frame event count"), Frames[Index].EventCount, Source.Frames[Index].EventCount);
		TestEqual(TEXT("Frame tick presence"), Frames[Index].WorldTick.IsSet(), Source.Frames[Index].WorldTick.IsSet());
		if (Frames[Index].WorldTick.IsSet() && Source.Frames[Index].WorldTick.IsSet())
		{
			TestEqual(TEXT("Frame tick delta"), Frames[Index].WorldTick->RealDeltaSeconds,
				Source.Frames[Index].WorldTick->RealDeltaSeconds);
			TestEqual(TEXT("Frame tick paused"), Frames[Index].WorldTick->bPaused, Source.Frames[Index].WorldTick->bPaused);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadEmptyStreamsTest, "Cortex.Replay.Payload.EmptyStreams", PayloadTestFlags)
bool FCortexReplayPayloadEmptyStreamsTest::RunTest(const FString& Parameters)
{
	TArray<uint8> EventsGz;
	TestTrue(TEXT("EncodeEvents of nothing succeeds"),
		CortexReplayPayload::EncodeEvents(TConstArrayView<FCortexReplayEvent>(), EventsGz).bSuccess);
	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	TestTrue(TEXT("DecodeEvents of empty stream succeeds"), CortexReplayPayload::DecodeEvents(EventsGz, Events, Coverage).bSuccess);
	TestEqual(TEXT("No events"), Events.Num(), 0);

	TArray<uint8> FramesGz;
	TestTrue(TEXT("EncodeFrames of nothing succeeds"),
		CortexReplayPayload::EncodeFrames(TConstArrayView<FCortexReplayFrame>(), FramesGz).bSuccess);
	FCortexReplayTiming Timing;
	Timing.FrameCount = 0;
	TArray<FCortexReplayFrame> Frames;
	TestTrue(TEXT("DecodeFrames of empty stream succeeds"),
		CortexReplayPayload::DecodeFrames(FramesGz, 0, Timing, Frames).bSuccess);
	TestEqual(TEXT("No frames"), Frames.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadFramingRejectedTest, "Cortex.Replay.Payload.FramingRejected", PayloadTestFlags)
bool FCortexReplayPayloadFramingRejectedTest::RunTest(const FString& Parameters)
{
	TArray<FCortexReplayEvent> Events;
	TestTrue(TEXT("Control: a well-framed valid row decodes"),
		DecodeEventsFromRaw("{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\n", Events).bSuccess);

	const auto ExpectInvalid = [this, &Events](const TCHAR* Label, const char* Text)
	{
		const FCortexCommandResult Result = DecodeEventsFromRaw(Text, Events);
		TestFalse(Label, Result.bSuccess);
		TestEqual(Label, Result.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));
	};
	ExpectInvalid(TEXT("CRLF terminator"), "{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\r\n");
	ExpectInvalid(TEXT("Blank row"), "{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\n\n{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\n");
	ExpectInvalid(TEXT("Whitespace-only row"), "   \n");
	ExpectInvalid(TEXT("Missing final LF"), "{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}");

	// A raw 0x0D inside a JSON string, built at the byte level.
	const char RawCrRow[] = {'{', '"', 'k', 'e', 'y', '"', ':', '"', 'S', 0x0D, '"', ',', '"', 'k', 'i', 'n', 'd', '"', ':', '"', 'k',
		'e', 'y', '_', 'u', 'p', '"', '}', '\n', 0};
	ExpectInvalid(TEXT("Raw CR byte inside a string"), RawCrRow);

	// Control: the JSON escape \r (5C 72) is well-framed, so the row fails validation instead.
	const char EscapedCrRow[] = {'{', '"', 'k', 'e', 'y', '"', ':', '"', 'S', 0x5C, 0x72, '"', ',', '"', 'k', 'i', 'n', 'd', '"', ':',
		'"', 'k', 'e', 'y', '_', 'u', 'p', '"', '}', '\n', 0};
	const FCortexCommandResult Escaped = DecodeEventsFromRaw(EscapedCrRow, Events);
	TestFalse(TEXT("Escaped CR key is rejected"), Escaped.bSuccess);
	TestEqual(TEXT("Escaped CR is a row failure"), Escaped.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadContainerRejectedTest, "Cortex.Replay.Payload.ContainerRejected", PayloadTestFlags)
bool FCortexReplayPayloadContainerRejectedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Gz = MakeGz(MakePayloadBytes("{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\n"));
	Gz.Pop();
	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	const FCortexCommandResult Result = CortexReplayPayload::DecodeEvents(Gz, Events, Coverage);
	TestFalse(TEXT("Truncated container is rejected"), Result.bSuccess);
	TestEqual(TEXT("Container failure code"), Result.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadRowRejectedTest, "Cortex.Replay.Payload.RowRejected", PayloadTestFlags)
bool FCortexReplayPayloadRowRejectedTest::RunTest(const FString& Parameters)
{
	TArray<FCortexReplayEvent> Events;
	const FCortexCommandResult Row = DecodeEventsFromRaw("{\"kind\":\"key_up\"}\n", Events);
	TestFalse(TEXT("Row missing its key is rejected"), Row.bSuccess);
	TestEqual(TEXT("Row failure code"), Row.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	FCortexReplayTiming Timing;
	Timing.FrameCount = 1;
	TArray<FCortexReplayFrame> Frames;
	const FCortexCommandResult Set = CortexReplayPayload::DecodeFrames(
		MakeGz(MakePayloadBytes("{\"app_delta_seconds\":0.0166667,\"event_count\":5}\n")), 1, Timing, Frames);
	TestFalse(TEXT("Frame event_count mismatch is rejected"), Set.bSuccess);
	TestEqual(TEXT("Set failure code"), Set.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayPayloadLimitsEnforcedTest, "Cortex.Replay.Payload.LimitsEnforced", PayloadTestFlags)
bool FCortexReplayPayloadLimitsEnforcedTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	const FCortexReplaySnapshot Source = MakeTwoKeySnapshot(Fixture);

	TArray<uint8> Gz;
	const FCortexCommandResult Encode = CortexReplayPayload::EncodeEvents(Source.Events, Gz, 16);
	TestFalse(TEXT("Encode over the raw limit fails"), Encode.bSuccess);
	TestEqual(TEXT("Encode limit code"), Encode.ErrorCode, FString(CortexReplayErrorCodes::LimitExceeded));

	TArray<uint8> FullGz;
	TestTrue(TEXT("Unlimited encode succeeds"), CortexReplayPayload::EncodeEvents(Source.Events, FullGz).bSuccess);
	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	const FCortexCommandResult Compressed = CortexReplayPayload::DecodeEvents(FullGz, Events, Coverage, FullGz.Num() - 1);
	TestFalse(TEXT("Gz above the limit is rejected"), Compressed.bSuccess);
	TestEqual(TEXT("Compressed limit code"), Compressed.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));

	TArray<uint8> Raw;
	while (Raw.Num() < 2000)
	{
		Raw.Append(MakePayloadBytes("{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}\n"));
	}
	const TArray<uint8> BombGz = MakeGz(Raw);
	TestTrue(TEXT("Payload is highly compressible"), BombGz.Num() <= 200);
	const FCortexCommandResult Bomb = CortexReplayPayload::DecodeEvents(BombGz, Events, Coverage, 1000);
	TestFalse(TEXT("Raw output above the limit is rejected"), Bomb.bSuccess);
	TestEqual(TEXT("Raw limit code"), Bomb.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));
	return true;
}
