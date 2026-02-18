#include "Operations/CortexLevelOrganizationOps.h"

#include "ActorGroupingUtils.h"
#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "Editor/GroupActor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
    bool ResolveActorList(UWorld* World, const TArray<TSharedPtr<FJsonValue>>& ActorValues, TArray<AActor*>& OutActors)
    {
        for (const TSharedPtr<FJsonValue>& Value : ActorValues)
        {
            FCortexCommandResult Error;
            AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, Value->AsString(), Error);
            if (Actor)
            {
                OutActors.Add(Actor);
            }
        }

        return OutActors.Num() > 0;
    }
}

FCortexCommandResult FCortexLevelOrganizationOps::AttachActor(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorId;
    FString ParentId;
    if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("parent"), ParentId) || ParentId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: parent"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorId, Error);
    if (!Actor)
    {
        return Error;
    }

    AActor* Parent = FCortexLevelUtils::FindActorByLabelOrPath(World, ParentId, Error);
    if (!Parent)
    {
        return Error;
    }

    FString Socket;
    Params->TryGetStringField(TEXT("socket"), Socket);

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Attach Actor")));
    Actor->Modify();
    Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform, FName(*Socket));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetStringField(TEXT("parent"), Parent->GetName());
    Data->SetStringField(TEXT("socket"), Socket);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelOrganizationOps::DetachActor(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorId;
    if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorId, Error);
    if (!Actor)
    {
        return Error;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Detach Actor")));
    Actor->Modify();
    Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetStringField(TEXT("parent"), TEXT(""));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelOrganizationOps::SetTags(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorId;
    const TArray<TSharedPtr<FJsonValue>>* TagsJson = nullptr;

    if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetArrayField(TEXT("tags"), TagsJson) || !TagsJson)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: tags"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorId, Error);
    if (!Actor)
    {
        return Error;
    }

    TArray<FName> Tags;
    for (const TSharedPtr<FJsonValue>& TagValue : *TagsJson)
    {
        Tags.Add(FName(*TagValue->AsString()));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Tags")));
    Actor->Modify();
    Actor->Tags = Tags;

    TArray<TSharedPtr<FJsonValue>> TagValues;
    for (const FName& Tag : Actor->Tags)
    {
        TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetArrayField(TEXT("tags"), TagValues);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelOrganizationOps::SetFolder(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorId;
    FString Folder;
    if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    Params->TryGetStringField(TEXT("folder"), Folder);

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorId, Error);
    if (!Actor)
    {
        return Error;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Folder")));
    Actor->Modify();
    Actor->SetFolderPath(FName(*Folder));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    const FName ResolvedFolderPath = Actor->GetFolderPath();
    Data->SetStringField(
        TEXT("folder"),
        ResolvedFolderPath.IsNone() ? TEXT("") : ResolvedFolderPath.ToString());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelOrganizationOps::GroupActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("actors"), ActorValues) || !ActorValues)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: actors"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    if (World->IsPartitionedWorld())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Grouping is not supported in World Partition levels"));
    }

    TArray<AActor*> ActorsToGroup;
    if (!ResolveActorList(World, *ActorValues, ActorsToGroup))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No actors matched for grouping"));
    }

    UActorGroupingUtils* GroupingUtils = UActorGroupingUtils::Get();
    if (!GroupingUtils)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("ActorGroupingUtils unavailable"));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Group Actors")));
    AGroupActor* Group = GroupingUtils->GroupActors(ActorsToGroup);
    if (!Group)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to group actors"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("group"), Group->GetName());
    Data->SetNumberField(TEXT("count"), ActorsToGroup.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelOrganizationOps::UngroupActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString GroupId;
    if (!Params->TryGetStringField(TEXT("group"), GroupId) || GroupId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: group"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    if (World->IsPartitionedWorld())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Ungrouping is not supported in World Partition levels"));
    }

    AActor* GroupActor = FCortexLevelUtils::FindActorByLabelOrPath(World, GroupId, Error);
    if (!GroupActor)
    {
        return Error;
    }

    AGroupActor* Group = Cast<AGroupActor>(GroupActor);
    if (!Group)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            FString::Printf(TEXT("Actor '%s' is not a group actor"), *GroupId)
        );
    }

    TArray<AActor*> GroupedActors;
    Group->GetGroupActors(GroupedActors);
    const int32 Count = GroupedActors.Num();

    UActorGroupingUtils* GroupingUtils = UActorGroupingUtils::Get();
    if (!GroupingUtils)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("ActorGroupingUtils unavailable"));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Ungroup Actors")));
    GroupingUtils->UngroupActors({GroupActor});

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("group"), GroupId);
    Data->SetNumberField(TEXT("count"), Count);
    return FCortexCommandRouter::Success(Data);
}
