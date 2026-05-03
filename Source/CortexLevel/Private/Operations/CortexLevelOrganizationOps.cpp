#include "Operations/CortexLevelOrganizationOps.h"

#include "ActorGroupingUtils.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "Editor/GroupActor.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace
{
    TSharedPtr<FJsonObject> CopyLevelOrganizationBatchJsonObject(const TSharedPtr<FJsonObject>& Source)
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

    TArray<TSharedPtr<FJsonValue>> ToLevelOrganizationBatchJsonStringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    FCortexAssetFingerprint MakeLevelOrganizationActorFingerprint(const AActor* Actor)
    {
        if (!IsValid(Actor))
        {
            return MakeObjectAssetFingerprint(nullptr);
        }

        FString Signature = Actor->GetPathName();
        Signature += TEXT("|");
        Signature += Actor->GetFolderPath().ToString();
        if (const AActor* Parent = Actor->GetAttachParentActor())
        {
            Signature += TEXT("|");
            Signature += Parent->GetPathName();
        }
        for (const FName& Tag : Actor->Tags)
        {
            Signature += TEXT("|");
            Signature += Tag.ToString();
        }

        return MakeObjectAssetFingerprint(Actor, GetTypeHash(Signature));
    }

    TSharedPtr<FJsonObject> BuildLevelOrganizationBatchResponseData(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("status"), BatchResult.Status);
        Data->SetArrayField(TEXT("written_targets"), ToLevelOrganizationBatchJsonStringArray(BatchResult.WrittenTargets));
        Data->SetArrayField(TEXT("unwritten_targets"), ToLevelOrganizationBatchJsonStringArray(BatchResult.UnwrittenTargets));

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
                Entry->SetArrayField(TEXT("warnings"), ToLevelOrganizationBatchJsonStringArray(ItemResult.Result.Warnings));
            }
            PerItem.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("per_item"), PerItem);
        return Data;
    }

    FCortexCommandResult MakeLevelOrganizationBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = BuildLevelOrganizationBatchResponseData(BatchResult);
        if (BatchResult.Status == TEXT("committed"))
        {
            return FCortexCommandRouter::Success(Data);
        }

        return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
    }

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

    FCortexBatchPreflightResult PreflightLevelOrganizationActor(const FCortexBatchMutationItem& Item)
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

        return FCortexBatchPreflightResult::Success(MakeLevelOrganizationActorFingerprint(Actor).ToJson());
    }

    FCortexCommandResult CommitAttachActor(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelOrganizationBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelOrganizationOps::AttachActor(ItemParams);
    }

    FCortexCommandResult CommitDetachActor(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelOrganizationBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelOrganizationOps::DetachActor(ItemParams);
    }

    FCortexCommandResult CommitSetTags(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelOrganizationBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelOrganizationOps::SetTags(ItemParams);
    }

    FCortexCommandResult CommitSetFolder(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelOrganizationBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelOrganizationOps::SetFolder(ItemParams);
    }
}

FCortexCommandResult FCortexLevelOrganizationOps::AttachActor(const TSharedPtr<FJsonObject>& Params)
{
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }

        return MakeLevelOrganizationBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelOrganizationActor,
            CommitAttachActor));
    }

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
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }

        return MakeLevelOrganizationBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelOrganizationActor,
            CommitDetachActor));
    }

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
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }

        return MakeLevelOrganizationBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelOrganizationActor,
            CommitSetTags));
    }

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
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }

        return MakeLevelOrganizationBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelOrganizationActor,
            CommitSetFolder));
    }

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
