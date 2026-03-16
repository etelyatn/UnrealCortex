// Source/CortexQA/Private/Recording/CortexQARecorder.cpp
#include "Recording/CortexQARecorder.h"
#include "Recording/CortexQAInputRecorder.h"
#include "Recording/CortexQASessionSerializer.h"
#include "CortexQAModule.h"
#include "CortexCoreModule.h"
#include "CortexCoreDelegates.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Widgets/Notifications/SNotificationList.h"

FCortexQARecorder::FCortexQARecorder()
{
    InputRecorder = MakeShared<FCortexQAInputRecorder>();
}

FCortexQARecorder::~FCortexQARecorder()
{
    if (bIsRecording)
    {
        // Minimal cleanup only — do NOT call StopRecording() which fires delegates and saves to disk.
        bIsRecording = false;

        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(InputRecorder);
        }
        InputRecorder->StopRecording();

        FEditorDelegates::EndPIE.Remove(EndPIEHandle);

        if (UWorld* World = PIEWorldWeak.Get())
        {
            World->GetTimerManager().ClearTimer(PositionSampleTimer);
        }
    }
}

bool FCortexQARecorder::StartRecording(UWorld* PIEWorld, const FString& SessionName)
{
    if (bIsRecording)
    {
        UE_LOG(LogCortexQA, Warning, TEXT("Already recording — stop first"));
        return false;
    }

    if (PIEWorld == nullptr)
    {
        UE_LOG(LogCortexQA, Warning, TEXT("Cannot start recording: PIE world is null"));
        return false;
    }

    PIEWorldWeak = PIEWorld;
    CurrentSessionName = SessionName;
    RecordedSteps.Empty();
    RecordingStartTime = FPlatformTime::Seconds();
    LastRecordedPosition = FVector::ZeroVector;
    bIsRecording = true;

    // Register input processor
    if (!FSlateApplication::IsInitialized())
    {
        UE_LOG(LogCortexQA, Warning, TEXT("FSlateApplication not initialized — cannot record input"));
        bIsRecording = false;
        return false;
    }
    InputRecorder->StartRecording(RecordingStartTime);
    FSlateApplication::Get().RegisterInputPreProcessor(InputRecorder, 0);

    // Start position sampling timer (every 0.5s)
    PIEWorld->GetTimerManager().SetTimer(
        PositionSampleTimer,
        FTimerDelegate::CreateRaw(this, &FCortexQARecorder::SamplePosition),
        0.5f,
        true
    );

    // Bind PIE end delegate
    EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FCortexQARecorder::OnPIEEnded);

    UE_LOG(LogCortexQA, Log, TEXT("Recording started: %s"), *SessionName);
    return true;
}

FString FCortexQARecorder::StopRecording()
{
    if (!bIsRecording)
    {
        return FString();
    }

    bIsRecording = false;

    // Unregister input processor BEFORE stopping — prevents late Handle* callbacks
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().UnregisterInputPreProcessor(InputRecorder);
    }
    InputRecorder->StopRecording();

    // Clear timer
    UWorld* World = PIEWorldWeak.Get();
    if (World != nullptr)
    {
        World->GetTimerManager().ClearTimer(PositionSampleTimer);
    }

    // Unbind PIE delegate
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
    EndPIEHandle.Reset();

    // Build session info
    FCortexQASessionInfo Session;
    Session.Name = CurrentSessionName;
    Session.Source = TEXT("recorded");
    Session.RecordedAt = FDateTime::UtcNow();
    Session.DurationSeconds = FPlatformTime::Seconds() - RecordingStartTime;
    Session.bComplete = !bPartialRecording;
    bPartialRecording = false;
    Session.Steps = RecordedSteps;
    Session.RawInput = InputRecorder->GetRecordedEvents();

    if (World != nullptr)
    {
        Session.MapPath = World->GetMapName();
        // Strip PIE prefix if present
        Session.MapPath.RemoveFromStart(TEXT("UEDPIE_0_"));
    }

    // Save to disk
    FString OutPath;
    const FString Dir = FCortexQASessionSerializer::GetDefaultRecordingsDir();
    if (!FCortexQASessionSerializer::SaveSession(Session, Dir, OutPath))
    {
        UE_LOG(LogCortexQA, Warning, TEXT("Failed to save recording"));
        return FString();
    }

    // Fire progress delegate
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
    Progress->SetStringField(TEXT("type"), TEXT("session_saved"));
    Progress->SetStringField(TEXT("path"), OutPath);
    Progress->SetStringField(TEXT("name"), CurrentSessionName);
    Progress->SetStringField(TEXT("source"), TEXT("recorded"));
    Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);

    UE_LOG(LogCortexQA, Log, TEXT("Recording saved: %s (%d steps)"), *OutPath, RecordedSteps.Num());
    return OutPath;
}

