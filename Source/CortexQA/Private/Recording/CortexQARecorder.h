// Source/CortexQA/Private/Recording/CortexQARecorder.h
#pragma once

#include "CoreMinimal.h"
#include "Recording/CortexQASessionTypes.h"

class FCortexQAInputRecorder;

/**
 * Orchestrates a full recording session: input capture, position sampling,
 * interaction detection, and assertion hotkeys.
 */
class FCortexQARecorder
{
public:
    FCortexQARecorder();
    ~FCortexQARecorder();

    /**
     * Start recording in the given PIE world.
     * @param PIEWorld  Active PIE world (null = error).
     * @param SessionName  User-provided name.
     * @return true if recording started.
     */
    bool StartRecording(UWorld* PIEWorld, const FString& SessionName);

    /**
     * Stop recording and save session to disk.
     * @return Saved file path, or empty string on failure.
     */
    FString StopRecording();

    /** Add an assertion checkpoint for the actor currently under the crosshair. */
    void AddAssertionCheckpoint();

    bool IsRecording() const { return bIsRecording; }

    /** Get the steps captured so far (for live ticker in UI). */
    const TArray<FCortexQAStep>& GetRecordedSteps() const { return RecordedSteps; }

private:
    void SamplePosition();
    void DetectInteraction(const FString& KeyName);
    AActor* RaycastInteractionTarget() const;
    void OnPIEEnded(bool bIsSimulating);
    void ShowAssertionToast(const FString& ActorName, const FString& PropertiesText);

    bool bIsRecording = false;
    bool bPartialRecording = false; // Set by OnPIEEnded before StopRecording
    FString CurrentSessionName;
    double RecordingStartTime = 0.0;

    TSharedPtr<FCortexQAInputRecorder> InputRecorder;
    TArray<FCortexQAStep> RecordedSteps;
    FVector LastRecordedPosition = FVector::ZeroVector;

    TWeakObjectPtr<UWorld> PIEWorldWeak;
    FTimerHandle PositionSampleTimer;
    FDelegateHandle EndPIEHandle;
    FDelegateHandle InputRecorderRegistration;
};
