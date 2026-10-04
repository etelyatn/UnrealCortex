#pragma once

#include "CoreMinimal.h"

/**
 * Error codes for CortexReplay.
 *
 * The generic codes mirror the canonical values declared by CortexErrorCodes
 * (CortexCore) so the router and Replay handlers cannot drift; they are restated
 * here as constexpr TCHAR literals for allocation-free use inside the library.
 * Replay-specific codes keep their own constexpr TCHAR literals.
 */
namespace CortexReplayErrorCodes
{
	// Generic codes shared with CortexErrorCodes.
	constexpr TCHAR InvalidField[] = TEXT("INVALID_FIELD");
	constexpr TCHAR InvalidValue[] = TEXT("INVALID_VALUE");
	constexpr TCHAR InvalidOperation[] = TEXT("INVALID_OPERATION");
	constexpr TCHAR UnknownCommand[] = TEXT("UNKNOWN_COMMAND");
	constexpr TCHAR DirtyEditorState[] = TEXT("DIRTY_EDITOR_STATE");
	constexpr TCHAR EditorBusy[] = TEXT("EDITOR_BUSY");
	constexpr TCHAR AssetNotFound[] = TEXT("ASSET_NOT_FOUND");
	constexpr TCHAR SaveFailed[] = TEXT("SAVE_FAILED");
	constexpr TCHAR LimitExceeded[] = TEXT("LIMIT_EXCEEDED");

	// Replay-specific codes.
	constexpr TCHAR RecordingNotFound[] = TEXT("RECORDING_NOT_FOUND");
	constexpr TCHAR InvalidRecording[] = TEXT("INVALID_RECORDING");
	constexpr TCHAR UnsupportedRecordingFormat[] = TEXT("UNSUPPORTED_RECORDING_FORMAT");
	constexpr TCHAR IncompleteRecording[] = TEXT("INCOMPLETE_RECORDING");
	constexpr TCHAR PermissionDenied[] = TEXT("PERMISSION_DENIED");
	constexpr TCHAR TargetUnavailable[] = TEXT("TARGET_UNAVAILABLE");
	constexpr TCHAR IncompatiblePrerequisites[] = TEXT("INCOMPATIBLE_PREREQUISITES");
	constexpr TCHAR RunNotFound[] = TEXT("RUN_NOT_FOUND");
	constexpr TCHAR StorageFailure[] = TEXT("STORAGE_FAILURE");
	constexpr TCHAR ReplayTimingError[] = TEXT("REPLAY_TIMING_ERROR");
	constexpr TCHAR ReplayPoseGuardFailed[] = TEXT("REPLAY_POSE_GUARD_FAILED");
	constexpr TCHAR ReplayUIGuardFailed[] = TEXT("REPLAY_UI_GUARD_FAILED");
	constexpr TCHAR ReplayUIWaitTimeout[] = TEXT("REPLAY_UI_WAIT_TIMEOUT");
}
