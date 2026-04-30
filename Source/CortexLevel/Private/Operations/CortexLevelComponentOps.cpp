#include "Operations/CortexLevelComponentOps.h"

#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "CortexLevelUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace
{
    TSharedPtr<FJsonObject> CopyLevelComponentBatchJsonObject(const TSharedPtr<FJsonObject>& Source)
    {
        TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
        if (!Source.IsValid())
        {
            return Copy;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
        {
            Copy->SetField(Pair.Key, Pair.Value);
        }

        return Copy;
    }

    TArray<TSharedPtr<FJsonValue>> ToLevelComponentBatchJsonStringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    FCortexAssetFingerprint MakeLevelComponentActorFingerprint(const AActor* Actor)
    {
        if (!IsValid(Actor))
        {
            return MakeObjectAssetFingerprint(nullptr);
        }

        return MakeObjectAssetFingerprint(
            Actor,
            GetTypeHash(Actor->GetPathName() + TEXT("|") + Actor->GetActorLabel() + TEXT("|") + Actor->GetClass()->GetPathName()));
    }

    TSharedPtr<FJsonObject> BuildLevelComponentBatchResponseData(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("status"), BatchResult.Status);
        Data->SetArrayField(TEXT("written_targets"), ToLevelComponentBatchJsonStringArray(BatchResult.WrittenTargets));
        Data->SetArrayField(TEXT("unwritten_targets"), ToLevelComponentBatchJsonStringArray(BatchResult.UnwrittenTargets));

        TArray<TSharedPtr<FJsonValue>> PerItem;
        PerItem.Reserve(BatchResult.PerItem.Num());
        for (const FCortexBatchMutationItemResult& ItemResult : BatchResult.PerItem)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("target"), ItemResult.Target);
            Entry->SetBoolField(TEXT("success"), ItemResult.Result.bSuccess);
            if (ItemResult.Result.Data.IsValid())
            {
                Entry->SetObjectField(TEXT("data"), ItemResult.Result.Data);
            }
            if (!ItemResult.Result.ErrorCode.IsEmpty())
            {
                Entry->SetStringField(TEXT("error_code"), ItemResult.Result.ErrorCode);
            }
            if (!ItemResult.Result.ErrorMessage.IsEmpty())
            {
                Entry->SetStringField(TEXT("error_message"), ItemResult.Result.ErrorMessage);
            }
            if (ItemResult.Result.ErrorDetails.IsValid())
            {
                Entry->SetObjectField(TEXT("error_details"), ItemResult.Result.ErrorDetails);
            }
            if (ItemResult.Result.Warnings.Num() > 0)
            {
                Entry->SetArrayField(TEXT("warnings"), ToLevelComponentBatchJsonStringArray(ItemResult.Result.Warnings));
            }
            PerItem.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("per_item"), PerItem);
        return Data;
    }

    FCortexCommandResult MakeLevelComponentBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = BuildLevelComponentBatchResponseData(BatchResult);
        if (BatchResult.Status == TEXT("committed"))
        {
            return FCortexCommandRouter::Success(Data);
        }

        return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
    }

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

    FCortexBatchPreflightResult PreflightSetComponentProperty(const FCortexBatchMutationItem& Item)
    {
        FCortexCommandResult Error;
        UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
        if (!World)
        {
            return FCortexBatchPreflightResult::Error(Error.ErrorCode, Error.ErrorMessage, Error.ErrorDetails);
        }

        AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, Item.Target, Error);
        if (!Actor)
        {
            return FCortexBatchPreflightResult::Error(Error.ErrorCode, Error.ErrorMessage, Error.ErrorDetails);
        }

        FString ComponentName;
        FString PropertyName;
        if (!Item.Params.IsValid()
            || !Item.Params->TryGetStringField(TEXT("component"), ComponentName)
            || ComponentName.IsEmpty()
            || !Item.Params->TryGetStringField(TEXT("property"), PropertyName)
            || PropertyName.IsEmpty())
        {
            return FCortexBatchPreflightResult::Error(
                CortexErrorCodes::InvalidValue,
                TEXT("Missing required parameter: component or property"));
        }

        if (!Item.Params->HasField(TEXT("value")))
        {
            return FCortexBatchPreflightResult::Error(
                CortexErrorCodes::InvalidValue,
                TEXT("Missing required parameter: value"));
        }

        UActorComponent* Component = FindComponentByName(Actor, ComponentName);
        if (!Component)
        {
            return FCortexBatchPreflightResult::Error(
                CortexErrorCodes::ComponentNotFound,
                FString::Printf(TEXT("Component not found: %s"), *ComponentName),
                FCortexLevelUtils::CollectComponentSuggestions(Actor));
        }

        FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!Property)
        {
            return FCortexBatchPreflightResult::Error(
                CortexErrorCodes::PropertyNotFound,
                FString::Printf(TEXT("Property not found: %s"), *PropertyName));
        }

        return FCortexBatchPreflightResult::Success(MakeLevelComponentActorFingerprint(Actor).ToJson());
    }

    FCortexCommandResult CommitSetComponentProperty(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelComponentBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelComponentOps::SetComponentProperty(ItemParams);
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
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            FCortexLevelUtils::CollectComponentSuggestions(Actor)
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
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound,
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            FCortexLevelUtils::CollectComponentSuggestions(Actor));
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
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }

        return MakeLevelComponentBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightSetComponentProperty,
            CommitSetComponentProperty));
    }

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
        return FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound,
            FString::Printf(TEXT("Component not found: %s"), *ComponentName),
            FCortexLevelUtils::CollectComponentSuggestions(Actor));
    }

    // Check handler registry for custom write logic
    const FPropertyWriteHandler* CustomHandler = GetWriteHandlers().Find(FName(*PropertyName));

    // Resolve the property for generic path and PECP notification
    FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!CustomHandler && !Property)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound,
            FString::Printf(TEXT("Property not found: %s"), *PropertyName));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Component Property")));
    Component->Modify();

    // PreEditChange before any write (mirrors Details Panel pattern)
    if (Property)
    {
        Component->PreEditChange(Property);
    }

    TArray<FString> Warnings;
    bool bWriteSucceeded = false;
    if (CustomHandler)
    {
        bWriteSucceeded = (*CustomHandler)(Component, JsonValue, Warnings);
    }
    else
    {
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Component);
        bWriteSucceeded = FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Component, Warnings);
    }

    // Always pair PostEditChangeProperty with PreEditChange — even if the write failed
    // (transform hierarchy, visibility cascading, mobility re-registration, etc.)
    if (Property)
    {
        FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
        Component->PostEditChangeProperty(ChangedEvent);
    }

    if (!bWriteSucceeded)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue,
            FString::Printf(TEXT("Failed to set property: %s"), *PropertyName));
    }

    // Belt-and-suspenders: mark render state dirty and refresh viewports
    Component->MarkRenderStateDirty();
    if (GEditor)
    {
        GEditor->RedrawAllViewports();
    }

    // Build response — re-read the value after PECP (it may have been clamped/normalized)
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("component"), Component->GetName());
    Data->SetStringField(TEXT("property"), PropertyName);

    if (Property)
    {
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Component);
        Data->SetStringField(TEXT("type"), Property->GetCPPType());
        Data->SetField(TEXT("value"), FCortexSerializer::PropertyToJson(Property, ValuePtr));
    }

    FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
    Result.Warnings = MoveTemp(Warnings);
    return Result;
}

const TMap<FName, FCortexLevelComponentOps::FPropertyWriteHandler>& FCortexLevelComponentOps::GetWriteHandlers()
{
    static const TMap<FName, FPropertyWriteHandler> Handlers;
    // Empty: all properties currently work via generic write + PECP.
    // Add handlers here only when a property's generic write itself fails
    // (not for side effects — PostEditChangeProperty handles those).
    return Handlers;
}
