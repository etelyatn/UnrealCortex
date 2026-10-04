#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Templates/UniquePtr.h"

/**
 * Reentry and motion bookkeeping owned by one physical input session.
 *
 * SyntheticDepth is incremented around every generated event so this session's own capture
 * processor can prove the event is synthetic: it must be neither recorded nor treated as
 * human interference. ProcessedMotionGeneration advances only for pointer motion the session
 * actually routed through normal Slate routing, and is the freshness authority for
 * event-coordinate UI hits. OperationGeneration is paired with the owning operation so a
 * different synthetic caller cannot borrow this guard by claiming the same depth.
 */
struct FCortexEditorPhysicalInputDispatchContext
{
	/** >0 while this session is dispatching a generated event. */
	int32 SyntheticDepth = 0;
	/** Count of pointer-motion events this session processed through normal routing. */
	uint64 ProcessedMotionGeneration = 0;
	/** Generation of the owning operation; revalidated around every virtual dispatch. */
	uint64 OperationGeneration = 0;
};

/**
 * Constructs the engine input events a captured portable event replays as.
 *
 * All builders are pure: they never touch Slate, the world or the session, and they preserve
 * the recorded pressed-button set, modifiers, repeat flag, user and device.
 */
class FCortexEditorPhysicalInputEventBuilder
{
public:
	/** Builds the FKeyEvent for a KeyDown/KeyUp captured event. */
	static bool BuildKeyEvent(const FCortexEditorPhysicalInputEvent& Event,
		const FInputDeviceId& Device, int32 SlateUserIndex, FKeyEvent& OutEvent);

	/** Builds the FPointerEvent for a PointerMove against the previous viewport position. */
	static bool BuildPointerMoveEvent(const FCortexEditorPhysicalInputEvent& Event,
		const FInputDeviceId& Device, int32 SlateUserIndex,
		const TSet<FKey>& PressedButtons, const FVector2D& LastViewportPosition,
		FPointerEvent& OutEvent);

	/** Builds the FPointerEvent for a PointerDown/PointerUp/DoubleClick boundary. */
	static bool BuildPointerButtonEvent(const FCortexEditorPhysicalInputEvent& Event,
		const FInputDeviceId& Device, int32 SlateUserIndex,
		const TSet<FKey>& PressedButtons, FPointerEvent& OutEvent);

	/** Builds the FPointerEvent for a Wheel event. */
	static bool BuildWheelEvent(const FCortexEditorPhysicalInputEvent& Event,
		const FInputDeviceId& Device, int32 SlateUserIndex,
		const TSet<FKey>& PressedButtons, FPointerEvent& OutEvent);
};
