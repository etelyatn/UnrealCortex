#include "Operations/CortexQAActionOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAConditionUtils.h"
#include "CortexQAUtils.h"
#include "CortexTypes.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
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

        if (TargetValue->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Array = TargetValue->AsArray();
            if (Array.Num() != 3)
            {
                OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target array must have 3 elements"));
                return false;
            }
            OutTarget = FVector(
                static_cast<float>(Array[0]->AsNumber()),
                static_cast<float>(Array[1]->AsNumber()),
                static_cast<float>(Array[2]->AsNumber()));
            return true;
        }

        OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target must be actor name or [x,y,z]"));
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
    APawn* Pawn = (PC != nullptr) ? PC->GetPawn() : nullptr;
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
    FCortexQAUtils::SetRotatorArray(Data, TEXT("control_rotation"), LookRotation);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAActionOps::Interact(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    if (PC == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player controller"));
    }

    FString KeyName = TEXT("E");
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("key"), KeyName);
    }

    const FKey InteractionKey(*KeyName);
    if (!InteractionKey.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Invalid key name"));
    }

    PC->InputKey(FInputKeyParams(InteractionKey, EInputEvent::IE_Pressed, 1.0, false));

    TSharedPtr<FTimerHandle> ReleaseHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *ReleaseHandle,
        [PC, InteractionKey]()
        {
                if (PC != nullptr)
                {
                PC->InputKey(FInputKeyParams(InteractionKey, EInputEvent::IE_Released, 0.0, false));
                }
        },
        0.1f,
        false);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    Data->SetStringField(TEXT("key"), KeyName);
    return FCortexCommandRouter::Success(Data);
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
    APawn* Pawn = (PC != nullptr) ? PC->GetPawn() : nullptr;
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

    const double StartSeconds = FPlatformTime::Seconds();
    const FString MovementMethod = TEXT("navigation");
    if (FNavigationSystem::GetCurrent<UNavigationSystemV1>(PIEWorld) != nullptr)
    {
        UAIBlueprintHelperLibrary::SimpleMoveToLocation(PC, TargetLocation);
    }

    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, DeferredCallback = MoveTemp(DeferredCallback), TargetLocation, AcceptanceRadius, TimeoutSeconds, StartSeconds, Pawn, PIEWorld, MovementMethod]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during move_to"));
                DeferredCallback(MoveTemp(Final));
                if (PIEWorld != nullptr)
                {
                    PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                }
                return;
            }

            if (Pawn == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Player pawn no longer valid"));
                DeferredCallback(MoveTemp(Final));
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            const FVector Current = Pawn->GetActorLocation();
            const double Distance = FVector::Dist(Current, TargetLocation);
            const double Elapsed = FPlatformTime::Seconds() - StartSeconds;

            if (Distance <= AcceptanceRadius)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("arrived"), true);
                FCortexQAUtils::SetVectorArray(Data, TEXT("final_location"), Current);
                Data->SetNumberField(TEXT("distance_to_target"), Distance);
                Data->SetNumberField(TEXT("duration_seconds"), Elapsed);
                Data->SetStringField(TEXT("movement_method"), MovementMethod);
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            if (Elapsed >= TimeoutSeconds)
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetBoolField(TEXT("arrived"), false);
                Details->SetNumberField(TEXT("distance_to_target"), Distance);
                FCortexCommandResult Final = FCortexCommandRouter::Error(
                    CortexErrorCodes::ConditionTimeout,
                    TEXT("move_to timed out"),
                    Details);
                DeferredCallback(MoveTemp(Final));
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
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
        TSharedPtr<FTimerHandle> DelayHandle = MakeShared<FTimerHandle>();
        PIEWorld->GetTimerManager().SetTimer(
            *DelayHandle,
            [DeferredCallback = MoveTemp(DeferredCallback)]() mutable
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetBoolField(TEXT("condition_met"), true);
                FCortexCommandResult Final = FCortexCommandRouter::Success(Data);
                DeferredCallback(MoveTemp(Final));
            },
            static_cast<float>(FMath::Max(0.0, TimeoutSeconds)),
            false);

        FCortexCommandResult Deferred;
        Deferred.bIsDeferred = true;
        return Deferred;
    }

    const double StartSeconds = FPlatformTime::Seconds();
    TSharedPtr<FTimerHandle> TimerHandle = MakeShared<FTimerHandle>();
    PIEWorld->GetTimerManager().SetTimer(
        *TimerHandle,
        [TimerHandle, DeferredCallback = MoveTemp(DeferredCallback), Params, TimeoutSeconds, StartSeconds, PIEWorld]() mutable
        {
            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(CortexErrorCodes::PIETerminated, TEXT("PIE terminated during wait_for"));
                DeferredCallback(MoveTemp(Final));
                if (PIEWorld != nullptr)
                {
                    PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                }
                return;
            }

            const FCortexQAConditionEvalResult Eval = FCortexQAConditionUtils::Evaluate(PIEWorld, Params);
            if (!Eval.bValid)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(Eval.ErrorCode, Eval.ErrorMessage);
                DeferredCallback(MoveTemp(Final));
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
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
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
                return;
            }

            const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
            if (Elapsed >= TimeoutSeconds)
            {
                FCortexCommandResult Final = FCortexCommandRouter::Error(
                    CortexErrorCodes::ConditionTimeout,
                    TEXT("wait_for timed out"));
                DeferredCallback(MoveTemp(Final));
                PIEWorld->GetTimerManager().ClearTimer(*TimerHandle);
            }
        },
        0.1f,
        true);

    FCortexCommandResult Deferred;
    Deferred.bIsDeferred = true;
    return Deferred;
}
