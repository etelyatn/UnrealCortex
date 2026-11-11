#include "Misc/AutomationTest.h"

#include "CortexReplayFormat.h"
#include "CortexReplayTypes.h"

namespace
{
constexpr EAutomationTestFlags RowsTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

FCortexReplayFrame MakeFrame(double AppDelta, TOptional<FCortexReplayFrameTick> Tick, int32 EventCount)
{
	FCortexReplayFrame Frame;
	Frame.AppDeltaSeconds = AppDelta;
	Frame.WorldTick = Tick;
	Frame.EventCount = EventCount;
	return Frame;
}

FCortexReplayFrameTick MakeTick(float RealDelta, bool bPaused)
{
	FCortexReplayFrameTick Tick;
	Tick.RealDeltaSeconds = RealDelta;
	Tick.bPaused = bPaused;
	return Tick;
}

FCortexReplayEvent MakeEvent(ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event;
	Event.Input.Kind = Kind;
	Event.Input.Key = Key;
	return Event;
}

FCortexReplayInteractionGuard MakeNotApplicableGuard()
{
	FCortexReplayInteractionGuard Guard;
	Guard.UICoverage = ECortexEditorUICoverage::NotApplicable;
	return Guard;
}

FCortexReplayInteractionGuard MakeSupportedGuard()
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::UMG;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
	Identity->RootClassPath = TEXT("/Game/UI/WBP_FormatRows.WBP_FormatRows_C");
	Identity->WidgetAncestry.Add(FName(TEXT("Root")));
	Identity->IdentitySha256 = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(*Identity);

	FCortexReplayInteractionGuard Guard;
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UITarget = TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
	Guard.ExpectedLocalPosition = FVector2D(0.25, 0.75);
	return Guard;
}

FCortexReplayInteractionGuard MakeUnavailableGuard()
{
	FCortexReplayInteractionGuard Guard;
	Guard.UICoverage = ECortexEditorUICoverage::Unavailable;
	Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::DynamicInstance;
	return Guard;
}

bool ModifiersEqual(const FModifierKeysState& A, const FModifierKeysState& B)
{
	return A.IsLeftShiftDown() == B.IsLeftShiftDown() && A.IsRightShiftDown() == B.IsRightShiftDown()
		&& A.IsLeftControlDown() == B.IsLeftControlDown() && A.IsRightControlDown() == B.IsRightControlDown()
		&& A.IsLeftAltDown() == B.IsLeftAltDown() && A.IsRightAltDown() == B.IsRightAltDown()
		&& A.IsLeftCommandDown() == B.IsLeftCommandDown() && A.IsRightCommandDown() == B.IsRightCommandDown()
		&& A.AreCapsLocked() == B.AreCapsLocked();
}

