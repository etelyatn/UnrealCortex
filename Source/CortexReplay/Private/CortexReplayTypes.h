#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexTypes.h"

enum class ECortexReplayOrigin : uint8 { Human, AI };

enum class ECortexReplayState : uint8
{
	Preparing, Recording, Replaying, Finalizing,
	Completed, Cancelled, Interrupted, Error
};

/** Viewport input state the human had at the capture epoch (replay compares it before the epoch). */
struct FCortexReplayViewportState
{
	bool bMouseCapture = false;
	bool bHighPrecision = false;
};

/** Recording-side format version (format 4: compact rows, gzip streams). */
constexpr int32 CortexReplayRecordingFormatVersion = 4;

struct FCortexReplayInitialState
{
	int32 SchemaVersion = CortexReplayRecordingFormatVersion;
	int32 RecordingId = 0;
	FString PawnClassPath;
	FCortexEditorPhysicalInputPlayerPose Pose;
	FCortexReplayViewportState Viewport;
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

struct FCortexReplayEvent
{
	int32 Sequence = 0;
	FCortexEditorPhysicalInputEvent Input;
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

/** Largest per-frame application delta a recording may carry (also bounds replay pacing sleeps). */
constexpr double CortexReplayMaxFrameDeltaSeconds = 10.0;

/** Frame-ordinal timing identity of a format-3 recording. */
struct FCortexReplayTiming
{
	int32 FrameCount = 0;
};

/** World tick the consuming engine frame ran: the real delta handed to UWorld::Tick and the pause state. */
struct FCortexReplayFrameTick
{
	float RealDeltaSeconds = 0.0f;
	bool bPaused = false;
};

/**
 * One engine frame that consumed input after the capture epoch, indexed from zero. Every input
 * event belongs to exactly one frame. A frame with no observed world tick leaves WorldTick unset
 * (distinct from a zero-delta tick); AppDeltaSeconds is always in (0, CortexReplayMaxFrameDeltaSeconds].
 */
struct FCortexReplayFrame
{
	int32 FrameIndex = 0;
	double AppDeltaSeconds = 0.0;
	TOptional<FCortexReplayFrameTick> WorldTick;
	int32 FirstSequence = 0;
	int32 EventCount = 0;
};

struct FCortexReplayMetadata
{
	int32 SchemaVersion = CortexReplayRecordingFormatVersion;
	int32 RecordingId = 0;
	FString Name, Description, MapAssetPath, EngineVersion, PluginVersion;
	FDateTime CreatedAtUtc;
	double DurationSeconds = 0.0;
	bool bAIEnabled = false;
	/** Whether a replay of this recording shows the 3-2-1 hands-off countdown first. Human-edited. */
	bool bCountdownEnabled = true;
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

/**
 * One library directory as listed without loading its payload. Metadata is set when metadata.json
 * parses; otherwise LoadError explains why the recording cannot be used.
 */
struct FCortexReplayLibraryEntry
{
	int32 RecordingId = 0;
	TOptional<FCortexReplayMetadata> Metadata;
	FCortexCommandResult LoadError;
};
