#include "Operations/CortexQAWorldOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAUtils.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
    TSharedPtr<FJsonObject> SerializeActorState(AActor* Actor)
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

        return Json;
    }
}

FCortexCommandResult FCortexQAWorldOps::ObserveState(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    TArray<TSharedPtr<FJsonValue>> ActorsJson;
    for (TActorIterator<AActor> It(PIEWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }

        ActorsJson.Add(MakeShared<FJsonValueObject>(SerializeActorState(Actor)));
    }
    Data->SetArrayField(TEXT("actors"), ActorsJson);
    Data->SetNumberField(TEXT("actor_count"), ActorsJson.Num());

    const FCortexCommandResult PlayerState = GetPlayerState(MakeShared<FJsonObject>());
    if (PlayerState.bSuccess && PlayerState.Data.IsValid())
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

    return FCortexCommandRouter::Success(SerializeActorState(Actor));
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
