#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPhysicalInputSession;
class UWorld;

struct FCortexEditorObservedWorldTick
{
	FName TickType = NAME_None;
	float RealDeltaSeconds = 0.0f;
	float DeltaSeconds = 0.0f;
	double RealTimeOffsetSeconds = 0.0;
	double TimeOffsetSeconds = 0.0;
	bool bPaused = false;
	float EffectiveTimeDilation = 1.0f;
};

struct FCortexEditorEngineFrameRecord
{
	uint64 CaptureFrameCounter = 0;
	double FrameBeginSeconds = 0.0;
	double InputBoundarySeconds = 0.0;
	double AppDeltaSeconds = 0.0;
	double AppCurrentSeconds = 0.0;
	double AppLastSeconds = 0.0;
	int32 SelectedWorldTickCount = 0;
	TOptional<FCortexEditorObservedWorldTick> WorldTick;
	bool bBoundaryObserved = false;
	bool bEndObserved = false;
};

struct FCortexEditorAppClockStep
{
	double CurrentSeconds = 0.0;
	double LastSeconds = 0.0;
	double DeltaSeconds = 0.0;
};

enum class ECortexEditorClockState : uint8
{
	Preparing,
	Replaying,
	Handoff,
	Released,
	Lost
};

/** Observes one native engine frame; installation precedes the owned PIE request. */
class CORTEXEDITOR_API FCortexEditorEngineFrameObserver
{
public:
	FCortexEditorEngineFrameObserver();
	~FCortexEditorEngineFrameObserver();

	FCortexEditorEngineFrameObserver(const FCortexEditorEngineFrameObserver&) = delete;
	FCortexEditorEngineFrameObserver& operator=(const FCortexEditorEngineFrameObserver&) = delete;

	FCortexCommandResult Install(FCortexEditorPhysicalInputSession& Session,
		TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnInputBoundary,
		TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnFrameClosed);
	void Uninstall();
	bool HasSelectedWorld() const;
	UWorld* GetSelectedWorld() const;
	const FCortexEditorEngineFrameRecord* GetLastClosedFrame() const;
	int32 GetSelectedWorldTickCount() const;
	FCortexCommandResult RunSlateOnlyWaitWork();

private:
	struct FState;
	TSharedPtr<FState> State;
};