void FCortexQARecorder::SamplePosition()
{
    UWorld* World = PIEWorldWeak.Get();
    if (World == nullptr || !bIsRecording)
    {
        return;
    }

    APlayerController* PC = World->GetFirstPlayerController();
    if (PC == nullptr)
    {
        return;
    }

    APawn* Pawn = PC->GetPawn();
    if (Pawn == nullptr)
    {
        return;
    }

    const FVector Pos = Pawn->GetActorLocation();
    const FRotator Rot = Pawn->GetActorRotation();

    // Skip if position hasn't changed meaningfully
    if (!LastRecordedPosition.IsZero() && FVector::Dist(Pos, LastRecordedPosition) < 10.0f)
    {
        return;
    }

    LastRecordedPosition = Pos;

    FCortexQAStep Step;
    Step.Type = TEXT("position_snapshot");
    Step.TimestampMs = (FPlatformTime::Seconds() - RecordingStartTime) * 1000.0;
    Step.Params = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> LocArr;
    LocArr.Add(MakeShared<FJsonValueNumber>(Pos.X));
    LocArr.Add(MakeShared<FJsonValueNumber>(Pos.Y));
    LocArr.Add(MakeShared<FJsonValueNumber>(Pos.Z));
    Step.Params->SetArrayField(TEXT("location"), LocArr);

    TArray<TSharedPtr<FJsonValue>> RotArr;
    RotArr.Add(MakeShared<FJsonValueNumber>(Rot.Pitch));
    RotArr.Add(MakeShared<FJsonValueNumber>(Rot.Yaw));
    RotArr.Add(MakeShared<FJsonValueNumber>(Rot.Roll));
    Step.Params->SetArrayField(TEXT("rotation"), RotArr);

    RecordedSteps.Add(MoveTemp(Step));

    // Fire progress for live ticker
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
    Progress->SetStringField(TEXT("type"), TEXT("recording_step"));
    Progress->SetNumberField(TEXT("step_index"), RecordedSteps.Num() - 1);
    Progress->SetStringField(TEXT("step_type"), TEXT("position_snapshot"));
    Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);
}

void FCortexQARecorder::DetectInteraction(const FString& KeyName)
{
    if (!bIsRecording)
    {
        return;
    }

    AActor* Target = RaycastInteractionTarget();
    if (Target == nullptr)
    {
        return;
    }

    FCortexQAStep Step;
    Step.Type = TEXT("interact");
    Step.TimestampMs = (FPlatformTime::Seconds() - RecordingStartTime) * 1000.0;
    Step.Params = MakeShared<FJsonObject>();
    Step.Params->SetStringField(TEXT("target"), Target->GetPathName());
    Step.Params->SetStringField(TEXT("key"), KeyName);
    RecordedSteps.Add(MoveTemp(Step));

    // Fire progress
    FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
    Progress->SetStringField(TEXT("type"), TEXT("recording_step"));
    Progress->SetNumberField(TEXT("step_index"), RecordedSteps.Num() - 1);
    Progress->SetStringField(TEXT("step_type"), TEXT("interact"));
    Progress->SetStringField(TEXT("target"), Target->GetActorNameOrLabel());
    Core.OnDomainProgress().Broadcast(FName(TEXT("qa")), Progress);
}

