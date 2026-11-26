#include "Operations/CortexLevelActorOps.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexLevelModule.h"
#include "CortexLevelUtils.h"
#include "CortexPIEUtils.h"
#include "CortexTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace
{
    TSharedPtr<FJsonObject> CopyLevelActorBatchJsonObject(const TSharedPtr<FJsonObject>& Source)
    {
        TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
        if (!Source.IsValid())
        {
            return Copy;
        }

        for (const auto& Pair : Source->Values)
        {
            Copy->SetField(Pair.Key, Pair.Value);
        }

        return Copy;
    }

    TArray<TSharedPtr<FJsonValue>> ToLevelActorBatchJsonStringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    FCortexAssetFingerprint MakeLevelActorFingerprint(const AActor* Actor)
    {
        if (!IsValid(Actor))
        {
            return MakeObjectAssetFingerprint(nullptr);
        }

        FString Signature = Actor->GetPathName();
        Signature += TEXT("|");
        Signature += Actor->GetActorLabel();
        Signature += TEXT("|");
        Signature += Actor->GetClass()->GetPathName();
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

    TSharedPtr<FJsonObject> BuildLevelActorBatchResponseData(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("status"), BatchResult.Status);
        Data->SetArrayField(TEXT("written_targets"), ToLevelActorBatchJsonStringArray(BatchResult.WrittenTargets));
        Data->SetArrayField(TEXT("unwritten_targets"), ToLevelActorBatchJsonStringArray(BatchResult.UnwrittenTargets));

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
                Entry->SetArrayField(TEXT("warnings"), ToLevelActorBatchJsonStringArray(ItemResult.Result.Warnings));
            }
            PerItem.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("per_item"), PerItem);
        return Data;
    }

    FCortexCommandResult MakeLevelActorBatchCommandResult(const FCortexBatchMutationResult& BatchResult)
    {
        TSharedPtr<FJsonObject> Data = BuildLevelActorBatchResponseData(BatchResult);
        if (BatchResult.Status == TEXT("committed"))
        {
            return FCortexCommandRouter::Success(Data);
        }

        return FCortexCommandRouter::Error(BatchResult.ErrorCode, BatchResult.ErrorMessage, Data);
    }

    FCortexBatchPreflightResult PreflightLevelActorImpl(const FCortexBatchMutationItem& Item, bool bRequireEditor)
    {
        FCortexCommandResult Error;
        UWorld* World = FCortexLevelUtils::ResolveWorld(Item.Params, Error);
        if (!World)
        {
            return FCortexBatchPreflightResult::Error(Error.ErrorCode, Error.ErrorMessage, Error.ErrorDetails);
        }

        if (bRequireEditor && !FCortexLevelUtils::ShouldTransact(World))
        {
            return FCortexBatchPreflightResult::Error(
                CortexErrorCodes::InvalidOperation,
                TEXT("duplicate_actor does not support world 'pie'; use level.spawn_actor to create a runtime actor"));
        }

        AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, Item.Target, Error);
        if (!Actor)
        {
            return FCortexBatchPreflightResult::Error(Error.ErrorCode, Error.ErrorMessage, Error.ErrorDetails);
        }

        return FCortexBatchPreflightResult::Success(MakeLevelActorFingerprint(Actor).ToJson());
    }

    FCortexBatchPreflightResult PreflightLevelActor(const FCortexBatchMutationItem& Item)
    {
        return PreflightLevelActorImpl(Item, /*bRequireEditor=*/false);
    }

    FCortexBatchPreflightResult PreflightDuplicateActor(const FCortexBatchMutationItem& Item)
    {
        return PreflightLevelActorImpl(Item, /*bRequireEditor=*/true);
    }

    FCortexCommandResult CommitDeleteActor(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelActorBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelActorOps::DeleteActor(ItemParams);
    }

    FCortexCommandResult CommitDuplicateActor(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelActorBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelActorOps::DuplicateActor(ItemParams);
    }

    FCortexCommandResult CommitRenameActor(const FCortexBatchMutationItem& Item)
    {
        TSharedPtr<FJsonObject> ItemParams = CopyLevelActorBatchJsonObject(Item.Params);
        ItemParams->SetStringField(TEXT("actor"), Item.Target);
        return FCortexLevelActorOps::RenameActor(ItemParams);
    }
}

