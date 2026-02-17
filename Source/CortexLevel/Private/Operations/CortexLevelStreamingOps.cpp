#include "Operations/CortexLevelStreamingOps.h"

#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Dom/JsonValue.h"
#include "Engine/LevelStreaming.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

namespace
{
    FString WorldTypeToString(EWorldType::Type Type)
    {
        switch (Type)
        {
        case EWorldType::Game: return TEXT("Game");
        case EWorldType::Editor: return TEXT("Editor");
        case EWorldType::PIE: return TEXT("PIE");
        case EWorldType::EditorPreview: return TEXT("EditorPreview");
        case EWorldType::GamePreview: return TEXT("GamePreview");
        case EWorldType::GameRPC: return TEXT("GameRPC");
        case EWorldType::Inactive: return TEXT("Inactive");
        default: return TEXT("None");
        }
    }

    ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& NameOrPath)
    {
        if (!World)
        {
            return nullptr;
        }

        for (ULevelStreaming* Streaming : World->GetStreamingLevels())
        {
            if (!Streaming)
            {
                continue;
            }

            const FString PackageName = Streaming->GetWorldAssetPackageName();
            const FString ShortName = FPackageName::GetShortName(PackageName);
            if (PackageName == NameOrPath || ShortName == NameOrPath)
            {
                return Streaming;
            }
        }

        return nullptr;
    }
}

FCortexCommandResult FCortexLevelStreamingOps::GetInfo(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    int32 ActorCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsValid(*It))
        {
            ++ActorCount;
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("level_name"), World->GetMapName());
    Data->SetStringField(TEXT("level_path"), World->GetOutermost() ? World->GetOutermost()->GetName() : TEXT(""));
    Data->SetStringField(TEXT("world_type"), WorldTypeToString(World->WorldType));
    Data->SetNumberField(TEXT("actor_count"), ActorCount);
    Data->SetBoolField(TEXT("is_world_partition"), World->IsPartitionedWorld());

    FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
    if (GEditor && GEditor->IsPlaySessionInProgress())
    {
        Result.Warnings.Add(TEXT("PIE is active; level state may be transient"));
    }

    return Result;
}

