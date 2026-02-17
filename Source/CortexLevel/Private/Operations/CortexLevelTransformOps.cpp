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
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
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
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!Actor)
    {
        return Error;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Actor Transform")));
    Actor->Modify();

    FVector Location;
    if (FCortexLevelUtils::ParseVectorField(Params, TEXT("location"), Location))
    {
        Actor->SetActorLocation(Location);
    }

    FVector Rotation;
    if (FCortexLevelUtils::ParseVectorField(Params, TEXT("rotation"), Rotation))
    {
        Actor->SetActorRotation(FRotator(Rotation.X, Rotation.Y, Rotation.Z));
    }

    FVector Scale;
    if (FCortexLevelUtils::ParseVectorField(Params, TEXT("scale"), Scale))
    {
        Actor->SetActorScale3D(Scale);
    }

    return FCortexCommandRouter::Success(SerializeActorDetails(Actor));
}

FCortexCommandResult FCortexLevelTransformOps::SetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound, TEXT("Missing required parameter: property"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
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
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: value"));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Actor Property")));
    Actor->Modify();

    TArray<FString> Warnings;
    if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Warnings))
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
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound, TEXT("Missing required parameter: property"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
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
