#include "Operations/CortexQAWorldOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAUtils.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"

namespace
{
    FString AngleToDirection(float AngleDegrees, float VerticalDegrees, float SideSign)
    {
        if (FMath::Abs(VerticalDegrees) > 45.0f)
        {
            return VerticalDegrees > 0.0f ? TEXT("above") : TEXT("below");
        }
        if (AngleDegrees < 30.0f)
        {
            return TEXT("ahead");
        }
        if (AngleDegrees > 150.0f)
        {
            return TEXT("behind");
        }
        return SideSign >= 0.0f ? TEXT("right") : TEXT("left");
    }

    TSharedPtr<FJsonObject> SerializeActorState(
        AActor* Actor,
        const FVector* PlayerLocation,
        const FVector* PlayerForward,
        UWorld* PIEWorld,
        bool bIncludeLOS)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Actor->GetName());
        Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        FCortexQAUtils::SetVectorArray(Json, TEXT("location"), Actor->GetActorLocation());
        FCortexQAUtils::SetRotatorArray(Json, TEXT("rotation"), Actor->GetActorRotation());
        FCortexQAUtils::SetVectorArray(Json, TEXT("scale"), Actor->GetActorScale3D());
        Json->SetBoolField(TEXT("is_hidden"), Actor->IsHidden());

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Json->SetArrayField(TEXT("tags"), Tags);

        TArray<UActorComponent*> Components = Actor->GetComponents().Array();
        TArray<TSharedPtr<FJsonValue>> ComponentsJson;
        for (const UActorComponent* Component : Components)
        {
            if (Component == nullptr)
            {
                continue;
            }

            TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
            ComponentJson->SetStringField(TEXT("name"), Component->GetName());
            ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
            ComponentsJson.Add(MakeShared<FJsonValueObject>(ComponentJson));
        }
        Json->SetArrayField(TEXT("components"), ComponentsJson);

        if (PlayerLocation != nullptr && PlayerForward != nullptr)
        {
            const FVector ToActor = Actor->GetActorLocation() - *PlayerLocation;
            const double Distance = ToActor.Size();
            const FVector Dir = ToActor.GetSafeNormal();
            const double Dot = FVector::DotProduct(*PlayerForward, Dir);
            const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0)));
            const double Vertical = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Dir.Z, -1.0f, 1.0f)));
            const double SideSign = FVector::CrossProduct(*PlayerForward, Dir).Z;

            Json->SetNumberField(TEXT("distance"), Distance);
            Json->SetNumberField(TEXT("relative_angle"), Angle);
            Json->SetStringField(TEXT("relative_direction"), AngleToDirection(static_cast<float>(Angle), static_cast<float>(Vertical), static_cast<float>(SideSign)));
            Json->SetBoolField(TEXT("in_interaction_range"), Distance <= 200.0);

            if (bIncludeLOS && PIEWorld != nullptr)
            {
                FHitResult Hit;
                FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CortexQAObserveLOS), true);
                TraceParams.AddIgnoredActor(Actor);
                const bool bHit = PIEWorld->LineTraceSingleByChannel(
                    Hit,
                    *PlayerLocation,
                    Actor->GetActorLocation(),
                    ECC_Visibility,
                    TraceParams);
                Json->SetBoolField(TEXT("in_line_of_sight"), !bHit || Hit.GetActor() == Actor);
            }
        }

        return Json;
    }
}

FCortexCommandResult FCortexQAWorldOps::ObserveState(const TSharedPtr<FJsonObject>& Params)
{    
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    bool bIncludeHidden = true;
    bool bIncludeLOS = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("include_hidden"), bIncludeHidden);
        Params->TryGetBoolField(TEXT("include_line_of_sight"), bIncludeLOS);
    }

    FVector PlayerLocation = FVector::ZeroVector;
    FVector PlayerForward = FVector::ForwardVector;
    const FCortexCommandResult PlayerState = GetPlayerState(MakeShared<FJsonObject>());
    const bool bHasPlayer = PlayerState.bSuccess && PlayerState.Data.IsValid();
    if (bHasPlayer)
    {
        APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
        APawn* Pawn = (PC != nullptr) ? PC->GetPawn() : nullptr;
        if (Pawn != nullptr)
        {
            PlayerLocation = Pawn->GetActorLocation();
            PlayerForward = Pawn->GetActorForwardVector().GetSafeNormal();
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> World = MakeShared<FJsonObject>();
    World->SetNumberField(TEXT("time_seconds"), PIEWorld->GetTimeSeconds());
    World->SetNumberField(TEXT("delta_seconds"), PIEWorld->GetDeltaSeconds());
    Data->SetObjectField(TEXT("world"), World);

    TArray<TSharedPtr<FJsonValue>> ActorsJson;
    for (TActorIterator<AActor> It(PIEWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }

        if (!bIncludeHidden && Actor->IsHidden())
        {
            continue;
        }

        ActorsJson.Add(MakeShared<FJsonValueObject>(SerializeActorState(
            Actor,
            bHasPlayer ? &PlayerLocation : nullptr,
            bHasPlayer ? &PlayerForward : nullptr,
            PIEWorld,
            bIncludeLOS)));
    }
    Data->SetArrayField(TEXT("actors"), ActorsJson);
    Data->SetNumberField(TEXT("actor_count"), ActorsJson.Num());

    if (bHasPlayer)
    {
        Data->SetObjectField(TEXT("player"), PlayerState.Data);
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAWorldOps::GetActorState(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    FString ActorId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            TEXT("Missing required param: actor"));
    }

    AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
    if (Actor == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            FString::Printf(TEXT("Actor not found: %s"), *ActorId));
    }

    return FCortexCommandRouter::Success(SerializeActorState(Actor, nullptr, nullptr, PIEWorld, false));
}

FCortexCommandResult FCortexQAWorldOps::GetPlayerState(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    if (PC == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player controller found"));
    }

    APawn* Pawn = PC->GetPawn();
    if (Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player pawn found"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    FCortexQAUtils::SetVectorArray(Data, TEXT("location"), Pawn->GetActorLocation());
    FCortexQAUtils::SetRotatorArray(Data, TEXT("rotation"), Pawn->GetActorRotation());
    FCortexQAUtils::SetVectorArray(Data, TEXT("velocity"), Pawn->GetVelocity());
    Data->SetNumberField(TEXT("speed"), Pawn->GetVelocity().Size());
    Data->SetBoolField(TEXT("is_moving"), !Pawn->GetVelocity().IsNearlyZero());
    Data->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetName());
    Data->SetStringField(TEXT("controller_class"), PC->GetClass()->GetName());

    UCharacterMovementComponent* CharacterMovement = Pawn->FindComponentByClass<UCharacterMovementComponent>();
    Data->SetBoolField(TEXT("is_falling"), CharacterMovement != nullptr && CharacterMovement->IsFalling());
    Data->SetBoolField(TEXT("is_crouching"), CharacterMovement != nullptr && CharacterMovement->IsCrouching());

    if (APlayerCameraManager* CameraManager = PC->PlayerCameraManager)
    {
        FCortexQAUtils::SetVectorArray(Data, TEXT("camera_location"), CameraManager->GetCameraLocation());
        FCortexQAUtils::SetRotatorArray(Data, TEXT("camera_rotation"), CameraManager->GetCameraRotation());
        Data->SetNumberField(TEXT("camera_fov"), CameraManager->GetFOVAngle());
    }

    return FCortexCommandRouter::Success(Data);
}
