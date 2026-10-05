#include "CortexEditorPhysicalInputCapture.h"

#include "CortexEditorPhysicalInputSession.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"

FCortexEditorPhysicalInputCaptureProcessor::FCortexEditorPhysicalInputCaptureProcessor(
	FCortexEditorPhysicalInputSession& InSession)
	: Session(InSession)
{
}

FCortexEditorPhysicalInputCaptureProcessor::~FCortexEditorPhysicalInputCaptureProcessor() = default;

// The processor is passive for every event: Slate preprocessors are consulted in bucket order and
// this one only observes the selected user/device, so it must never consume or alter real input.
void FCortexEditorPhysicalInputCaptureProcessor::Tick(const float /*DeltaTime*/,
	FSlateApplication& /*SlateApp*/, TSharedRef<ICursor> /*Cursor*/)
{
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleKeyDownEvent(FSlateApplication& /*SlateApp*/,
	const FKeyEvent& InKeyEvent)
{
	Session.ObserveProcessorKey(InKeyEvent, true);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleKeyUpEvent(FSlateApplication& /*SlateApp*/,
	const FKeyEvent& InKeyEvent)
{
	Session.ObserveProcessorKey(InKeyEvent, false);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleMouseMoveEvent(FSlateApplication& /*SlateApp*/,
	const FPointerEvent& MouseEvent)
{
	Session.ObserveProcessorMouseMove(MouseEvent);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleMouseButtonDownEvent(FSlateApplication& /*SlateApp*/,
	const FPointerEvent& MouseEvent)
{
	Session.ObserveProcessorMouseButton(MouseEvent, ECortexEditorPhysicalInputKind::PointerDown);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleMouseButtonUpEvent(FSlateApplication& /*SlateApp*/,
	const FPointerEvent& MouseEvent)
{
	Session.ObserveProcessorMouseButton(MouseEvent, ECortexEditorPhysicalInputKind::PointerUp);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleMouseButtonDoubleClickEvent(
	FSlateApplication& /*SlateApp*/, const FPointerEvent& MouseEvent)
{
	Session.ObserveProcessorMouseButton(MouseEvent, ECortexEditorPhysicalInputKind::DoubleClick);
	return false;
}

bool FCortexEditorPhysicalInputCaptureProcessor::HandleMouseWheelOrGestureEvent(
	FSlateApplication& /*SlateApp*/, const FPointerEvent& InWheelEvent, const FPointerEvent* /*InGestureEvent*/)
{
	Session.ObserveProcessorMouseWheel(InWheelEvent);
	return false;
}

const TCHAR* FCortexEditorPhysicalInputCaptureProcessor::GetDebugName() const
{
	return TEXT("CortexPhysicalInputCapture");
}

const TSet<FKey>& FCortexEditorPhysicalInputCaptureProcessor::GetObservedHeldKeys() const
{
	static const TSet<FKey> Empty;
	return Session.CaptureState.IsValid() ? Session.CaptureState->ObservedHeldKeys : Empty;
}

const TSet<FKey>& FCortexEditorPhysicalInputCaptureProcessor::GetObservedHeldButtons() const
{
	static const TSet<FKey> Empty;
	return Session.CaptureState.IsValid() ? Session.CaptureState->ObservedHeldButtons : Empty;
}

const TSet<FKey>& FCortexEditorPhysicalInputCaptureProcessor::GetObservedHeldModifiers() const
{
	static const TSet<FKey> Empty;
	return Session.CaptureState.IsValid() ? Session.CaptureState->ObservedModifierKeys : Empty;
}