FCortexCommandResult FCortexLevelStreamingOps::ListSublevels(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Levels;
    for (ULevelStreaming* Streaming : World->GetStreamingLevels())
    {
        if (!Streaming)
        {
            continue;
        }

        TSharedPtr<FJsonObject> LevelJson = MakeShared<FJsonObject>();
        LevelJson->SetStringField(TEXT("name"), FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
        LevelJson->SetStringField(TEXT("path"), Streaming->GetWorldAssetPackageName());
        LevelJson->SetBoolField(TEXT("loaded"), Streaming->IsLevelLoaded());
        LevelJson->SetBoolField(TEXT("visible"), Streaming->GetShouldBeVisibleFlag());
        LevelJson->SetStringField(TEXT("streaming_method"), Streaming->GetClass()->GetName());
        Levels.Add(MakeShared<FJsonValueObject>(LevelJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("sublevels"), Levels);
    Data->SetNumberField(TEXT("count"), Levels.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::LoadSublevel(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString LevelName;
    if (!Params->TryGetStringField(TEXT("level"), LevelName) || LevelName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, TEXT("Missing required parameter: level"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    ULevelStreaming* Streaming = FindStreamingLevel(World, LevelName);
    if (!Streaming)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, FString::Printf(TEXT("Sublevel not found: %s"), *LevelName));
    }

    Streaming->SetShouldBeLoaded(true);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("level"), FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
    Data->SetBoolField(TEXT("loaded"), true);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::UnloadSublevel(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString LevelName;
    if (!Params->TryGetStringField(TEXT("level"), LevelName) || LevelName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, TEXT("Missing required parameter: level"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    ULevelStreaming* Streaming = FindStreamingLevel(World, LevelName);
    if (!Streaming)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, FString::Printf(TEXT("Sublevel not found: %s"), *LevelName));
    }

    Streaming->SetShouldBeLoaded(false);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("level"), FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
    Data->SetBoolField(TEXT("loaded"), false);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::SetSublevelVisibility(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString LevelName;
    bool bVisible = true;
    if (!Params->TryGetStringField(TEXT("level"), LevelName) || LevelName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, TEXT("Missing required parameter: level"));
    }
    if (!Params->TryGetBoolField(TEXT("visible"), bVisible))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: visible"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    ULevelStreaming* Streaming = FindStreamingLevel(World, LevelName);
    if (!Streaming)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::SublevelNotFound, FString::Printf(TEXT("Sublevel not found: %s"), *LevelName));
    }

    Streaming->SetShouldBeVisible(bVisible);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("level"), FPackageName::GetShortName(Streaming->GetWorldAssetPackageName()));
    Data->SetBoolField(TEXT("visible"), bVisible);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::ListDataLayers(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    if (!GEditor)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
    }

    UDataLayerEditorSubsystem* DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
    if (!DataLayerSubsystem)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("DataLayerEditorSubsystem unavailable"));
    }

    TArray<UDataLayerInstance*> Layers = DataLayerSubsystem->GetAllDataLayers();
    TArray<TSharedPtr<FJsonValue>> LayerValues;

    for (UDataLayerInstance* Layer : Layers)
    {
        if (!IsValid(Layer))
        {
            continue;
        }

        TSharedPtr<FJsonObject> LayerJson = MakeShared<FJsonObject>();
        LayerJson->SetStringField(TEXT("name"), Layer->GetDataLayerShortName());
        LayerJson->SetStringField(TEXT("full_name"), Layer->GetDataLayerFullName());
        LayerJson->SetBoolField(TEXT("loaded"), Layer->IsEffectiveLoadedInEditor());
        LayerJson->SetBoolField(TEXT("visible"), Layer->IsVisible());
        LayerValues.Add(MakeShared<FJsonValueObject>(LayerJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("data_layers"), LayerValues);
    Data->SetNumberField(TEXT("count"), LayerValues.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::SetDataLayer(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString ActorId;
    FString DataLayerName;
    if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("Missing required parameter: actor"));
    }
    if (!Params->TryGetStringField(TEXT("data_layer"), DataLayerName) || DataLayerName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::DataLayerNotFound, TEXT("Missing required parameter: data_layer"));
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

    if (!GEditor)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
    }

    UDataLayerEditorSubsystem* DataLayerSubsystem = GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>();
    if (!DataLayerSubsystem)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("DataLayerEditorSubsystem unavailable"));
    }

    UDataLayerInstance* Layer = DataLayerSubsystem->GetDataLayerInstance(FName(*DataLayerName));
    if (!Layer)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::DataLayerNotFound, FString::Printf(TEXT("Data layer not found: %s"), *DataLayerName));
    }

    if (!DataLayerSubsystem->AddActorToDataLayer(Actor, Layer))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to assign actor to data layer"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    Data->SetStringField(TEXT("data_layer"), Layer->GetDataLayerShortName());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::SaveLevel(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    ULevel* PersistentLevel = World->PersistentLevel;
    if (!PersistentLevel)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Persistent level unavailable"));
    }

    const bool bSaved = FEditorFileUtils::SaveLevel(PersistentLevel);
    if (!bSaved)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to save current level"));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("level"), World->GetMapName());
    Data->SetBoolField(TEXT("saved"), true);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelStreamingOps::SaveAll(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    const bool bSaved = FEditorFileUtils::SaveDirtyPackages(
        /*bPromptUserToSave*/false,
        /*bSaveMapPackages*/true,
        /*bSaveContentPackages*/true,
        /*bFastSave*/true
    );

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("saved"), bSaved);
    return FCortexCommandRouter::Success(Data);
}
