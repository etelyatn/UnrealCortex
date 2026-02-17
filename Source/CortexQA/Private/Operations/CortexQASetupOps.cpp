#include "Operations/CortexQASetupOps.h"

#include "CortexCommandRouter.h"
#include "CortexPropertyUtils.h"
#include "CortexQAUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
    bool ParseVector(const TArray<TSharedPtr<FJsonValue>>& Array, FVector& Out)
    {
        if (Array.Num() != 3)
        {
            return false;
        }
        Out = FVector(
            static_cast<float>(Array[0]->AsNumber()),
            static_cast<float>(Array[1]->AsNumber()),
            static_cast<float>(Array[2]->AsNumber()));
        return true;
    }
}

FCortexCommandResult FCortexQASetupOps::TeleportPlayer(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = (PC != nullptr) ? PC->GetPawn() : nullptr;
    if (Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player pawn found"));
    }

    const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("location"), LocationArray) || LocationArray == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: location"));
    }

    FVector Location;
    if (!ParseVector(*LocationArray, Location))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("location must be [x,y,z]"));
    }

    FRotator Rotation = Pawn->GetActorRotation();
    const TArray<TSharedPtr<FJsonValue>>* RotationArray = nullptr;
    if (Params->TryGetArrayField(TEXT("rotation"), RotationArray) && RotationArray != nullptr)
    {
        FVector RotationVec;
        if (!ParseVector(*RotationArray, RotationVec))
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("rotation must be [pitch,yaw,roll]"));
        }
        Rotation = FRotator(RotationVec.X, RotationVec.Y, RotationVec.Z);
    }

    Pawn->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    FCortexQAUtils::SetVectorArray(Data, TEXT("location"), Pawn->GetActorLocation());
    FCortexQAUtils::SetRotatorArray(Data, TEXT("rotation"), Pawn->GetActorRotation());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQASetupOps::SetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    FString ActorId;
    FString PropertyPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: actor"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: property"));
    }

    AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
    if (Actor == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorId));
    }

    FProperty* Property = nullptr;
    void* ValuePtr = nullptr;
    if (!FCortexPropertyUtils::ResolvePropertyPath(Actor, PropertyPath, Property, ValuePtr))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::PropertyNotFound,
            FString::Printf(TEXT("Property path not found: %s"), *PropertyPath));
    }

    const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (!JsonValue.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: value"));
    }

    TArray<FString> Warnings;
    if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Warnings))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::UnsupportedType,
            FString::Printf(TEXT("Unsupported property type for '%s'"), *PropertyPath));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("property"), PropertyPath);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQASetupOps::SetRandomSeed(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    int32 Seed = 0;
    if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("seed"), Seed))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: seed"));
    }

    FMath::RandInit(Seed);
    FMath::SRandInit(Seed);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    Data->SetNumberField(TEXT("seed"), Seed);
    return FCortexCommandRouter::Success(Data);
}
