#include "Operations/CortexQASetupOps.h"

#include "CortexCommandRouter.h"
#include "CortexPropertyUtils.h"
#include "CortexQAUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "GameFramework/Pawn.h"

FCortexCommandResult FCortexQASetupOps::TeleportPlayer(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No player pawn found"));
    }

    const TSharedPtr<FJsonValue> LocationValue = Params.IsValid() ? Params->TryGetField(TEXT("location")) : nullptr;
    FVector Location = FVector::ZeroVector;
    FString LocationError;
    if (!LocationValue.IsValid() || !FCortexQAUtils::ParseVectorParam(LocationValue, Location, LocationError))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("Invalid location: %s"), *LocationError));
    }

    FRotator Rotation = Pawn->GetActorRotation();
    const TSharedPtr<FJsonValue> RotationValue = Params.IsValid() ? Params->TryGetField(TEXT("rotation")) : nullptr;
    if (RotationValue.IsValid())
    {
        FString RotationError;
        if (!FCortexQAUtils::ParseRotatorParam(RotationValue, Rotation, RotationError))
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                FString::Printf(TEXT("Invalid rotation: %s"), *RotationError));
        }
    }

    Pawn->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    FCortexQAUtils::SetVectorObject(Data, TEXT("location"), Pawn->GetActorLocation());
    FCortexQAUtils::SetRotatorObject(Data, TEXT("rotation"), Pawn->GetActorRotation());
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
    if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Actor, Warnings))
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

    double SeedDouble = 0.0;
    if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("seed"), SeedDouble))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: seed"));
    }

    const int32 Seed = static_cast<int32>(SeedDouble);
    FMath::RandInit(Seed);
    FMath::SRandInit(Seed);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("success"), true);
    Data->SetNumberField(TEXT("seed"), Seed);
    return FCortexCommandRouter::Success(Data);
}
