#include "Operations/CortexLevelTransformOps.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "CortexLevelUtils.h"
#include "CortexPropertyUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
    FString MobilityToString(EComponentMobility::Type Mobility)
    {
        switch (Mobility)
        {
        case EComponentMobility::Static:
            return TEXT("Static");
        case EComponentMobility::Stationary:
            return TEXT("Stationary");
        case EComponentMobility::Movable:
            return TEXT("Movable");
        default:
            return TEXT("Unknown");
        }
    }

    TSharedPtr<FJsonObject> SerializeActorDetails(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("name"), Actor->GetName());
        Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        Data->SetStringField(TEXT("blueprint"), Actor->GetClass()->ClassGeneratedBy ? Actor->GetClass()->ClassGeneratedBy->GetPathName() : TEXT(""));

        FCortexLevelUtils::SetVectorArray(Data, TEXT("location"), Actor->GetActorLocation());
        const FRotator Rotation = Actor->GetActorRotation();
        FCortexLevelUtils::SetVectorArray(Data, TEXT("rotation"), FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
        FCortexLevelUtils::SetVectorArray(Data, TEXT("scale"), Actor->GetActorScale3D());

        const USceneComponent* Root = Actor->GetRootComponent();
        Data->SetStringField(TEXT("mobility"), Root ? MobilityToString(Root->Mobility) : TEXT("Unknown"));
        Data->SetBoolField(TEXT("hidden"), Actor->IsHidden());
        Data->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Data->SetArrayField(TEXT("tags"), Tags);

        AActor* ParentActor = Actor->GetAttachParentActor();
        Data->SetStringField(TEXT("parent"), ParentActor ? ParentActor->GetName() : TEXT(""));

        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);

        TArray<TSharedPtr<FJsonValue>> ComponentArray;
        for (UActorComponent* Component : Components)
        {
            if (!IsValid(Component))
            {
                continue;
            }

            TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
            ComponentObj->SetStringField(TEXT("name"), Component->GetName());
            ComponentObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
            ComponentArray.Add(MakeShared<FJsonValueObject>(ComponentObj));
        }

        Data->SetArrayField(TEXT("components"), ComponentArray);
        Data->SetNumberField(TEXT("component_count"), ComponentArray.Num());

        return Data;
    }
}

FCortexCommandResult FCortexLevelTransformOps::GetActor(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: actor"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::ResolveWorld(Params, Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!Actor)
    {
        return Error;
    }

    return FCortexCommandRouter::Success(SerializeActorDetails(Actor));
}

FCortexCommandResult FCortexLevelTransformOps::SetTransform(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: actor"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::ResolveWorld(Params, Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!Actor)
    {
        return Error;
    }

    // Validate every present vector before the transaction so a malformed request changes nothing.
    FVector Location = FVector::ZeroVector;
    FVector Rotation = FVector::ZeroVector;
    FVector Scale = FVector::OneVector;
    const bool bHasLocation = Params->HasField(TEXT("location"));
    const bool bHasRotation = Params->HasField(TEXT("rotation"));
    const bool bHasScale = Params->HasField(TEXT("scale"));
    const bool bValidLocation = !bHasLocation || FCortexLevelUtils::ParseVectorField(Params, TEXT("location"), Location);
    const bool bValidRotation = !bHasRotation || FCortexLevelUtils::ParseVectorField(Params, TEXT("rotation"), Rotation);
    const bool bValidScale = !bHasScale || FCortexLevelUtils::ParseVectorField(Params, TEXT("scale"), Scale);
    if (!bValidLocation || !bValidRotation || !bValidScale)
    {
        const TCHAR* FieldName = !bValidLocation
            ? TEXT("location")
            : (!bValidRotation ? TEXT("rotation") : TEXT("scale"));
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            FString::Printf(TEXT("%s must be a [x,y,z] array of numbers"), FieldName));
    }

    FCortexLevelPIETransactionScope PIEScope(World);

    FScopedTransaction Transaction(
        FText::FromString(TEXT("Cortex: Set Actor Transform")),
        FCortexLevelUtils::ShouldTransact(World));
    Actor->Modify();

    if (bHasLocation)
    {
        Actor->SetActorLocation(Location);
    }

    if (bHasRotation)
    {
        Actor->SetActorRotation(FRotator(Rotation.X, Rotation.Y, Rotation.Z));
    }

    if (bHasScale)
    {
        Actor->SetActorScale3D(Scale);
    }

    return FCortexCommandRouter::Success(SerializeActorDetails(Actor));
}

FCortexCommandResult FCortexLevelTransformOps::SetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: property"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::ResolveWorld(Params, Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!Actor)
    {
        return Error;
    }

    FProperty* Property = nullptr;
    void* ValuePtr = nullptr;
    if (!FCortexPropertyUtils::ResolvePropertyPath(Actor, PropertyPath, Property, ValuePtr))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::PropertyNotFound,
            FString::Printf(TEXT("Property path not found: %s"), *PropertyPath)
        );
    }

    TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (!JsonValue.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: value"));
    }

    FCortexLevelPIETransactionScope PIEScope(World);

    FScopedTransaction Transaction(
        FText::FromString(TEXT("Cortex: Set Actor Property")),
        FCortexLevelUtils::ShouldTransact(World));
    Actor->Modify();

    TArray<FString> Warnings;
    if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Actor, Warnings))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            FString::Printf(TEXT("Failed to set property: %s"), *PropertyPath)
        );
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetStringField(TEXT("property"), PropertyPath);
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    Data->SetField(TEXT("value"), FCortexSerializer::PropertyToJson(Property, ValuePtr));

    FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
    Result.Warnings = MoveTemp(Warnings);
    return Result;
}

FCortexCommandResult FCortexLevelTransformOps::GetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: property"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::ResolveWorld(Params, Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!Actor)
    {
        return Error;
    }

    FProperty* Property = nullptr;
    void* ValuePtr = nullptr;
    if (!FCortexPropertyUtils::ResolvePropertyPath(Actor, PropertyPath, Property, ValuePtr))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::PropertyNotFound,
            FString::Printf(TEXT("Property path not found: %s"), *PropertyPath)
        );
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("property"), PropertyPath);
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    Data->SetField(TEXT("value"), FCortexSerializer::PropertyToJson(Property, ValuePtr));
    return FCortexCommandRouter::Success(Data);
}
