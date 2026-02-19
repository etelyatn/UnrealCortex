#include "CortexLevelUtils.h"

#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

UWorld* FCortexLevelUtils::GetEditorWorld(FCortexCommandResult& OutError)
{
    if (!GEngine)
    {
        OutError = FCortexCommandRouter::Error(
            CortexErrorCodes::EditorNotReady,
            TEXT("Engine is not available")
        );
        return nullptr;
    }

    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if ((Context.WorldType == EWorldType::Editor || Context.WorldType == EWorldType::EditorPreview) && Context.World())
        {
            return Context.World();
        }
    }

    OutError = FCortexCommandRouter::Error(
        CortexErrorCodes::EditorNotReady,
        TEXT("No editor world available")
    );
    return nullptr;
}

AActor* FCortexLevelUtils::FindActorByLabelOrPath(UWorld* World, const FString& ActorIdentifier, FCortexCommandResult& OutError)
{
    if (!World)
    {
        OutError = FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("World is not available"));
        return nullptr;
    }

    TArray<AActor*> LabelMatches;
    AActor* NameMatch = nullptr;
    AActor* PathMatch = nullptr;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }

        if (Actor->GetActorLabel() == ActorIdentifier)
        {
            LabelMatches.Add(Actor);
        }

        if (!NameMatch && Actor->GetFName().ToString() == ActorIdentifier)
        {
            NameMatch = Actor;
        }

        if (!PathMatch && Actor->GetPathName() == ActorIdentifier)
        {
            PathMatch = Actor;
        }
    }

    if (LabelMatches.Num() > 1)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Matches;
        for (AActor* Match : LabelMatches)
        {
            Matches.Add(MakeShared<FJsonValueString>(Match->GetPathName()));
        }
        Details->SetArrayField(TEXT("matches"), Matches);

        OutError = FCortexCommandRouter::Error(
            CortexErrorCodes::AmbiguousActor,
            FString::Printf(TEXT("Multiple actors found with label: %s"), *ActorIdentifier),
            Details
        );
        return nullptr;
    }

    if (LabelMatches.Num() == 1)
    {
        return LabelMatches[0];
    }

    if (NameMatch)
    {
        return NameMatch;
    }

    if (PathMatch)
    {
        return PathMatch;
    }

    OutError = FCortexCommandRouter::Error(
        CortexErrorCodes::ActorNotFound,
        FString::Printf(TEXT("Actor not found: %s"), *ActorIdentifier)
    );
    return nullptr;
}

UClass* FCortexLevelUtils::ResolveActorClass(const FString& ClassIdentifier, FCortexCommandResult& OutError)
{
    if (ClassIdentifier.IsEmpty())
    {
        OutError = FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Class identifier is empty"));
        return nullptr;
    }

    UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassIdentifier);

    if (!ResolvedClass && !ClassIdentifier.StartsWith(TEXT("/")))
    {
        const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassIdentifier);
        ResolvedClass = FindObject<UClass>(nullptr, *EnginePath);
    }

    if (!ResolvedClass)
    {
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!IsValid(Candidate))
            {
                continue;
            }

            if (Candidate->GetName() == ClassIdentifier || Candidate->GetPathName() == ClassIdentifier)
            {
                ResolvedClass = Candidate;
                break;
            }
        }
    }

    if (!ResolvedClass)
    {
        const FString PkgName = FPackageName::ObjectPathToPackageName(ClassIdentifier);
        // Only call DoesPackageExist for valid mount-point paths (starting with "/") —
        // bare class names like "NonExistentClass_XYZ" are not valid package paths and
        // will trigger a LogPackageName warning inside DoesPackageExist.
        if (PkgName.StartsWith(TEXT("/")) && (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName)))
        {
            UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ClassIdentifier);
            if (Blueprint)
            {
                ResolvedClass = Blueprint->GeneratedClass;
            }
        }
    }

    if (!ResolvedClass || !ResolvedClass->IsChildOf(AActor::StaticClass()))
    {
        OutError = FCortexCommandRouter::Error(
            CortexErrorCodes::ClassNotFound,
            FString::Printf(TEXT("Actor class not found: %s"), *ClassIdentifier)
        );
        return nullptr;
    }

    return ResolvedClass;
}

TSharedPtr<FJsonObject> FCortexLevelUtils::SerializeActorSummary(AActor* Actor)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return Json;
    }

    Json->SetStringField(TEXT("name"), Actor->GetName());
    Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
    Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

    SetVectorArray(Json, TEXT("location"), Actor->GetActorLocation());

    Json->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());

    TArray<TSharedPtr<FJsonValue>> Tags;
    for (const FName& Tag : Actor->Tags)
    {
        Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
    }
    Json->SetArrayField(TEXT("tags"), Tags);

    return Json;
}

bool FCortexLevelUtils::TryParseVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue, FVector& OutVector)
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

bool FCortexLevelUtils::ParseVectorField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutVector)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Params->TryGetArrayField(FieldName, Values))
    {
        return false;
    }

    if (!Values || Values->Num() != 3)
    {
        return false;
    }

    OutVector.X = static_cast<float>((*Values)[0]->AsNumber());
    OutVector.Y = static_cast<float>((*Values)[1]->AsNumber());
    OutVector.Z = static_cast<float>((*Values)[2]->AsNumber());
    return true;
}

void FCortexLevelUtils::SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Vector)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
    Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
    Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
    Json->SetArrayField(FieldName, Values);
}
