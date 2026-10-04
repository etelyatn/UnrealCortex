#include "CortexEditorPhysicalInputDispatch.h"

#include "Application/SlateApplicationBase.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"

bool FCortexEditorPhysicalInputEventBuilder::BuildKeyEvent(const FCortexEditorPhysicalInputEvent& Event,
	const FInputDeviceId& Device, int32 SlateUserIndex, FKeyEvent& OutEvent)
{
	if (!Event.Key.IsValid())
	{
		return false;
	}
	OutEvent = FKeyEvent(Event.Key, Event.Modifiers, Device, Event.bRepeat, 0, 0,
		TOptional<int32>(SlateUserIndex));
	return true;
}

bool FCortexEditorPhysicalInputEventBuilder::BuildPointerMoveEvent(
	const FCortexEditorPhysicalInputEvent& Event, const FInputDeviceId& Device, int32 SlateUserIndex,
	const TSet<FKey>& PressedButtons, const FVector2D& LastViewportPosition, FPointerEvent& OutEvent)
{
	OutEvent = FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex,
		Event.ViewportPosition, LastViewportPosition, PressedButtons, EKeys::Invalid, 0.0f,
		Event.Modifiers, TOptional<int32>(SlateUserIndex));
	return true;
}

bool FCortexEditorPhysicalInputEventBuilder::BuildPointerButtonEvent(
	const FCortexEditorPhysicalInputEvent& Event, const FInputDeviceId& Device, int32 SlateUserIndex,
	const TSet<FKey>& PressedButtons, FPointerEvent& OutEvent)
{
	if (!Event.Key.IsValid())
	{
		return false;
	}
	OutEvent = FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex,
		Event.ViewportPosition, Event.ViewportPosition, PressedButtons, Event.Key, 0.0f,
		Event.Modifiers, TOptional<int32>(SlateUserIndex));
	return true;
}

bool FCortexEditorPhysicalInputEventBuilder::BuildWheelEvent(
	const FCortexEditorPhysicalInputEvent& Event, const FInputDeviceId& Device, int32 SlateUserIndex,
	const TSet<FKey>& PressedButtons, FPointerEvent& OutEvent)
{
	OutEvent = FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex,
		Event.ViewportPosition, Event.ViewportPosition, PressedButtons, EKeys::Invalid,
		Event.WheelDelta, Event.Modifiers, TOptional<int32>(SlateUserIndex));
	return true;
}
