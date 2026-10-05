#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"

enum class ECortexReplayOrigin : uint8 { Human, AI };

enum class ECortexReplayState : uint8
{
	Preparing, Recording, Replaying, Finalizing,
	Completed, Cancelled, Interrupted, Error
};

struct FCortexReplayInitialState
{
	int32 SchemaVersion = 1;
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

struct FCortexReplayMetadata
{
	int32 SchemaVersion = 1;
	int32 RecordingId = 0;
	FString Name, Description, MapAssetPath, EngineVersion, PluginVersion;
	FDateTime CreatedAtUtc;
	double DurationSeconds = 0.0;
	bool bAIEnabled = false;
	bool bComplete = false;
	FCortexEditorPhysicalInputTargetInfo Prerequisites;
	FCortexReplayGuardCoverage GuardCoverage;
	FString InitialStateSha256, InputsSha256;
};

struct FCortexReplaySnapshot
{
	FCortexReplayMetadata Metadata;
	FCortexReplayInitialState InitialState;
	TArray<FCortexReplayEvent> Events;
	FString RecordingSnapshotSha256;
};
