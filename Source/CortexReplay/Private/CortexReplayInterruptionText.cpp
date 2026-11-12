#include "CortexReplayInterruptionText.h"

#include "CortexReplayErrorCodes.h"

#include "Dom/JsonObject.h"
#include "InputCoreTypes.h"

namespace
{
FString DetailString(const FCortexCommandResult& Cause, const TCHAR* Field)
{
	FString Value;
	if (Cause.ErrorDetails.IsValid())
	{
		Cause.ErrorDetails->TryGetStringField(Field, Value);
	}
	return Value;
}

/** Key and button names come from the engine's display names; an unknown name stays as recorded. */
FString DisplayName(const FString& Input)
{
	const FKey Key(*Input);
	return Key.IsValid() ? Key.GetDisplayName().ToString() : Input;
}

FString MakeForeignInputTitle(const FCortexCommandResult& Cause)
{
	const FString Kind = DetailString(Cause, TEXT("input_kind"));
	const FString Input = DetailString(Cause, TEXT("input"));
	const bool bRelease = DetailString(Cause, TEXT("edge")) == TEXT("release");
	if (Kind == TEXT("pointer_move"))
	{
		return TEXT("Replay stopped: you moved the mouse");
	}
	if (Kind == TEXT("wheel"))
	{
		return TEXT("Replay stopped: you scrolled the mouse wheel");
	}
	if (Kind == TEXT("key"))
	{
		return bRelease ? FString::Printf(TEXT("Replay stopped: you released %s"), *DisplayName(Input))
			: FString::Printf(TEXT("Replay stopped: you pressed %s"), *DisplayName(Input));
	}
	if (Kind == TEXT("mouse_button"))
	{
		return bRelease ? FString::Printf(TEXT("Replay stopped: you released %s"), *DisplayName(Input))
			: FString::Printf(TEXT("Replay stopped: you clicked %s"), *DisplayName(Input));
	}
	return FString::Printf(TEXT("Replay stopped: %s"), *Cause.ErrorMessage);
}
}

FString CortexReplayInterruptionText::MakeTitle(const FCortexCommandResult& Cause)
{
	const FString CauseKind = DetailString(Cause, TEXT("cause"));
	if (CauseKind == TEXT("foreign_input"))
	{
		return MakeForeignInputTitle(Cause);
	}
	if (CauseKind == TEXT("focus_lost"))
	{
		return TEXT("Replay stopped: the game window lost focus");
	}
	if (CauseKind == TEXT("player_lost"))
	{
		return TEXT("Replay stopped: the player character was destroyed");
	}
	if (CauseKind == TEXT("target_lost") || Cause.ErrorCode == CortexReplayErrorCodes::TargetUnavailable)
	{
		return TEXT("Replay stopped: the game session ended");
	}
	return FString::Printf(TEXT("Replay stopped: %s"), *Cause.ErrorMessage);
}

FCortexCommandResult CortexReplayInterruptionText::ToRunError(const FCortexCommandResult& Cause,
	int32 CompletedFrames, int32 FrameCount, double StoppedAtSeconds)
{
	FCortexCommandResult Error = Cause;
	Error.bSuccess = false;
	Error.ErrorMessage = MakeTitle(Cause);
	if (DetailString(Cause, TEXT("cause")) == TEXT("foreign_input"))
	{
		Error.ErrorCode = CortexReplayErrorCodes::ReplayInterruptedByInput;
	}
	// The incoming details object is shared with the cause, so the run error owns a copy.
	Error.ErrorDetails = MakeShared<FJsonObject>();
	if (Cause.ErrorDetails.IsValid())
	{
		Error.ErrorDetails->Values = Cause.ErrorDetails->Values;
	}
	Error.ErrorDetails->SetStringField(TEXT("reason"), Cause.ErrorMessage);
	Error.ErrorDetails->SetNumberField(TEXT("stopped_at_frame"), CompletedFrames);
	Error.ErrorDetails->SetNumberField(TEXT("frame_count"), FrameCount);
	Error.ErrorDetails->SetNumberField(TEXT("stopped_at_seconds"), StoppedAtSeconds);
	return Error;
}

FString CortexReplayInterruptionText::FormatStopPoint(double StoppedAtSeconds, int32 CompletedFrames,
	int32 FrameCount)
{
	// Whole tenths, rounded half up, so 75.25 reads 1:15.3 instead of following binary printf rounding.
	const int64 Tenths = static_cast<int64>(FMath::FloorToDouble(FMath::Max(StoppedAtSeconds, 0.0) * 10.0 + 0.5));
	return FString::Printf(TEXT("at %lld:%02lld.%lld, frame %d of %d"),
		Tenths / 600, (Tenths / 10) % 60, Tenths % 10, CompletedFrames, FrameCount);
}

bool CortexReplayInterruptionText::IsInputCause(const FCortexCommandResult& RunError)
{
	return RunError.ErrorCode == CortexReplayErrorCodes::ReplayInterruptedByInput;
}
