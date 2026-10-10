#pragma once

#include "CoreMinimal.h"
#include "CortexEditorEngineFrameObserver.h"
#include "CortexEditorPhysicalInput.h"

enum class ECortexReplayOrigin : uint8 { Human, AI };

enum class ECortexReplayState : uint8
{
	Preparing, Recording, Replaying, Finalizing,
	Completed, Cancelled, Interrupted, Error
};

struct FCortexReplayInitialState
{
	int32 SchemaVersion = 2;
	int32 RecordingId = 0;
	FString PawnClassPath;
	FCortexEditorPhysicalInputPlayerPose Pose;
};

struct FCortexReplayGuardCoverage
{
	int32 PosePresses = 0;
	int32 UISupportedPresses = 0;
	int32 UIUnavailablePresses = 0;
	int32 UINotApplicablePresses = 0;
};

struct FCortexReplayInteractionGuard
{
	FCortexEditorPhysicalInputPlayerPose ExpectedPose;
	ECortexEditorUICoverage UICoverage = ECortexEditorUICoverage::NotApplicable;
	ECortexEditorUIUnavailableReason UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> UITarget;
	FVector2D ExpectedLocalPosition = FVector2D::ZeroVector;
};

struct FCortexReplayCaptureDiagnostics
{
	uint64 FrameNumber = 0;
	double WorldTimeSeconds = 0.0;
	bool bWorldPaused = false;
	bool bTargetOwnsPointerCapture = false;
};

struct FCortexReplayEvent
{
	int32 Sequence = 0;
	double TimeSeconds = 0.0;
	FCortexEditorPhysicalInputEvent Input;
	FCortexReplayCaptureDiagnostics CaptureContext;
	TOptional<FCortexReplayInteractionGuard> Guard;
};

/** Outcome of a frame preparation poll. */
enum class ECortexReplayFramePreparation : uint8
{
	/** The requested frame's whole event range is prepared and ready to commit. */
	Ready,
	/** The frame is not yet fully prepared (for example a permitted UI wait is pending). */
	Waiting
};

/** Frame-ordinal timing identity of a format-2 recording. */
struct FCortexReplayTiming
{
	int32 FrameCount = 0;
	int32 InputEpochFrame = INDEX_NONE;
};

/**
 * One recorded frame. Every input event belongs to exactly one frame; a warmup frame admits no
 * input (EventCount == 0). Application offsets are relative to the first owned frame boundary, and
 * a frame with no observed world tick leaves WorldTick unset (distinct from a zero-delta tick).
 */
struct FCortexReplayFrame
{
	int32 FrameIndex = 0;
	double InputDeadlineSeconds = 0.0;
	double AppDeltaSeconds = 0.0;
	double AppCurrentOffsetSeconds = 0.0;
	double AppLastOffsetSeconds = 0.0;
	TOptional<FCortexEditorObservedWorldTick> WorldTick;
	int32 FirstSequence = 0;
	int32 EventCount = 0;
	uint64 CaptureFrame = 0;
	double FrameBeginSeconds = 0.0;
};

struct FCortexReplayMetadata
{
	int32 SchemaVersion = 2;
	int32 RecordingId = 0;
	FString Name, Description, MapAssetPath, EngineVersion, PluginVersion;
	FDateTime CreatedAtUtc;
	double DurationSeconds = 0.0;
	bool bAIEnabled = false;
	bool bComplete = false;
	FCortexEditorPhysicalInputTargetInfo Prerequisites;
	FCortexReplayGuardCoverage GuardCoverage;
	FString InitialStateSha256, InputsSha256, FramesSha256;
	FCortexReplayTiming Timing;
};

struct FCortexReplaySnapshot
{
	FCortexReplayMetadata Metadata;
	FCortexReplayInitialState InitialState;
	TArray<FCortexReplayEvent> Events;
	TArray<FCortexReplayFrame> Frames;
	FString RecordingSnapshotSha256;
};