void FCortexQARecorder::AddAssertionCheckpoint()
{
    if (!bIsRecording)
    {
        return;
    }

    AActor* Target = RaycastInteractionTarget();
    if (Target == nullptr)
    {
        UE_LOG(LogCortexQA, Warning, TEXT("No actor under crosshair for assertion"));
        return;
    }

    // Capture common properties based on actor
    TArray<TPair<FString, TSharedPtr<FJsonValue>>> CapturedProps;

    // Always capture bHidden
    CapturedProps.Add(MakeTuple(
        FString(TEXT("bHidden")),
        TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Target->IsHidden()))
    ));

    // Check for door-like actors (has tag "Interactable" and "Door")
    if (Target->Tags.Contains(FName(TEXT("Interactable"))) && Target->Tags.Contains(FName(TEXT("Door"))))
    {
        // Try to read bIsOpen via reflection
        FBoolProperty* OpenProp = FindFProperty<FBoolProperty>(Target->GetClass(), TEXT("bIsOpen"));
        if (OpenProp != nullptr)
        {
            const bool bIsOpen = OpenProp->GetPropertyValue_InContainer(Target);
            CapturedProps.Add(MakeTuple(
                FString(TEXT("bIsOpen")),
                TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(bIsOpen))
            ));
        }

        FBoolProperty* LockedProp = FindFProperty<FBoolProperty>(Target->GetClass(), TEXT("bIsLocked"));
        if (LockedProp != nullptr)
        {
            const bool bIsLocked = LockedProp->GetPropertyValue_InContainer(Target);
            CapturedProps.Add(MakeTuple(
                FString(TEXT("bIsLocked")),
                TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(bIsLocked))
            ));
        }
    }

    // Create one assert step per captured property
    const FString ActorPath = Target->GetPathName();
    FString PropsText;

    for (const auto& Pair : CapturedProps)
    {
        FCortexQAStep Step;
        Step.Type = TEXT("assert");
        Step.TimestampMs = (FPlatformTime::Seconds() - RecordingStartTime) * 1000.0;
        Step.Params = MakeShared<FJsonObject>();
        Step.Params->SetStringField(TEXT("type"), TEXT("actor_property"));
        Step.Params->SetStringField(TEXT("actor"), ActorPath);
        Step.Params->SetStringField(TEXT("property"), Pair.Key);
        Step.Params->SetField(TEXT("value"), Pair.Value);
        Step.Params->SetStringField(TEXT("message"),
            FString::Printf(TEXT("%s should have %s matching recorded value"), *Target->GetActorNameOrLabel(), *Pair.Key));
        RecordedSteps.Add(MoveTemp(Step));

        // Build toast text
        FString ValStr;
        if (Pair.Value->Type == EJson::Boolean)
        {
            ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
        }
        else
        {
            ValStr = TEXT("...");
        }
        if (!PropsText.IsEmpty())
        {
            PropsText += TEXT(", ");
        }
        PropsText += FString::Printf(TEXT("%s=%s"), *Pair.Key, *ValStr);
    }

    ShowAssertionToast(Target->GetActorNameOrLabel(), PropsText);
}

AActor* FCortexQARecorder::RaycastInteractionTarget() const
{
    UWorld* World = PIEWorldWeak.Get();
    if (World == nullptr)
    {
        return nullptr;
    }

    APlayerController* PC = World->GetFirstPlayerController();
    if (PC == nullptr)
    {
        return nullptr;
    }

    FVector CamLoc;
    FRotator CamRot;
    PC->GetPlayerViewPoint(CamLoc, CamRot);

    const FVector TraceEnd = CamLoc + CamRot.Vector() * 1000.0f;

    FHitResult Hit;
    FCollisionQueryParams QueryParams;
    QueryParams.AddIgnoredActor(PC->GetPawn());

    if (World->LineTraceSingleByChannel(Hit, CamLoc, TraceEnd, ECC_Visibility, QueryParams))
    {
        return Hit.GetActor();
    }

    return nullptr;
}

void FCortexQARecorder::OnPIEEnded(bool bIsSimulating)
{
    if (!bIsRecording)
    {
        return;
    }

    UE_LOG(LogCortexQA, Warning, TEXT("PIE ended during recording — saving partial session"));
    bPartialRecording = true;
    StopRecording();
}

void FCortexQARecorder::ShowAssertionToast(const FString& ActorName, const FString& PropertiesText)
{
    FNotificationInfo Info(FText::FromString(
        FString::Printf(TEXT("Captured: %s — %s"), *ActorName, *PropertiesText)));
    Info.ExpireDuration = 3.0f;
    Info.bUseThrobber = false;
    FSlateNotificationManager::Get().AddNotification(Info);
}
