#include "Operations/CortexLevelQueryOps.h"

#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
    struct FRegionFilter
    {
        bool bEnabled = false;
        bool bSphere = false;
        FVector Center = FVector::ZeroVector;
        FVector Extent = FVector::ZeroVector;
        float Radius = 0.0f;
    };

    bool ParseVectorArray(const TSharedPtr<FJsonObject>& Json, const TCHAR* FieldName, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Json->TryGetArrayField(FieldName, Arr) || !Arr || Arr->Num() != 3)
        {
            return false;
        }

        Out.X = static_cast<float>((*Arr)[0]->AsNumber());
        Out.Y = static_cast<float>((*Arr)[1]->AsNumber());
        Out.Z = static_cast<float>((*Arr)[2]->AsNumber());
        return true;
    }

    void SetQueryVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Vec)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(Vec.X));
        Arr.Add(MakeShared<FJsonValueNumber>(Vec.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(Vec.Z));
        Json->SetArrayField(FieldName, Arr);
    }

    bool ParseRegionFilter(const TSharedPtr<FJsonObject>& Params, FRegionFilter& OutRegion)
    {
        const TSharedPtr<FJsonObject>* RegionObj = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("region"), RegionObj) || !RegionObj || !RegionObj->IsValid())
        {
            return true;
        }

        FString Type;
        if (!(*RegionObj)->TryGetStringField(TEXT("type"), Type))
        {
            return false;
        }

        Type = Type.ToLower();
        OutRegion.bEnabled = true;

        if (Type == TEXT("sphere"))
        {
            OutRegion.bSphere = true;
            if (!ParseVectorArray(*RegionObj, TEXT("center"), OutRegion.Center))
            {
                return false;
            }
            double Radius = 0.0;
            if (!(*RegionObj)->TryGetNumberField(TEXT("radius"), Radius))
            {
                return false;
            }
            OutRegion.Radius = static_cast<float>(Radius);
            return true;
        }

        if (Type == TEXT("box"))
        {
            OutRegion.bSphere = false;
            if (!ParseVectorArray(*RegionObj, TEXT("center"), OutRegion.Center))
            {
                return false;
            }
            if (!ParseVectorArray(*RegionObj, TEXT("extent"), OutRegion.Extent))
            {
                return false;
            }
            return true;
        }

        return false;
    }

    bool PassesTagsFilter(AActor* Actor, const TArray<FString>& RequiredTags)
    {
        for (const FString& Tag : RequiredTags)
        {
            if (!Actor->Tags.Contains(FName(*Tag)))
            {
                return false;
            }
        }
        return true;
    }

    bool PassesRegionFilter(AActor* Actor, const FRegionFilter& Region)
    {
        if (!Region.bEnabled)
        {
            return true;
        }

        const FVector Location = Actor->GetActorLocation();
        if (Region.bSphere)
        {
            return FVector::DistSquared(Location, Region.Center) <= FMath::Square(Region.Radius);
        }

        const FVector Delta = Location - Region.Center;
        return FMath::Abs(Delta.X) <= Region.Extent.X &&
            FMath::Abs(Delta.Y) <= Region.Extent.Y &&
            FMath::Abs(Delta.Z) <= Region.Extent.Z;
    }

    TArray<AActor*> CollectFilteredActors(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
    {
        FString ClassFilter;
        Params->TryGetStringField(TEXT("class"), ClassFilter);

        UClass* FilterClass = nullptr;
        if (!ClassFilter.IsEmpty())
        {
            FilterClass = FCortexLevelUtils::ResolveActorClass(ClassFilter, OutError);
            if (!FilterClass)
            {
                return {};
            }
        }

        TArray<FString> RequiredTags;
        const TArray<TSharedPtr<FJsonValue>>* TagArray = nullptr;
        if (Params->TryGetArrayField(TEXT("tags"), TagArray) && TagArray)
        {
            for (const TSharedPtr<FJsonValue>& Tag : *TagArray)
            {
                RequiredTags.Add(Tag->AsString());
            }
        }

        FString FolderFilter;
        Params->TryGetStringField(TEXT("folder"), FolderFilter);

        FRegionFilter Region;
        if (!ParseRegionFilter(Params, Region))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Invalid region filter"));
            return {};
        }

        TArray<AActor*> Actors;
        int32 ScannedCount = 0;
        constexpr int32 MaxScanCount = 10000;

        for (TActorIterator<AActor> It(World, FilterClass ? FilterClass : AActor::StaticClass()); It; ++It)
        {
            if (++ScannedCount > MaxScanCount)
            {
                break;
            }

            AActor* Actor = *It;
            if (!IsValid(Actor))
            {
                continue;
            }

            if (!RequiredTags.IsEmpty() && !PassesTagsFilter(Actor, RequiredTags))
            {
                continue;
            }

            if (!FolderFilter.IsEmpty() && Actor->GetFolderPath().ToString() != FolderFilter)
            {
                continue;
            }

            if (!PassesRegionFilter(Actor, Region))
            {
                continue;
            }

            Actors.Add(Actor);
        }

        Actors.Sort([](const AActor& A, const AActor& B)
        {
            return A.GetActorLabel() < B.GetActorLabel();
        });

        return Actors;
    }

    TSharedPtr<FJsonObject> ToSummary(AActor* Actor)
    {
        return FCortexLevelUtils::SerializeActorSummary(Actor);
    }
}

