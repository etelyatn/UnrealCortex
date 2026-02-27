#include "Operations/CortexQAActionOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAConditionUtils.h"
#include "CortexQAUtils.h"
#include "CortexTypes.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "NavigationSystem.h"

namespace
{
    bool TryParseTarget(const TSharedPtr<FJsonObject>& Params, UWorld* World, FVector& OutTarget, FCortexCommandResult& OutError)
    {
        if (!Params.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
            return false;
        }

        const TSharedPtr<FJsonValue> TargetValue = Params->TryGetField(TEXT("target"));
        if (!TargetValue.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: target"));
            return false;
        }

        if (TargetValue->Type == EJson::String)
        {
            const FString ActorId = TargetValue->AsString();
            AActor* Actor = FCortexQAUtils::FindActorByName(World, ActorId);
            if (Actor == nullptr)
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::ActorNotFound,
                    FString::Printf(TEXT("Actor not found: %s"), *ActorId));
                return false;
            }
            OutTarget = Actor->GetActorLocation();
            return true;
        }

        FString ParseError;
        FVector ParsedTarget = FVector::ZeroVector;
        if (FCortexQAUtils::ParseVectorParam(TargetValue, ParsedTarget, ParseError))
        {
            OutTarget = ParsedTarget;
            return true;
        }

        OutError = FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("target must be actor name or vector object/array: %s"), *ParseError));
        return false;
    }
}

FCortexCommandResult FCortexQAActionOps::LookAt(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller or pawn"));
    }

    FVector TargetLocation = FVector::ZeroVector;
    FCortexCommandResult ParseError;
    if (!TryParseTarget(Params, PIEWorld, TargetLocation, ParseError))
    {
        return ParseError;
    }

    const FVector Direction = TargetLocation - Pawn->GetActorLocation();
    const FRotator LookRotation = Direction.Rotation();
    PC->SetControlRotation(LookRotation);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    FCortexQAUtils::SetRotatorObject(Data, TEXT("control_rotation"), LookRotation);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAActionOps::LookTo(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller or pawn"));
    }

    double Yaw = 0.0;
    if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("yaw"), Yaw))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: yaw"));
    }

    double Pitch = 0.0;
    double Duration = 0.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("pitch"), Pitch);
        Params->TryGetNumberField(TEXT("duration"), Duration);
    }

    const FRotator TargetRotation(Pitch, Yaw, 0.0);
    if (Duration <= 0.0)
    {
        PC->SetControlRotation(TargetRotation);

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        FCortexQAUtils::SetVectorObject(Data, TEXT("location"), Pawn->GetActorLocation());
        FCortexQAUtils::SetRotatorObject(Data, TEXT("rotation"), PC->GetControlRotation());
        Data->SetNumberField(TEXT("yaw"), Yaw);
        Data->SetNumberField(TEXT("pitch"), Pitch);
        Data->SetNumberField(TEXT("duration"), 0.0);
        Data->SetBoolField(TEXT("interpolated"), false);
        return FCortexCommandRouter::Success(Data);
    }

    if (!DeferredCallback)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Deferred callback is required"));
    }

    const FRotator StartRotation = PC->GetControlRotation();
    const float YawDelta = FMath::Abs(FMath::FindDeltaAngleDegrees(StartRotation.Yaw, TargetRotation.Yaw));
    const float PitchDelta = FMath::Abs(FMath::FindDeltaAngleDegrees(StartRotation.Pitch, TargetRotation.Pitch));
    const float MaxAngularDelta = FMath::Max(YawDelta, PitchDelta);
    const float InterpSpeed = MaxAngularDelta / static_cast<float>(Duration);
    const double StartGameTime = PIEWorld->GetTimeSeconds();

    TWeakObjectPtr<APlayerController> WeakPC = PC;
    TWeakObjectPtr<APawn> WeakPawn = Pawn;
    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, WeakPC, WeakPawn, TargetRotation, InterpSpeed, Duration, StartGameTime, DeferredCallback = MoveTemp(DeferredCallback)]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during look_to"));
                DeferredCallback(MoveTemp(Final));
                return;
            }

            UWorld* CurrentPIEWorld = GEditor->PlayWorld;
            APlayerController* CurrentPC = WeakPC.Get();
            APawn* CurrentPawn = WeakPawn.Get();
            if (CurrentPC == nullptr || CurrentPawn == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Player controller or pawn no longer valid"));
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            const FRotator Current = CurrentPC->GetControlRotation();
            const FRotator Next = FMath::RInterpConstantTo(Current, TargetRotation, 0.05f, InterpSpeed);
            CurrentPC->SetControlRotation(Next);

            const double Elapsed = CurrentPIEWorld->GetTimeSeconds() - StartGameTime;
            if (Next.Equals(TargetRotation, 1.0f) || Elapsed >= Duration)
            {
                CurrentPC->SetControlRotation(TargetRotation);

                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                FCortexQAUtils::SetVectorObject(Data, TEXT("location"), CurrentPawn->GetActorLocation());
                FCortexQAUtils::SetRotatorObject(Data, TEXT("rotation"), CurrentPC->GetControlRotation());
                Data->SetNumberField(TEXT("yaw"), TargetRotation.Yaw);
                Data->SetNumberField(TEXT("pitch"), TargetRotation.Pitch);
                Data->SetNumberField(TEXT("duration"), Elapsed);
                Data->SetBoolField(TEXT("interpolated"), true);

                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
            }
        },
        0.05f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}