FCortexCommandResult FCortexLevelActorOps::SpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ClassIdentifier;
    if (!Params->TryGetStringField(TEXT("class_name"), ClassIdentifier) || ClassIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: class_name"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::ResolveWorld(Params, Error);
    if (!World)
    {
        return Error;
    }

    FCortexLevelPIETransactionScope PIEScope(World);

    UClass* ActorClass = FCortexLevelUtils::ResolveActorClass(ClassIdentifier, Error);
    if (!ActorClass)
    {
        return Error;
    }

    FVector Location;
    if (!FCortexLevelUtils::TryParseVector(Params, TEXT("location"), FVector::ZeroVector, Location))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("location must be [x,y,z]"));
    }

    FVector RotationValues;
    if (!FCortexLevelUtils::TryParseVector(Params, TEXT("rotation"), FVector::ZeroVector, RotationValues))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("rotation must be [pitch,yaw,roll]"));
    }

    FVector Scale;
    if (!FCortexLevelUtils::TryParseVector(Params, TEXT("scale"), FVector(1.0f), Scale))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("scale must be [x,y,z]"));
    }

    const FRotator Rotation(RotationValues.X, RotationValues.Y, RotationValues.Z);

    const bool bTransact = FCortexLevelUtils::ShouldTransact(World);
    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Spawn Actor")), bTransact);

    FActorSpawnParameters SpawnParameters;
    if (bTransact)
    {
        SpawnParameters.ObjectFlags |= RF_Transactional;
    }

    FString LevelName;
    if (Params->TryGetStringField(TEXT("level"), LevelName) && !LevelName.IsEmpty())
    {
        ULevel* TargetLevel = FCortexLevelUtils::ResolveSublevel(World, LevelName, Error);
        if (!TargetLevel)
        {
            return Error;
        }
        SpawnParameters.OverrideLevel = TargetLevel;
    }

    AActor* SpawnedActor = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location, Scale), SpawnParameters);
    if (!SpawnedActor)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::SpawnFailed,
            FString::Printf(TEXT("Failed to spawn actor of class: %s"), *ClassIdentifier)
        );
    }

    if (bTransact)
    {
        SpawnedActor->Modify();
    }

    FString Label;
    if (Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
    {
        SpawnedActor->SetActorLabel(Label);
    }

    FString Folder;
    if (Params->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
    {
        SpawnedActor->SetFolderPath(FName(*Folder));
    }

    FCortexCommandResult Result = FCortexCommandRouter::Success(MakeShared<FJsonObject>());

    FString MeshPath;
    if (Params->TryGetStringField(TEXT("mesh"), MeshPath) && !MeshPath.IsEmpty())
    {
        if (UStaticMeshComponent* MeshComponent = SpawnedActor->FindComponentByClass<UStaticMeshComponent>())
        {
            if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath))
            {
                MeshComponent->SetStaticMesh(Mesh);
            }
            else
            {
                Result.Warnings.Add(FString::Printf(TEXT("Could not load mesh: %s"), *MeshPath));
            }
        }
        else
        {
            Result.Warnings.Add(TEXT("Actor has no StaticMeshComponent; mesh parameter ignored"));
        }
    }

    FString MaterialPath;
    if (Params->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
    {
        if (UStaticMeshComponent* MeshComponent = SpawnedActor->FindComponentByClass<UStaticMeshComponent>())
        {
            if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath))
            {
                MeshComponent->SetMaterial(0, Material);
            }
            else
            {
                Result.Warnings.Add(FString::Printf(TEXT("Could not load material: %s"), *MaterialPath));
            }
        }
        else
        {
            Result.Warnings.Add(TEXT("Actor has no StaticMeshComponent; material parameter ignored"));
        }
    }

    TSharedPtr<FJsonObject> Data = Result.Data;
    Data->SetStringField(TEXT("name"), SpawnedActor->GetName());
    Data->SetStringField(TEXT("label"), SpawnedActor->GetActorLabel());
    Data->SetStringField(TEXT("class"), SpawnedActor->GetClass()->GetName());
    FCortexLevelUtils::SetVectorArray(Data, TEXT("location"), SpawnedActor->GetActorLocation());
    FCortexLevelUtils::SetVectorArray(Data, TEXT("rotation"), FVector(SpawnedActor->GetActorRotation().Pitch, SpawnedActor->GetActorRotation().Yaw, SpawnedActor->GetActorRotation().Roll));
    FCortexLevelUtils::SetVectorArray(Data, TEXT("scale"), SpawnedActor->GetActorScale3D());
    Data->SetStringField(TEXT("folder"), SpawnedActor->GetFolderPath().ToString());

    return Result;
}