FCortexCommandResult FCortexLevelQueryOps::ListActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    TArray<AActor*> Actors = CollectFilteredActors(World, Params, Error);
    if (!Error.ErrorCode.IsEmpty())
    {
        return Error;
    }

    int32 Offset = 0;
    int32 Limit = 100;
    Params->TryGetNumberField(TEXT("offset"), Offset);
    Params->TryGetNumberField(TEXT("limit"), Limit);

    Offset = FMath::Max(0, Offset);
    Limit = FMath::Clamp(Limit, 1, 1000);

    const int32 Total = Actors.Num();
    const int32 End = FMath::Min(Total, Offset + Limit);

    TArray<TSharedPtr<FJsonValue>> Results;
    for (int32 Index = Offset; Index < End; ++Index)
    {
        Results.Add(MakeShared<FJsonValueObject>(ToSummary(Actors[Index])));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("actors"), Results);
    Data->SetNumberField(TEXT("count"), Results.Num());
    Data->SetNumberField(TEXT("total"), Total);
    Data->SetNumberField(TEXT("offset"), Offset);
    Data->SetNumberField(TEXT("limit"), Limit);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelQueryOps::FindActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: pattern"));
    }

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Matches;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }

        const FString Label = Actor->GetActorLabel();
        const FString Name = Actor->GetName();
        if (Label.MatchesWildcard(Pattern) || Name.MatchesWildcard(Pattern))
        {
            Matches.Add(MakeShared<FJsonValueObject>(ToSummary(Actor)));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("matches"), Matches);
    Data->SetNumberField(TEXT("count"), Matches.Num());
    Data->SetStringField(TEXT("pattern"), Pattern);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelQueryOps::GetBounds(const TSharedPtr<FJsonObject>& Params)
{
    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    TArray<AActor*> Actors = CollectFilteredActors(World, Params ? Params : MakeShared<FJsonObject>(), Error);
    if (!Error.ErrorCode.IsEmpty())
    {
        return Error;
    }

    if (Actors.Num() == 0)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, TEXT("No actors matched filters"));
    }

    FVector Min = FVector(FLT_MAX);
    FVector Max = FVector(-FLT_MAX);

    for (AActor* Actor : Actors)
    {
        const FVector Location = Actor->GetActorLocation();
        Min.X = FMath::Min(Min.X, Location.X);
        Min.Y = FMath::Min(Min.Y, Location.Y);
        Min.Z = FMath::Min(Min.Z, Location.Z);

        Max.X = FMath::Max(Max.X, Location.X);
        Max.Y = FMath::Max(Max.Y, Location.Y);
        Max.Z = FMath::Max(Max.Z, Location.Z);
    }

    const FVector Center = (Min + Max) * 0.5f;
    const FVector Extent = (Max - Min) * 0.5f;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    SetQueryVectorArray(Data, TEXT("min"), Min);
    SetQueryVectorArray(Data, TEXT("max"), Max);
    SetQueryVectorArray(Data, TEXT("center"), Center);
    SetQueryVectorArray(Data, TEXT("extent"), Extent);
    Data->SetNumberField(TEXT("actor_count"), Actors.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelQueryOps::SelectActors(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
    }

    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing params"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("actors"), ActorValues) || !ActorValues)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, TEXT("Missing required parameter: actors"));
    }

    bool bAdd = false;
    Params->TryGetBoolField(TEXT("add"), bAdd);

    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        return Error;
    }

    if (!bAdd)
    {
        GEditor->SelectNone(false, true, false);
    }

    TArray<TSharedPtr<FJsonValue>> Selected;
    for (const TSharedPtr<FJsonValue>& Value : *ActorValues)
    {
        const FString Identifier = Value->AsString();
        FCortexCommandResult FindError;
        AActor* Actor = FCortexLevelUtils::FindActorByLabelOrPath(World, Identifier, FindError);
        if (!Actor)
        {
            continue;
        }

        GEditor->SelectActor(Actor, true, false, true);
        Selected.Add(MakeShared<FJsonValueObject>(ToSummary(Actor)));
    }

    GEditor->NoteSelectionChange();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("selection"), Selected);
    Data->SetNumberField(TEXT("count"), Selected.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelQueryOps::GetSelection(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    if (!GEditor)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("GEditor unavailable"));
    }

    USelection* Selection = GEditor->GetSelectedActors();
    if (!Selection)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("Selection unavailable"));
    }

    TArray<TSharedPtr<FJsonValue>> Actors;
    for (FSelectionIterator It(*Selection); It; ++It)
    {
        AActor* Actor = Cast<AActor>(*It);
        if (!IsValid(Actor))
        {
            continue;
        }

        Actors.Add(MakeShared<FJsonValueObject>(ToSummary(Actor)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("selection"), Actors);
    Data->SetNumberField(TEXT("count"), Actors.Num());
    return FCortexCommandRouter::Success(Data);
}