void ExpectEventEqual(FAutomationTestBase& Test, int32 Index, const FCortexReplayEvent& Actual,
	const FCortexReplayEvent& Expected)
{
	const FString Label = FString::Printf(TEXT("Event %d"), Index);
	Test.TestEqual(*(Label + TEXT(" sequence")), Actual.Sequence, Index);
	Test.TestTrue(*(Label + TEXT(" kind")), Actual.Input.Kind == Expected.Input.Kind);
	Test.TestEqual(*(Label + TEXT(" key")), Actual.Input.Key.ToString(), Expected.Input.Key.ToString());
	Test.TestTrue(*(Label + TEXT(" modifiers")), ModifiersEqual(Actual.Input.Modifiers, Expected.Input.Modifiers));
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

const TCHAR* const BaseEventRow = TEXT("{\"key\":\"SpaceBar\",\"kind\":\"key_up\"");
const TCHAR* const BaseFrameRow = TEXT("{\"app_delta_seconds\":0.1,\"world_tick\":{\"real_delta_seconds\":0.5,\"paused\":false}}");

FString EventRowWith(const TCHAR* Extra)
{
	return FString(BaseEventRow) + TEXT(",") + Extra + TEXT("}");
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsIdleFrameTest,
	"Cortex.Replay.FormatRows.IdleFrameWritesOnlyDelta",
	RowsTestFlags
)

bool FCortexReplayFormatRowsIdleFrameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double AppDelta = 0.016666799783706665;
	const FCortexReplayFrame Frame = MakeFrame(AppDelta, MakeTick(static_cast<float>(AppDelta), false), 0);
	TestEqual(TEXT("An idle derivable frame is one number"), CortexReplayFormat::SerializeFrameRow(Frame),
		FString(TEXT("{\"app_delta_seconds\":0.016666799783706665}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsFrameExceptionsTest,
	"Cortex.Replay.FormatRows.FrameExceptionsRoundTrip",
	RowsTestFlags
)

bool FCortexReplayFormatRowsFrameExceptionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FCortexReplayFrame> Source;
	Source.Add(MakeFrame(0.016, TOptional<FCortexReplayFrameTick>(), 2));
	Source.Add(MakeFrame(0.016, MakeTick(0.016f, true), 0));
	Source.Add(MakeFrame(0.0166667, MakeTick(0.02f, false), 0));
	Source.Add(MakeFrame(0.1, MakeTick(static_cast<float>(0.1), false), 1));

	TArray<FString> Rows;
	for (const FCortexReplayFrame& Frame : Source)
	{
		Rows.Add(CortexReplayFormat::SerializeFrameRow(Frame));
	}

	TestTrue(TEXT("An unticked frame writes world_tick null"), Rows[0].Contains(TEXT("\"world_tick\":null")));
	TestTrue(TEXT("A paused frame writes the world_tick object"), Rows[1].Contains(TEXT("\"world_tick\":{")));
	TestTrue(TEXT("A frame with a different real delta writes the world_tick object"),
		Rows[2].Contains(TEXT("\"world_tick\":{")));
	TestFalse(TEXT("A derivable frame omits world_tick"), Rows[3].Contains(TEXT("world_tick")));

	FCortexReplayTiming Timing;
	Timing.FrameCount = 4;
	TArray<FCortexReplayFrame> Parsed;
	FString Error;
	if (!TestTrue(TEXT("The rows parse"), CortexReplayFormat::ParseFrameRows(Rows, 3, Timing, Parsed, Error)))
	{
		AddError(Error);
		return false;
	}

	const int32 ExpectedFirstSequence[4] = { 0, 2, 2, 2 };
	if (!TestEqual(TEXT("Frame count"), Parsed.Num(), 4))
	{
		return false;
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FString Label = FString::Printf(TEXT("Frame %d"), Index);
		TestEqual(*(Label + TEXT(" index")), Parsed[Index].FrameIndex, Index);
		TestEqual(*(Label + TEXT(" first sequence")), Parsed[Index].FirstSequence, ExpectedFirstSequence[Index]);
		TestEqual(*(Label + TEXT(" event count")), Parsed[Index].EventCount, Source[Index].EventCount);
		TestTrue(*(Label + TEXT(" app delta exact")), Parsed[Index].AppDeltaSeconds == Source[Index].AppDeltaSeconds);
		TestEqual(*(Label + TEXT(" world tick presence")), Parsed[Index].WorldTick.IsSet(), Source[Index].WorldTick.IsSet());
		if (Parsed[Index].WorldTick.IsSet() && Source[Index].WorldTick.IsSet())
		{
			TestTrue(*(Label + TEXT(" real delta exact")),
				Parsed[Index].WorldTick->RealDeltaSeconds == Source[Index].WorldTick->RealDeltaSeconds);
			TestEqual(*(Label + TEXT(" paused")), Parsed[Index].WorldTick->bPaused, Source[Index].WorldTick->bPaused);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsEventDefaultsTest,
	"Cortex.Replay.FormatRows.EventDefaultsOmitted",
	RowsTestFlags
)

bool FCortexReplayFormatRowsEventDefaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FCortexReplayEvent KeyUp = MakeEvent(ECortexEditorPhysicalInputKind::KeyUp, EKeys::SpaceBar);
	TestEqual(TEXT("A default key_up is kind and key only"), CortexReplayFormat::SerializeEventRow(KeyUp),
		FString(TEXT("{\"key\":\"SpaceBar\",\"kind\":\"key_up\"}")));

	FCortexReplayEvent Move = MakeEvent(ECortexEditorPhysicalInputKind::RelativeMove, EKeys::Mouse2D);
	Move.Input.Delta = FVector2D(-3.0, 0.0);
	const FString Row = CortexReplayFormat::SerializeEventRow(Move);
	for (const TCHAR* Key : { TEXT("viewport_position"), TEXT("wheel_delta"), TEXT("repeat"), TEXT("modifiers"),
		TEXT("sequence"), TEXT("time_seconds"), TEXT("coordinate_space"), TEXT("schema_version") })
	{
		TestFalse(*FString::Printf(TEXT("A relative_move omits %s"), Key), Row.Contains(Key));
	}
	TestTrue(TEXT("The non-default delta is written"), Row.Contains(TEXT("\"delta\":{")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsEventRoundTripTest,
	"Cortex.Replay.FormatRows.EventRoundTripAllFields",
	RowsTestFlags
)

bool FCortexReplayFormatRowsEventRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using EKind = ECortexEditorPhysicalInputKind;
	TArray<FCortexReplayEvent> Source;

	FCortexReplayEvent& KeyDown = Source.Add_GetRef(MakeEvent(EKind::KeyDown, EKeys::SpaceBar));
	KeyDown.Guard = MakeNotApplicableGuard();

	Source.Add(MakeEvent(EKind::KeyUp, EKeys::SpaceBar));

	FCortexReplayEvent& PointerMove = Source.Add_GetRef(MakeEvent(EKind::PointerMove, EKeys::Mouse2D));
	PointerMove.Input.ViewportPosition = FVector2D(651.0, 197.3333740234375);

	FCortexReplayEvent& RelativeMove = Source.Add_GetRef(MakeEvent(EKind::RelativeMove, EKeys::Mouse2D));
	RelativeMove.Input.Delta = FVector2D(-7.0, 2.0);

	FCortexReplayEvent& PointerDown = Source.Add_GetRef(MakeEvent(EKind::PointerDown, EKeys::LeftMouseButton));
	PointerDown.Guard = MakeSupportedGuard();

	Source.Add(MakeEvent(EKind::PointerUp, EKeys::LeftMouseButton));

	FCortexReplayEvent& DoubleClick = Source.Add_GetRef(MakeEvent(EKind::DoubleClick, EKeys::LeftMouseButton));
	DoubleClick.Guard = MakeUnavailableGuard();

	FCortexReplayEvent& Wheel = Source.Add_GetRef(MakeEvent(EKind::Wheel, EKeys::MouseWheelAxis));
	Wheel.Input.WheelDelta = 1.5f;

	FCortexReplayEvent& Repeat = Source.Add_GetRef(MakeEvent(EKind::KeyDown, EKeys::W));
	Repeat.Input.bRepeat = true;

	for (int32 Modifier = 0; Modifier < 9; ++Modifier)
	{
		bool Flags[9] = { false, false, false, false, false, false, false, false, false };
		Flags[Modifier] = true;
		FCortexReplayEvent& Event = Source.Add_GetRef(MakeEvent(EKind::KeyUp, EKeys::SpaceBar));
		Event.Input.Modifiers = FModifierKeysState(Flags[0], Flags[1], Flags[2], Flags[3], Flags[4], Flags[5],
			Flags[6], Flags[7], Flags[8]);
	}
	FCortexReplayEvent& Combined = Source.Add_GetRef(MakeEvent(EKind::KeyUp, EKeys::SpaceBar));
	Combined.Input.Modifiers = FModifierKeysState(true, false, false, true, false, false, false, false, true);

	TArray<FString> Rows;
	for (int32 Index = 0; Index < Source.Num(); ++Index)
	{
		Source[Index].Sequence = Index;
		Rows.Add(CortexReplayFormat::SerializeEventRow(Source[Index]));
	}

	TArray<FCortexReplayEvent> Parsed;
	FCortexReplayGuardCoverage Coverage;
	FString Error;
	if (!TestTrue(TEXT("The rows parse"), CortexReplayFormat::ParseEventRows(Rows, Parsed, Coverage, Error)))
	{
		AddError(Error);
		return false;
	}

	TestTrue(TEXT("Coverage matches the source"),
		CortexReplayFormat::CoverageEquals(Coverage, CortexReplayFormat::ComputeGuardCoverage(Source)));
	if (!TestEqual(TEXT("Event count"), Parsed.Num(), Source.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Source.Num(); ++Index)
	{
		ExpectEventEqual(*this, Index, Parsed[Index], Source[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsRejectsUnknownKeysTest,
	"Cortex.Replay.FormatRows.RejectsUnknownKeys",
	RowsTestFlags
)

bool FCortexReplayFormatRowsRejectsUnknownKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTiming Timing;
	Timing.FrameCount = 1;

	const auto ParseEvent = [](const FString& Row)
	{
		TArray<FCortexReplayEvent> Events;
		FCortexReplayGuardCoverage Coverage;
		FString Error;
		return CortexReplayFormat::ParseEventRows({ Row }, Events, Coverage, Error);
	};
	const auto ParseFrame = [&Timing](const FString& Row)
	{
		TArray<FCortexReplayFrame> Frames;
		FString Error;
		return CortexReplayFormat::ParseFrameRows({ Row }, 0, Timing, Frames, Error);
	};

	// A key_up needs no guard, so the base row is valid and every failure below is the added member.
	TestTrue(TEXT("Control: the base event row parses"), ParseEvent(FString(BaseEventRow) + TEXT("}")));
	TestTrue(TEXT("Control: the base frame row parses"), ParseFrame(BaseFrameRow));

	for (const TCHAR* Extra : {
		TEXT("\"time_seconds\":0"), TEXT("\"sequence\":0"), TEXT("\"schema_version\":4"),
		TEXT("\"coordinate_space\":\"none\""), TEXT("\"modifiers\":{\"num_lock\":true}"),
		TEXT("\"modifiers\":{\"left_shift\":null}"), TEXT("\"modifiers\":{\"left_shift\":1}") })
	{
		TestFalse(*FString::Printf(TEXT("An event row with %s is rejected"), Extra), ParseEvent(EventRowWith(Extra)));
	}

	TestFalse(TEXT("An unknown world_tick member is rejected"),
		ParseFrame(TEXT("{\"app_delta_seconds\":0.1,\"world_tick\":{\"real_delta_seconds\":0.5,\"paused\":false,\"x\":1}}")));
	TestFalse(TEXT("A world_tick without paused is rejected"),
		ParseFrame(TEXT("{\"app_delta_seconds\":0.1,\"world_tick\":{\"real_delta_seconds\":0.5}}")));
	TestFalse(TEXT("A frame_index member is rejected"),
		ParseFrame(TEXT("{\"app_delta_seconds\":0.1,\"frame_index\":0}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsAcceptsExplicitDefaultsTest,
	"Cortex.Replay.FormatRows.AcceptsExplicitDefaults",
	RowsTestFlags
)

bool FCortexReplayFormatRowsAcceptsExplicitDefaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const auto Parse = [](const FString& Row, TArray<FCortexReplayEvent>& OutEvents)
	{
		FCortexReplayGuardCoverage Coverage;
		FString Error;
		return CortexReplayFormat::ParseEventRows({ Row }, OutEvents, Coverage, Error);
	};

	TArray<FCortexReplayEvent> Omitted;
	TArray<FCortexReplayEvent> Explicit;
	if (!TestTrue(TEXT("The omitted form parses"), Parse(FString(BaseEventRow) + TEXT("}"), Omitted))
		|| !TestTrue(TEXT("The explicit-default form parses"),
			Parse(TEXT("{\"key\":\"SpaceBar\",\"kind\":\"key_up\",\"repeat\":false,\"modifiers\":{\"left_shift\":false},")
				TEXT("\"wheel_delta\":0,\"delta\":{\"x\":0,\"y\":0}}"), Explicit)))
	{
		return false;
	}

	if (TestEqual(TEXT("One event each"), Omitted.Num() + Explicit.Num(), 2))
	{
		ExpectEventEqual(*this, 0, Explicit[0], Omitted[0]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayFormatRowsAttributionOverflowTest,
	"Cortex.Replay.FormatRows.AttributionOverflowRejected",
	RowsTestFlags
)

bool FCortexReplayFormatRowsAttributionOverflowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Rows = {
		TEXT("{\"app_delta_seconds\":0.1,\"event_count\":2147483647}"),
		TEXT("{\"app_delta_seconds\":0.1,\"event_count\":1}"),
		TEXT("{\"app_delta_seconds\":0.1,\"event_count\":1}") };
	FCortexReplayTiming Timing;
	Timing.FrameCount = 3;

	TArray<FCortexReplayFrame> Frames;
	FString Error;
	TestFalse(TEXT("A derived first_sequence above MAX_int32 is rejected"),
		CortexReplayFormat::ParseFrameRows(Rows, MAX_int32, Timing, Frames, Error));
	return true;
}
