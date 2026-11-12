#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayInterruptionText.h"

#include "Dom/JsonObject.h"

namespace
{
FCortexCommandResult MakeForeignCause(const TCHAR* InputKind, const TCHAR* Input, const TCHAR* Edge)
{
	FCortexCommandResult Cause = FCortexCommandRouter::Error(
		FString(CortexEditorPhysicalInputErrorCodes::ForeignInterruption),
		TEXT("Foreign physical input interrupted replay playback"));
	Cause.AddContext(TEXT("cause"), TEXT("foreign_input"));
	Cause.AddContext(TEXT("input_kind"), InputKind);
	Cause.AddContext(TEXT("input"), Input);
	if (Edge != nullptr)
	{
		Cause.AddContext(TEXT("edge"), Edge);
	}
	return Cause;
}

FCortexCommandResult MakeCauseWithCode(const TCHAR* Code, const TCHAR* Message, const TCHAR* CauseKey)
{
	FCortexCommandResult Cause = FCortexCommandRouter::Error(FString(Code), FString(Message));
	if (CauseKey != nullptr)
	{
		Cause.AddContext(TEXT("cause"), CauseKey);
	}
	return Cause;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionTextTitlesTest,
	"Cortex.Replay.InterruptionText.Titles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayInterruptionTextTitlesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReplayInterruptionText;

	TestEqual(TEXT("Pointer move"), MakeTitle(MakeForeignCause(TEXT("pointer_move"), TEXT("Mouse2D"), nullptr)),
		FString(TEXT("Replay stopped: you moved the mouse")));
	TestEqual(TEXT("Key press"), MakeTitle(MakeForeignCause(TEXT("key"), TEXT("Q"), TEXT("press"))),
		FString(TEXT("Replay stopped: you pressed Q")));
	TestEqual(TEXT("Key release"), MakeTitle(MakeForeignCause(TEXT("key"), TEXT("Q"), TEXT("release"))),
		FString(TEXT("Replay stopped: you released Q")));
	const FString ButtonName = EKeys::LeftMouseButton.GetDisplayName().ToString();
	TestEqual(TEXT("Button press"),
		MakeTitle(MakeForeignCause(TEXT("mouse_button"), TEXT("LeftMouseButton"), TEXT("press"))),
		FString(TEXT("Replay stopped: you clicked ")) + ButtonName);
	TestEqual(TEXT("Button release"),
		MakeTitle(MakeForeignCause(TEXT("mouse_button"), TEXT("LeftMouseButton"), TEXT("release"))),
		FString(TEXT("Replay stopped: you released ")) + ButtonName);
	TestEqual(TEXT("Wheel"), MakeTitle(MakeForeignCause(TEXT("wheel"), TEXT("MouseWheelAxis"), nullptr)),
		FString(TEXT("Replay stopped: you scrolled the mouse wheel")));
	TestEqual(TEXT("Unknown key name falls back to the raw input"),
		MakeTitle(MakeForeignCause(TEXT("key"), TEXT("NotARealKeyName"), TEXT("press"))),
		FString(TEXT("Replay stopped: you pressed NotARealKeyName")));
	TestEqual(TEXT("Focus lost"),
		MakeTitle(MakeCauseWithCode(TEXT("INVALID_OPERATION"), TEXT("Focus left the replay route"), TEXT("focus_lost"))),
		FString(TEXT("Replay stopped: the game window lost focus")));
	TestEqual(TEXT("Target lost"),
		MakeTitle(MakeCauseWithCode(TEXT("INVALID_OPERATION"), TEXT("Target is gone"), TEXT("target_lost"))),
		FString(TEXT("Replay stopped: the game session ended")));
	TestEqual(TEXT("Target unavailable code"),
		MakeTitle(MakeCauseWithCode(TEXT("TARGET_UNAVAILABLE"), TEXT("No target"), nullptr)),
		FString(TEXT("Replay stopped: the game session ended")));
	TestEqual(TEXT("Unknown code keeps the message"),
		MakeTitle(MakeCauseWithCode(TEXT("SOMETHING_ELSE"), TEXT("It broke"), nullptr)),
		FString(TEXT("Replay stopped: It broke")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionTextRunErrorTest,
	"Cortex.Replay.InterruptionText.RunError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayInterruptionTextRunErrorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReplayInterruptionText;

	const FCortexCommandResult Foreign = ToRunError(
		MakeForeignCause(TEXT("key"), TEXT("Q"), TEXT("press")), 164, 2524, 2.7);
	TestFalse(TEXT("Run error is a failure"), Foreign.bSuccess);
	TestEqual(TEXT("Foreign input code"), Foreign.ErrorCode, FString(TEXT("REPLAY_INTERRUPTED_BY_INPUT")));
	TestEqual(TEXT("Foreign input message"), Foreign.ErrorMessage, FString(TEXT("Replay stopped: you pressed Q")));
	TestTrue(TEXT("Foreign input is an input cause"), IsInputCause(Foreign));
	if (TestTrue(TEXT("Foreign details exist"), Foreign.ErrorDetails.IsValid()))
	{
		TestEqual(TEXT("Input kept"), Foreign.ErrorDetails->GetStringField(TEXT("input")), FString(TEXT("Q")));
		TestEqual(TEXT("Cause kept"), Foreign.ErrorDetails->GetStringField(TEXT("cause")),
			FString(TEXT("foreign_input")));
		TestEqual(TEXT("Reason is the original message"), Foreign.ErrorDetails->GetStringField(TEXT("reason")),
			FString(TEXT("Foreign physical input interrupted replay playback")));
		TestEqual(TEXT("Stopped frame"),
			static_cast<int32>(Foreign.ErrorDetails->GetNumberField(TEXT("stopped_at_frame"))), 164);
		TestEqual(TEXT("Frame count"),
			static_cast<int32>(Foreign.ErrorDetails->GetNumberField(TEXT("frame_count"))), 2524);
		TestEqual(TEXT("Stopped seconds"), Foreign.ErrorDetails->GetNumberField(TEXT("stopped_at_seconds")), 2.7);
	}

	const FCortexCommandResult Focus = ToRunError(
		MakeCauseWithCode(TEXT("INVALID_OPERATION"), TEXT("Focus left"), TEXT("focus_lost")), 3, 10, 0.5);
	TestEqual(TEXT("Focus keeps its code"), Focus.ErrorCode, FString(TEXT("INVALID_OPERATION")));
	TestEqual(TEXT("Focus message is the title"), Focus.ErrorMessage,
		FString(TEXT("Replay stopped: the game window lost focus")));
	TestFalse(TEXT("Focus is not an input cause"), IsInputCause(Focus));

	const FCortexCommandResult Unknown = ToRunError(
		MakeCauseWithCode(TEXT("SOMETHING_ELSE"), TEXT("It broke"), nullptr), 1, 5, 0.1);
	TestEqual(TEXT("Unknown cause keeps the code"), Unknown.ErrorCode, FString(TEXT("SOMETHING_ELSE")));
	TestEqual(TEXT("Unknown cause message is prefixed"), Unknown.ErrorMessage,
		FString(TEXT("Replay stopped: It broke")));
	if (TestTrue(TEXT("Unknown details exist"), Unknown.ErrorDetails.IsValid()))
	{
		TestEqual(TEXT("Unknown reason"), Unknown.ErrorDetails->GetStringField(TEXT("reason")),
			FString(TEXT("It broke")));
		TestTrue(TEXT("Unknown stop keys present"),
			Unknown.ErrorDetails->HasField(TEXT("stopped_at_frame"))
			&& Unknown.ErrorDetails->HasField(TEXT("frame_count"))
			&& Unknown.ErrorDetails->HasField(TEXT("stopped_at_seconds")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionTextStopPointTest,
	"Cortex.Replay.InterruptionText.StopPoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayInterruptionTextStopPointTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReplayInterruptionText;

	TestEqual(TEXT("Start"), FormatStopPoint(0.0, 0, 10), FString(TEXT("at 0:00.0, frame 0 of 10")));
	TestEqual(TEXT("Seconds"), FormatStopPoint(2.7, 164, 2524), FString(TEXT("at 0:02.7, frame 164 of 2524")));
	TestEqual(TEXT("Minutes"), FormatStopPoint(75.25, 1, 2), FString(TEXT("at 1:15.3, frame 1 of 2")));
	return true;
}