FCortexCommandResult FCortexQAActionOps::CheckStuck(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr || PC->PlayerCameraManager == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller, pawn, or camera manager"));
    }

    double Duration = 0.5;
    double Threshold = 10.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("duration"), Duration);
        Params->TryGetNumberField(TEXT("threshold"), Threshold);
    }
    Duration = FMath::Max(0.0, Duration);
    Threshold = FMath::Max(0.0, Threshold);

    const FVector StartLocation = Pawn->GetActorLocation();
    auto BuildResult = [StartLocation, Threshold](UWorld* World, APlayerController* CurrentPC, APawn* CurrentPawn, double DurationSeconds)
    {
        const FVector EndLocation = CurrentPawn->GetActorLocation();
        const FVector Velocity = CurrentPawn->GetVelocity();
        const double DistanceMoved = FVector::Dist(StartLocation, EndLocation);
        const bool bIsStuck = DistanceMoved <= Threshold;

        UCharacterMovementComponent* Movement = CurrentPawn->FindComponentByClass<UCharacterMovementComponent>();
        const bool bIsFalling = Movement != nullptr && Movement->IsFalling();

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("is_stuck"), bIsStuck);
        Data->SetNumberField(TEXT("distance_moved"), DistanceMoved);
        Data->SetNumberField(TEXT("threshold"), Threshold);
        Data->SetNumberField(TEXT("duration"), DurationSeconds);
        FCortexQAUtils::SetVectorObject(Data, TEXT("start_location"), StartLocation);
        FCortexQAUtils::SetVectorObject(Data, TEXT("end_location"), EndLocation);
        FCortexQAUtils::SetVectorObject(Data, TEXT("velocity"), Velocity);
        Data->SetNumberField(TEXT("speed"), Velocity.Size());
        Data->SetBoolField(TEXT("is_falling"), bIsFalling);

        if (!bIsStuck)
        {
            Data->SetField(TEXT("obstruction"), MakeShared<FJsonValueNull>());
            return FCortexCommandRouter::Success(Data);
        }

        const FVector TraceStart = CurrentPC->PlayerCameraManager->GetCameraLocation();
        const FVector TraceForward = CurrentPC->PlayerCameraManager->GetCameraRotation().Vector();
        const FVector TraceEnd = TraceStart + (TraceForward * 500.0f);

        FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CortexQACheckStuck), true);
        TraceParams.AddIgnoredActor(CurrentPawn);
        TraceParams.AddIgnoredActor(CurrentPC->PlayerCameraManager);

        FHitResult Hit;
        const bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams);
        if (!bHit || !Hit.bBlockingHit)
        {
            Data->SetField(TEXT("obstruction"), MakeShared<FJsonValueNull>());
            return FCortexCommandRouter::Success(Data);
        }

        TSharedPtr<FJsonObject> Obstruction = MakeShared<FJsonObject>();
        Obstruction->SetBoolField(TEXT("hit"), true);
        Obstruction->SetNumberField(TEXT("distance"), Hit.Distance);
        FCortexQAUtils::SetVectorObject(Obstruction, TEXT("location"), Hit.ImpactPoint);

        AActor* HitActor = Hit.GetActor();
        if (HitActor != nullptr)
        {
            Obstruction->SetStringField(TEXT("hit_type"), TEXT("actor"));
            TSharedPtr<FJsonObject> ActorData = MakeShared<FJsonObject>();
            ActorData->SetStringField(TEXT("name"), HitActor->GetName());
            ActorData->SetStringField(TEXT("path"), HitActor->GetPathName());
            Obstruction->SetObjectField(TEXT("actor"), ActorData);
        }
        else
        {
            Obstruction->SetStringField(TEXT("hit_type"), TEXT("geometry"));
            Obstruction->SetField(TEXT("actor"), MakeShared<FJsonValueNull>());
        }

        Data->SetObjectField(TEXT("obstruction"), Obstruction);
        return FCortexCommandRouter::Success(Data);
    };

    if (Duration <= 0.0)
    {
        return BuildResult(PIEWorld, PC, Pawn, 0.0);
    }

    if (!DeferredCallback)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Deferred callback is required"));
    }

    TWeakObjectPtr<APlayerController> WeakPC = PC;
    TWeakObjectPtr<APawn> WeakPawn = Pawn;
    const double StartGameTime = PIEWorld->GetTimeSeconds();
    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, BuildResult, WeakPC, WeakPawn, StartGameTime, Duration, DeferredCallback = MoveTemp(DeferredCallback)]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during check_stuck"));
                DeferredCallback(MoveTemp(Final));
                return;
            }

            UWorld* CurrentPIEWorld = GEditor->PlayWorld;
            const double Elapsed = CurrentPIEWorld->GetTimeSeconds() - StartGameTime;
            if (Elapsed < Duration)
            {
                return;
            }

            APlayerController* CurrentPC = WeakPC.Get();
            APawn* CurrentPawn = WeakPawn.Get();
            if (CurrentPC == nullptr || CurrentPawn == nullptr || CurrentPC->PlayerCameraManager == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Player controller or pawn no longer valid"));
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            FCortexCommandResult Final = BuildResult(CurrentPIEWorld, CurrentPC, CurrentPawn, Elapsed);
            DeferredCallback(MoveTemp(Final));
            CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
        },
        0.05f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}

