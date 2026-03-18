// Source/CortexQA/Private/Recording/CortexQASessionTypes.h
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Recording/replay session state */
enum class ECortexQASessionState : uint8
{
	Idle,
	Recording,
	Replaying
};

/** On-failure policy for replay */
enum class EQAReplayOnFailure : uint8
{
	Continue,
	Stop
};

/** Replay mode for position_snapshot steps */
enum class EQAReplayMode : uint8
{
	Smooth,    // Walk between positions using move_to (realistic, slower)
	Teleport   // Teleport with timing delays (fast, exact positions)
};

/** A single raw input event captured during recording */
struct FCortexQARawInputEvent
{
	double TimestampMs = 0.0;
	FString Type;       // "key_down", "key_up", "mouse_move", "mouse_down", "mouse_up"
	FString Key;        // For key events
	float DeltaX = 0.f; // For mouse move
	float DeltaY = 0.f;
	uint32 Modifiers = 0;
};

/** A single semantic step in a QA session */
struct FCortexQAStep
{
	FString Type;       // "position_snapshot", "move_to", "interact", "look_at", "wait", "assert", "key_press"
	TSharedPtr<FJsonObject> Params;
	double TimestampMs = 0.0;
};

/** Result of a single replay step */
struct FCortexQAStepResult
{
	int32 StepIndex = 0;
	FString StepType;
	bool bPassed = false;
	FString ErrorMessage;
	double DurationMs = 0.0;
};

/** Last run result stored in session JSON */
struct FCortexQALastRun
{
	FDateTime Timestamp;
	bool bPassed = false;
	int32 StepsPassed = 0;
	int32 StepsFailed = 0;
	double DurationSeconds = 0.0;
};

/** Full session info (metadata + steps) */
struct FCortexQASessionInfo
{
	int32 Version = 1;
	FString Name;
	FString Source;         // "recorded" or "ai_generated"
	FDateTime RecordedAt;
	FString MapPath;
	double DurationSeconds = 0.0;
	bool bComplete = true;
	TArray<FCortexQAStep> Steps;
	TArray<FCortexQARawInputEvent> RawInput;
	TOptional<FCortexQALastRun> LastRun;
	TArray<TSharedPtr<FJsonObject>> ConversationHistory;

	/** Disk path where this session is saved */
	FString FilePath;
};
