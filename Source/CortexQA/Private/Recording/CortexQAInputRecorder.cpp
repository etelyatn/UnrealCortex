// Source/CortexQA/Private/Recording/CortexQAInputRecorder.cpp
#include "Recording/CortexQAInputRecorder.h"
#include "HAL/PlatformTime.h"

void FCortexQAInputRecorder::StartRecording(double PIEStartTime)
{
    RecordedEvents.Empty();
    RecordingStartTime = PIEStartTime;
    bIsRecording = true;
}

void FCortexQAInputRecorder::StopRecording()
{
    bIsRecording = false;
}

double FCortexQAInputRecorder::GetRelativeTimestamp() const
{
    return (FPlatformTime::Seconds() - RecordingStartTime) * 1000.0;
}

bool FCortexQAInputRecorder::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
    if (!bIsRecording)
    {
        return false;
    }

    FCortexQARawInputEvent Evt;
    Evt.TimestampMs = GetRelativeTimestamp();
    Evt.Type = TEXT("key_down");
    Evt.Key = InKeyEvent.GetKey().ToString();
    Evt.Modifiers = static_cast<uint32>(InKeyEvent.IsShiftDown()) |
                    (static_cast<uint32>(InKeyEvent.IsControlDown()) << 1) |
                    (static_cast<uint32>(InKeyEvent.IsAltDown()) << 2) |
                    (static_cast<uint32>(InKeyEvent.IsCommandDown()) << 3);
    RecordedEvents.Add(MoveTemp(Evt));
    return false;
}

bool FCortexQAInputRecorder::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
    if (!bIsRecording)
    {
        return false;
    }

    FCortexQARawInputEvent Evt;
    Evt.TimestampMs = GetRelativeTimestamp();
    Evt.Type = TEXT("key_up");
    Evt.Key = InKeyEvent.GetKey().ToString();
    Evt.Modifiers = static_cast<uint32>(InKeyEvent.IsShiftDown()) |
                    (static_cast<uint32>(InKeyEvent.IsControlDown()) << 1) |
                    (static_cast<uint32>(InKeyEvent.IsAltDown()) << 2) |
                    (static_cast<uint32>(InKeyEvent.IsCommandDown()) << 3);
    RecordedEvents.Add(MoveTemp(Evt));
    return false;
}

bool FCortexQAInputRecorder::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
    if (!bIsRecording)
    {
        return false;
    }

    FCortexQARawInputEvent Evt;
    Evt.TimestampMs = GetRelativeTimestamp();
    Evt.Type = TEXT("mouse_move");
    Evt.DeltaX = MouseEvent.GetCursorDelta().X;
    Evt.DeltaY = MouseEvent.GetCursorDelta().Y;
    RecordedEvents.Add(MoveTemp(Evt));
    return false;
}

bool FCortexQAInputRecorder::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
    if (!bIsRecording)
    {
        return false;
    }

    FCortexQARawInputEvent Evt;
    Evt.TimestampMs = GetRelativeTimestamp();
    Evt.Type = TEXT("mouse_down");
    Evt.Key = MouseEvent.GetEffectingButton().ToString();
    RecordedEvents.Add(MoveTemp(Evt));
    return false;
}

bool FCortexQAInputRecorder::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
    if (!bIsRecording)
    {
        return false;
    }

    FCortexQARawInputEvent Evt;
    Evt.TimestampMs = GetRelativeTimestamp();
    Evt.Type = TEXT("mouse_up");
    Evt.Key = MouseEvent.GetEffectingButton().ToString();
    RecordedEvents.Add(MoveTemp(Evt));
    return false;
}