FCortexCommandResult FCortexQAActionOps::Interact(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    if (!DeferredCallback)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Deferred callback is required"));
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    if (PC == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller"));
    }

    FString KeyName = TEXT("E");
    double DurationSeconds = 0.1;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("key"), KeyName);
        Params->TryGetNumberField(TEXT("duration"), DurationSeconds);
    }
    DurationSeconds = FMath::Clamp(DurationSeconds, 0.01, 30.0);

    const FKey InteractionKey(*KeyName);
    if (!InteractionKey.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Invalid key name"));
    }

    PC->InputKey(FInputKeyEventArgs::CreateSimulated(InteractionKey, EInputEvent::IE_Pressed, 1.0f));

    TWeakObjectPtr<APlayerController> WeakPC = PC;
    const double StartGameTime = PIEWorld->GetTimeSeconds();
    TSharedPtr<FTimerHandle> HoldHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *HoldHandle,
        [HoldHandle, WeakPC, InteractionKey, KeyName, DurationSeconds, StartGameTime, DeferredCallback = MoveTemp(DeferredCallback)]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(
                    CortexErrorCodes::PIETerminated, TEXT("PIE terminated during interact hold"));
                DeferredCallback(MoveTemp(Final));
                return;
            }

            UWorld* CurrentPIEWorld = GEditor->PlayWorld;
            const double Elapsed = CurrentPIEWorld->GetTimeSeconds() - StartGameTime;

            if (Elapsed >= DurationSeconds)
            {
                if (APlayerController* ReleasedPC = WeakPC.Get())
                {
                    ReleasedPC->InputKey(FInputKeyEventArgs::CreateSimulated(InteractionKey, EInputEvent::IE_Released, 0.0f));
                }

                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("success"), true);
                Data->SetStringField(TEXT("key"), KeyName);
                Data->SetNumberField(TEXT("duration"), Elapsed);
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*HoldHandle);
            }
        },
        0.05f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}

FCortexCommandResult FCortexQAActionOps::MoveTo(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    if (!DeferredCallback)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Deferred callback is required"));
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller or pawn"));
    }

    FVector TargetLocation = FVector::ZeroVector;
    FCortexCommandResult ParseError;
    if (!TryParseTarget(Params, PIEWorld, TargetLocation, ParseError))
    {
        return ParseError;
    }

    double TimeoutSeconds = 10.0;
    double AcceptanceRadius = 75.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("timeout"), TimeoutSeconds);
        Params->TryGetNumberField(TEXT("acceptance_radius"), AcceptanceRadius);
    }

    const double StartGameTime = PIEWorld->GetTimeSeconds();
    bool bUseNavigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(PIEWorld) != nullptr;
    FString MovementMethod = bUseNavigation ? TEXT("navigation") : TEXT("direct");
    if (bUseNavigation)
    {
        UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, TargetLocation);
    }

    TWeakObjectPtr<APawn> WeakPawn = Pawn;
    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, DeferredCallback = MoveTemp(DeferredCallback), TargetLocation, AcceptanceRadius, TimeoutSeconds, StartGameTime, WeakPawn, MovementMethod, bUseNavigation]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during move_to"));
                DeferredCallback(MoveTemp(Final));
                return;
            }

            UWorld* CurrentPIEWorld = GEditor->PlayWorld;
            APawn* CurrentPawn = WeakPawn.Get();
            if (CurrentPawn == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Player pawn no longer valid"));
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            if (!bUseNavigation)
            {
                const float MoveSpeedUnitsPerSec = 600.0f;
                const float Step = MoveSpeedUnitsPerSec * 0.1f;
                const FVector NewLocation = FMath::VInterpConstantTo(
                    CurrentPawn->GetActorLocation(),
                    TargetLocation,
                    0.1f,
                    Step);
                CurrentPawn->SetActorLocation(NewLocation, false, nullptr, ETeleportType::None);
            }

            const FVector Current = CurrentPawn->GetActorLocation();
            const double Distance = FVector::Dist(Current, TargetLocation);
            const double Elapsed = CurrentPIEWorld->GetTimeSeconds() - StartGameTime;

            if (Distance <= AcceptanceRadius)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("arrived"), true);
                FCortexQAUtils::SetVectorObject(Data, TEXT("final_location"), Current);
                Data->SetNumberField(TEXT("distance_to_target"), Distance);
                Data->SetNumberField(TEXT("duration_seconds"), Elapsed);
                Data->SetStringField(TEXT("movement_method"), MovementMethod);
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            if (Elapsed >= TimeoutSeconds)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("arrived"), false);
                FCortexQAUtils::SetVectorObject(Data, TEXT("final_location"), Current);
                Data->SetNumberField(TEXT("distance_to_target"), Distance);
                Data->SetNumberField(TEXT("duration_seconds"), Elapsed);
                Data->SetStringField(TEXT("reason"), TEXT("timeout"));
                Data->SetStringField(TEXT("movement_method"), MovementMethod);
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
            }
        },
        0.1f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}

