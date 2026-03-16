// Source/CortexQA/Private/Recording/CortexQAInputRecorder.h
#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Recording/CortexQASessionTypes.h"

/**
 * Captures raw Slate input events during recording.
 * Registered as an input pre-processor; all Handle* methods return false (passthrough).
 */
class FCortexQAInputRecorder : public IInputProcessor
{
public:
    void StartRecording(double PIEStartTime);
    void StopRecording();
    bool IsRecording() const { return bIsRecording; }
    const TArray<FCortexQARawInputEvent>& GetRecordedEvents() const { return RecordedEvents; }

    // IInputProcessor interface
    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
    virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;

private:
    double GetRelativeTimestamp() const;

    // All Handle* callbacks run on Game Thread (Slate tick). No lock needed.
    bool bIsRecording = false;
    double RecordingStartTime = 0.0;
    TArray<FCortexQARawInputEvent> RecordedEvents;
};
