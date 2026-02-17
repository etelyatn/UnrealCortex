#include "Operations/CortexLevelComponentOps.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "CortexLevelUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace
{
    UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor || ComponentName.IsEmpty())
        {
            return nullptr;
        }

        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!IsValid(Component))
            {
                continue;
            }

            if (Component->GetName() == ComponentName)
            {
                return Component;
            }
        }

        return nullptr;
    }

    UClass* ResolveComponentClass(const FString& ClassName)
    {
        UClass* FoundClass = FindObject<UClass>(nullptr, *ClassName);
        if (FoundClass)
        {
            return FoundClass;
        }

        if (!ClassName.StartsWith(TEXT("/")))
        {
            const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
            FoundClass = FindObject<UClass>(nullptr, *EnginePath);
            if (FoundClass)
            {
                return FoundClass;
            }
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!IsValid(Candidate))
            {
                continue;
            }

            if (Candidate->GetName() == ClassName || Candidate->GetPathName() == ClassName)
            {
                return Candidate;
            }
        }

        return nullptr;
    }

    FString CreationMethodToString(EComponentCreationMethod Method)
    {
        switch (Method)
        {
        case EComponentCreationMethod::Native:
            return TEXT("Native");
        case EComponentCreationMethod::SimpleConstructionScript:
            return TEXT("SimpleConstructionScript");
        case EComponentCreationMethod::UserConstructionScript:
            return TEXT("UserConstructionScript");
        case EComponentCreationMethod::Instance:
            return TEXT("Instance");
        default:
            return TEXT("Unknown");
        }
    }

    TSharedPtr<FJsonObject> SerializeComponent(UActorComponent* Component, const USceneComponent* RootComponent)
    {
        TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
        ComponentJson->SetStringField(TEXT("name"), Component->GetName());
        ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
        ComponentJson->SetBoolField(TEXT("is_root"), Component == RootComponent);
        ComponentJson->SetStringField(TEXT("creation_method"), CreationMethodToString(Component->CreationMethod));

        TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
        int32 PropertyCount = 0;
        for (TFieldIterator<FProperty> It(Component->GetClass()); It && PropertyCount < 20; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }

            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Component);
            PropertiesJson->SetField(Property->GetName(), FCortexSerializer::PropertyToJson(Property, ValuePtr));
            ++PropertyCount;
        }

        ComponentJson->SetObjectField(TEXT("properties"), PropertiesJson);
        return ComponentJson;
    }
}

FCortexCommandResult FCortexLevelComponentOps::ListComponents(const TSharedPtr<FJsonObject>& Params)
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

    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);

    const USceneComponent* RootComponent = Actor->GetRootComponent();
    TArray<TSharedPtr<FJsonValue>> ComponentArray;

    for (UActorComponent* Component : Components)
    {
        if (!IsValid(Component))
        {
            continue;
        }

        ComponentArray.Add(MakeShared<FJsonValueObject>(SerializeComponent(Component, RootComponent)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetArrayField(TEXT("components"), ComponentArray);
    Data->SetNumberField(TEXT("count"), ComponentArray.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelComponentOps::AddComponent(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("class"), ClassName) || ClassName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Missing required parameter: class"));
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

    UClass* ComponentClass = ResolveComponentClass(ClassName);
    if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ClassNotFound,
            FString::Printf(TEXT("Component class not found: %s"), *ClassName)
        );
    }

    FString ComponentName;
    Params->TryGetStringField(TEXT("name"), ComponentName);
    if (ComponentName.IsEmpty())
    {
        ComponentName = FString::Printf(TEXT("%s_Cortex"), *ComponentClass->GetName());
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Component")));
    Actor->Modify();

    UActorComponent* NewComponent = NewObject<UActorComponent>(Actor, ComponentClass, FName(*ComponentName), RF_Transactional);
    if (!NewComponent)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to allocate component"));
    }

    NewComponent->CreationMethod = EComponentCreationMethod::Instance;

    if (USceneComponent* SceneComponent = Cast<USceneComponent>(NewComponent))
    {
        SceneComponent->SetupAttachment(Actor->GetRootComponent());
    }

    NewComponent->RegisterComponent();
    Actor->AddInstanceComponent(NewComponent);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("name"), NewComponent->GetName());
    Data->SetStringField(TEXT("class"), NewComponent->GetClass()->GetName());
    Data->SetStringField(TEXT("creation_method"), CreationMethodToString(NewComponent->CreationMethod));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelComponentOps::RemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("component"), ComponentName) || ComponentName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound, TEXT("Missing required parameter: component"));
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

    UActorComponent* Component = FindComponentByName(Actor, ComponentName);
    if (!Component)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ComponentNotFound,
            FString::Printf(TEXT("Component not found: %s"), *ComponentName)
        );
    }

    if (Component->CreationMethod == EComponentCreationMethod::Native ||
        Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ComponentRemoveDenied,
            FString::Printf(TEXT("Cannot remove component '%s' with creation method '%s'"), *ComponentName, *CreationMethodToString(Component->CreationMethod))
        );
    }

    const FString RemovedName = Component->GetName();

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Component")));
    Actor->Modify();
    Component->Modify();

    Actor->RemoveInstanceComponent(Component);
    Component->DestroyComponent();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("name"), RemovedName);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelComponentOps::GetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString ComponentName;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("component"), ComponentName) || ComponentName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound, TEXT("Missing required parameter: component"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
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

    UActorComponent* Component = FindComponentByName(Actor, ComponentName);
    if (!Component)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound, FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound, FString::Printf(TEXT("Property not found: %s"), *PropertyName));
    }

    void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Component);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("component"), Component->GetName());
    Data->SetStringField(TEXT("property"), PropertyName);
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    Data->SetField(TEXT("value"), FCortexSerializer::PropertyToJson(Property, ValuePtr));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelComponentOps::SetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    FString ComponentName;
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("component"), ComponentName) || ComponentName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound, TEXT("Missing required parameter: component"));
    }
    if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound, TEXT("Missing required parameter: property"));
    }

    TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (!JsonValue.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: value"));
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

    UActorComponent* Component = FindComponentByName(Actor, ComponentName);
    if (!Component)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound, FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    }

    FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound, FString::Printf(TEXT("Property not found: %s"), *PropertyName));
    }

    void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Component);

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Component Property")));
    Component->Modify();

    TArray<FString> Warnings;
    if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Warnings))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, FString::Printf(TEXT("Failed to set property: %s"), *PropertyName));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("component"), Component->GetName());
    Data->SetStringField(TEXT("property"), PropertyName);
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    Data->SetField(TEXT("value"), FCortexSerializer::PropertyToJson(Property, ValuePtr));

    FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
    Result.Warnings = MoveTemp(Warnings);
    return Result;
}