FCortexCommandResult FCortexQAActionOps::WaitFor(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    if (!DeferredCallback)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Deferred callback is required"));
    }

    double TimeoutSeconds = 5.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("timeout"), TimeoutSeconds);
    }

    FString Type;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidCondition, TEXT("Missing required param: type"));
    }

    if (Type == TEXT("delay"))
    {
        const double StartRealTime = FPlatformTime::Seconds();
        const double ClampedTimeoutSeconds = FMath::Max(0.0, TimeoutSeconds);
        TSharedPtr<FTimerHandle> DelayHandle = MakeShared<FTimerHandle>();
        PIEWorld->GetTimerManager().SetTimer(
            *DelayHandle,
            [PIEWorld, DelayHandle, DeferredCallback = MoveTemp(DeferredCallback), ClampedTimeoutSeconds, StartRealTime]() mutable
            {
                if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
                {
                    FCortexCommandResult Final = FCortexCommandRouter::Error(
                        CortexErrorCodes::PIETerminated,
                        TEXT("PIE terminated during wait_for delay"));
                    DeferredCallback(MoveTemp(Final));
                    if (PIEWorld != nullptr)
                    {
                        PIEWorld->GetTimerManager().ClearTimer(*DelayHandle);
                    }
                    return;
                }

                const double Elapsed = FPlatformTime::Seconds() - StartRealTime;
                if (Elapsed >= ClampedTimeoutSeconds)
                {
                    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                    Data->SetBoolField(TEXT("condition_met"), true);
                    FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                    DeferredCallback(MoveTemp(Final));
                    PIEWorld->GetTimerManager().ClearTimer(*DelayHandle);
                }
            },
            0.1f,
            true);

        FCortexCommandResult Deferred;
        Deferred.bIsDeferred = true;
        return Deferred;
    }

    const double StartGameTime = PIEWorld->GetTimeSeconds();
    TSharedPtr<FJsonValue> LastActualValue;
    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, DeferredCallback = MoveTemp(DeferredCallback), Params, TimeoutSeconds, StartGameTime, Type, LastActualValue]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during wait_for"));
                DeferredCallback(MoveTemp(Final));
                return;
            }

            UWorld* CurrentPIEWorld = GEditor->PlayWorld;
            const FCortexQAConditionEvalResult Eval = FCortexQAConditionUtils::Evaluate(CurrentPIEWorld, Params);
            if (!Eval.bValid)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(Eval.ErrorCode, Eval.ErrorMessage);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            if (Eval.ActualValue.IsValid())
            {
                LastActualValue = Eval.ActualValue;
            }

            if (Eval.bPassed)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("condition_met"), true);
                if (Eval.ActualValue.IsValid())
                {
                    Data->SetField(TEXT("actual_value"), Eval.ActualValue);
                }
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            const double Elapsed = CurrentPIEWorld->GetTimeSeconds() - StartGameTime;
            if (Elapsed >= TimeoutSeconds)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("condition_met"), false);
                Data->SetStringField(TEXT("type"), Type);
                Data->SetBoolField(TEXT("timed_out"), true);
                Data->SetNumberField(TEXT("duration_seconds"), Elapsed);
                if (LastActualValue.IsValid())
                {
                    Data->SetField(TEXT("actual_value"), LastActualValue);
                }
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                CurrentPIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
            }
        },
        0.1f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}