FCortexCommandResult FCortexLevelActorOps::DeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }
        FCortexCommandResult WorldError;
        if (!FCortexLevelUtils::ApplyWorldToBatchRequest(Params, Request, WorldError))
        {
            return WorldError;
        }

        return MakeLevelActorBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelActor,
            CommitDeleteActor));
    }

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

    FString ConfirmClass;
    if (Params->TryGetStringField(TEXT("confirm_class"), ConfirmClass) && !ConfirmClass.IsEmpty())
    {
        if (Actor->GetClass()->GetName() != ConfirmClass && Actor->GetClass()->GetPathName() != ConfirmClass)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidValue,
                FString::Printf(TEXT("confirm_class mismatch: expected %s, got %s"), *ConfirmClass, *Actor->GetClass()->GetName())
            );
        }
    }

    FString ActorName = Actor->GetName();
    FString ActorLabel = Actor->GetActorLabel();
    FString ActorClass = Actor->GetClass()->GetName();

    FCortexLevelPIETransactionScope PIEScope(World);

    bool bDeleted = false;
    if (FCortexLevelUtils::ShouldTransact(World))
    {
        UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
        if (!ActorSubsystem)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("EditorActorSubsystem unavailable"));
        }

        FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Delete Actor")));
        bDeleted = ActorSubsystem->DestroyActor(Actor);
    }
    else
    {
        // UEditorActorSubsystem::DestroyActor refuses non-editor worlds.
        bDeleted = World->DestroyActor(Actor, /*bNetForce=*/false, /*bShouldModifyLevel=*/false);
    }

    if (!bDeleted)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidOperation,
            FString::Printf(TEXT("Failed to delete actor: %s"), *ActorIdentifier)
        );
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), ActorName);
    Data->SetStringField(TEXT("label"), ActorLabel);
    Data->SetStringField(TEXT("class"), ActorClass);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelActorOps::DuplicateActor(const TSharedPtr<FJsonObject>& Params)
{
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }
        FCortexCommandResult WorldError;
        if (!FCortexLevelUtils::ApplyWorldToBatchRequest(Params, Request, WorldError))
        {
            return WorldError;
        }

        return MakeLevelActorBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightDuplicateActor,
            CommitDuplicateActor));
    }

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

    AActor* SourceActor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!SourceActor)
    {
        return Error;
    }

    if (!FCortexLevelUtils::ShouldTransact(World))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidOperation,
            TEXT("duplicate_actor does not support world 'pie'; use level.spawn_actor to create a runtime actor"));
    }

    FVector Offset;
    if (!FCortexLevelUtils::TryParseVector(Params, TEXT("offset"), FVector::ZeroVector, Offset))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("offset must be [x,y,z]"));
    }

    UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSubsystem)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("EditorActorSubsystem unavailable"));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Duplicate Actor")));

    AActor* DuplicatedActor = ActorSubsystem->DuplicateActor(SourceActor, World, Offset);
    if (!DuplicatedActor)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidOperation,
            FString::Printf(TEXT("Failed to duplicate actor: %s"), *ActorIdentifier)
        );
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), DuplicatedActor->GetName());
    Data->SetStringField(TEXT("label"), DuplicatedActor->GetActorLabel());
    Data->SetStringField(TEXT("class"), DuplicatedActor->GetClass()->GetName());
    FCortexLevelUtils::SetVectorArray(Data, TEXT("location"), DuplicatedActor->GetActorLocation());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelActorOps::RenameActor(const TSharedPtr<FJsonObject>& Params)
{
    if (Params.IsValid() && Params->HasField(TEXT("items")))
    {
        FCortexBatchMutationRequest Request;
        FCortexCommandResult ParseError;
        if (!FCortexBatchMutation::ParseRequest(Params, TEXT("actor"), Request, ParseError))
        {
            return ParseError;
        }
        FCortexCommandResult WorldError;
        if (!FCortexLevelUtils::ApplyWorldToBatchRequest(Params, Request, WorldError))
        {
            return WorldError;
        }

        return MakeLevelActorBatchCommandResult(FCortexBatchMutation::Run(
            Request,
            PreflightLevelActor,
            CommitRenameActor));
    }

    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    FString ActorIdentifier;
    if (!Params->TryGetStringField(TEXT("actor"), ActorIdentifier) || ActorIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: actor"));
    }

    FString Label;
    if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required parameter: label"));
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

    FCortexLevelPIETransactionScope PIEScope(World);

    FScopedTransaction Transaction(
        FText::FromString(TEXT("Cortex: Rename Actor")),
        FCortexLevelUtils::ShouldTransact(World));
    Actor->Modify();
    Actor->SetActorLabel(Label);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelActorOps::SetPIEView(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
    }

    UWorld* PlayWorld = FCortexPIEUtils::GetPIEWorld();
    if (PlayWorld == nullptr)
    {
        return FCortexPIEUtils::PIENotActiveError();
    }

    FCortexLevelPIETransactionScope PIEScope(PlayWorld);

    APlayerController* PlayerController = PlayWorld->GetFirstPlayerController();
    if (PlayerController == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidOperation,
            TEXT("PIE world has no player controller; start PIE with a local player"));
    }

    FVector Location;
    if (!FCortexLevelUtils::ParseVectorField(Params, TEXT("location"), Location))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("location must be [x,y,z]"));
    }

    FVector RotationValues;
    if (!FCortexLevelUtils::ParseVectorField(Params, TEXT("rotation"), RotationValues))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidValue,
            TEXT("rotation must be [pitch,yaw,roll]"));
    }

    double FovValue = 90.0;
    if (Params->HasField(TEXT("fov")))
    {
        if (!Params->TryGetNumberField(TEXT("fov"), FovValue)
            || !FMath::IsFinite(FovValue)
            || FovValue <= 0.0
            || FovValue >= 180.0)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidValue,
                TEXT("fov must be a number greater than 0 and less than 180"));
        }
    }

    const FRotator Rotation(RotationValues.X, RotationValues.Y, RotationValues.Z);

    static const FName CameraTag(TEXT("CortexPIEViewCamera"));
    ACameraActor* Camera = nullptr;
    for (TActorIterator<ACameraActor> It(PlayWorld); It; ++It)
    {
        if (IsValid(*It) && It->ActorHasTag(CameraTag))
        {
            Camera = *It;
            break;
        }
    }

    const bool bReused = Camera != nullptr;
    if (Camera == nullptr)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SpawnParameters.ObjectFlags |= RF_Transient;
        Camera = PlayWorld->SpawnActor<ACameraActor>(
            ACameraActor::StaticClass(),
            FTransform(Rotation, Location),
            SpawnParameters);
        if (Camera == nullptr)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::SpawnFailed,
                TEXT("Failed to spawn the PIE view camera"));
        }

        Camera->Tags.AddUnique(CameraTag);
        Camera->SetActorLabel(TEXT("CortexPIEViewCamera"));
    }
    else
    {
        Camera->SetActorLocationAndRotation(Location, Rotation);
    }

    UCameraComponent* CameraComponent = Camera->GetCameraComponent();
    if (CameraComponent == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidOperation,
            TEXT("The PIE view camera has no camera component"));
    }
    CameraComponent->SetFieldOfView(static_cast<float>(FovValue));

    PlayerController->SetViewTargetWithBlend(Camera, 0.0f);
    if (PlayerController->PlayerCameraManager != nullptr)
    {
        PlayerController->PlayerCameraManager->UpdateCamera(0.0f);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("camera"), Camera->GetName());
    Data->SetStringField(TEXT("label"), Camera->GetActorLabel());
    Data->SetBoolField(TEXT("reused"), bReused);
    FCortexLevelUtils::SetVectorArray(Data, TEXT("location"), Camera->GetActorLocation());
    const FRotator CameraRotation = Camera->GetActorRotation();
    FCortexLevelUtils::SetVectorArray(
        Data,
        TEXT("rotation"),
        FVector(CameraRotation.Pitch, CameraRotation.Yaw, CameraRotation.Roll));
    Data->SetNumberField(TEXT("fov"), CameraComponent->FieldOfView);
    Data->SetBoolField(TEXT("view_target_set"), PlayerController->GetViewTarget() == Camera);
    return FCortexCommandRouter::Success(Data);
}
