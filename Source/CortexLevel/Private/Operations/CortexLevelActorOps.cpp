#include "Operations/CortexLevelActorOps.h"

#include "CortexLevelModule.h"
#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace
{
    bool TryParseVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue, FVector& OutVector)
    {
        OutVector = DefaultValue;

        const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, ArrayValue))
        {
            return true;
        }

        if (!ArrayValue || ArrayValue->Num() != 3)
        {
            return false;
        }

        OutVector.X = static_cast<float>((*ArrayValue)[0]->AsNumber());
        OutVector.Y = static_cast<float>((*ArrayValue)[1]->AsNumber());
        OutVector.Z = static_cast<float>((*ArrayValue)[2]->AsNumber());
        return true;
    }

    void SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Vector)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
        Json->SetArrayField(FieldName, Values);
    }
}

FCortexCommandResult FCortexLevelActorOps::SpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ClassIdentifier;
    if (!Params->TryGetStringField(TEXT("class"), ClassIdentifier) || ClassIdentifier.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Missing required parameter: class"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    UClass* ActorClass = FCortexLevelUtils::ResolveActorClass(ClassIdentifier, Error);
    if (!ActorClass)
    {
        return Error;
    }

    FVector Location;
    if (!TryParseVector(Params, TEXT("location"), FVector::ZeroVector, Location))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("location must be [x,y,z]"));
    }

    FVector RotationValues;
    if (!TryParseVector(Params, TEXT("rotation"), FVector::ZeroVector, RotationValues))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("rotation must be [pitch,yaw,roll]"));
    }

    FVector Scale;
    if (!TryParseVector(Params, TEXT("scale"), FVector(1.0f), Scale))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("scale must be [x,y,z]"));
    }

    const FRotator Rotation(RotationValues.X, RotationValues.Y, RotationValues.Z);

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Spawn Actor")));

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transactional;

    AActor* SpawnedActor = World->SpawnActor<AActor>(ActorClass, FTransform(Rotation, Location, Scale), SpawnParameters);
    if (!SpawnedActor)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::SpawnFailed,
            FString::Printf(TEXT("Failed to spawn actor of class: %s"), *ClassIdentifier)
        );
    }

    SpawnedActor->Modify();

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
    SetVectorArray(Data, TEXT("location"), SpawnedActor->GetActorLocation());
    SetVectorArray(Data, TEXT("rotation"), FVector(SpawnedActor->GetActorRotation().Pitch, SpawnedActor->GetActorRotation().Yaw, SpawnedActor->GetActorRotation().Roll));
    SetVectorArray(Data, TEXT("scale"), SpawnedActor->GetActorScale3D());
    Data->SetStringField(TEXT("folder"), SpawnedActor->GetFolderPath().ToString());

    return Result;
}

FCortexCommandResult FCortexLevelActorOps::DeleteActor(const TSharedPtr<FJsonObject>& Params)
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

    UEditorActorSubsystem* ActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSubsystem)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("EditorActorSubsystem unavailable"));
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Delete Actor")));
    const bool bDeleted = ActorSubsystem->DestroyActor(Actor);
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

    AActor* SourceActor = FCortexLevelUtils::FindActorByLabelOrPath(World, ActorIdentifier, Error);
    if (!SourceActor)
    {
        return Error;
    }

    FVector Offset;
    if (!TryParseVector(Params, TEXT("offset"), FVector::ZeroVector, Offset))
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
    SetVectorArray(Data, TEXT("location"), DuplicatedActor->GetActorLocation());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelActorOps::RenameActor(const TSharedPtr<FJsonObject>& Params)
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

    FString Label;
    if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: label"));
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

    FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Rename Actor")));
    Actor->Modify();
    Actor->SetActorLabel(Label);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Actor->GetName());
    Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    return FCortexCommandRouter::Success(Data);
}
