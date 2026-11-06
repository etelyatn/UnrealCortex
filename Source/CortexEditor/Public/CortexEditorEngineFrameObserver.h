#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPhysicalInputSession;

/** The owned world tick an engine frame ran: the real delta UWorld::Tick received and its pause state. */
struct FCortexEditorObservedWorldTick
{
	float RealDeltaSeconds = 0.0f;
	bool bPaused = false;
};

/** What the observer saw of one engine frame of the owned PIE session. */
struct FCortexEditorEngineFrameRecord
{
	uint64 CaptureFrameCounter = 0;
	/** FApp's delta time sampled at the input boundary: the step GEngine->Tick receives this frame. */
	double AppDeltaSeconds = 0.0;
	/** -1 once the frame was invalidated, otherwise the owned world ticks seen so far (0 or 1). */
	int32 SelectedWorldTickCount = 0;
	TOptional<FCortexEditorObservedWorldTick> WorldTick;
	/** True once OnSamplingInput ran for this frame; reset when the frame is invalidated. */
	bool bBoundaryObserved = false;
};

/** Observes one native engine frame; installation precedes the owned PIE request. */
class CORTEXEDITOR_API FCortexEditorEngineFrameObserver
{
public:
	FCortexEditorEngineFrameObserver();
	~FCortexEditorEngineFrameObserver();

	FCortexEditorEngineFrameObserver(const FCortexEditorEngineFrameObserver&) = delete;
	FCortexEditorEngineFrameObserver& operator=(const FCortexEditorEngineFrameObserver&) = delete;

	/**
	 * OnInputBoundary runs at OnSamplingInput, after the frame's input was pumped and before the
	 * world ticks; OnFrameClosed runs at OnEndFrame with the finished record, which is invalid
	 * (bBoundaryObserved false, SelectedWorldTickCount -1) when anything about the frame was foreign.
	 */
	FCortexCommandResult Install(FCortexEditorPhysicalInputSession& Session,
		TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnInputBoundary,
		TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnFrameClosed);
	void Uninstall();

	/**
	 * The engine frame in flight once its input boundary was observed and it is still valid, or
	 * null before the boundary, after it closed, or once it was invalidated.
	 */
	const FCortexEditorEngineFrameRecord* GetOpenFrame() const;

private:
	struct FState;
	TSharedPtr<FState> State;
};
