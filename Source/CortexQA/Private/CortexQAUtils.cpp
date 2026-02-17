#include "CortexQAUtils.h"

#include "CortexTypes.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"

UWorld* FCortexQAUtils::GetPIEWorld()
{
    if (GEditor == nullptr)
    {
        return nullptr;
    }

    return GEditor->PlayWorld;
}

APlayerController* FCortexQAUtils::GetPlayerController(UWorld* World)
{
    if (World == nullptr)
    {
        return nullptr;
    }

    return World->GetFirstPlayerController();
}

AActor* FCortexQAUtils::FindActorByName(UWorld* World, const FString& ActorIdentifier)
{
    if (World == nullptr || ActorIdentifier.IsEmpty())
    {
        return nullptr;
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }

        if (Actor->GetActorLabel() == ActorIdentifier ||
            Actor->GetName() == ActorIdentifier ||
            Actor->GetPathName() == ActorIdentifier)
        {
            return Actor;
        }
    }

    return nullptr;
}

FCortexCommandResult FCortexQAUtils::PIENotActiveError()
{
    return FCortexCommandRouter::Error(
        CortexErrorCodes::PIENotActive,
        TEXT("PIE is not running. Start PIE before using QA commands."));
}

void FCortexQAUtils::SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Value)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Add(MakeShared<FJsonValueNumber>(Value.X));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Y));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Z));
    Json->SetArrayField(FieldName, Array);
}

void FCortexQAUtils::SetRotatorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FRotator& Value)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Add(MakeShared<FJsonValueNumber>(Value.Pitch));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Yaw));
    Array.Add(MakeShared<FJsonValueNumber>(Value.Roll));
    Json->SetArrayField(FieldName, Array);
}
