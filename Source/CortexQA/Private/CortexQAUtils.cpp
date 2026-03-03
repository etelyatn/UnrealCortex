#include "CortexQAUtils.h"

#include "CortexEditorUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Info.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"

UWorld* FCortexQAUtils::GetPIEWorld()
{
    return FCortexEditorUtils::GetPIEWorld();
}

APlayerController* FCortexQAUtils::GetPlayerController(UWorld* World)
{
    if (World == nullptr)
    {
        return nullptr;
    }

    return World->GetFirstPlayerController();
}

APawn* FCortexQAUtils::GetPlayerPawn(UWorld* World)
{
    APlayerController* PC = GetPlayerController(World);
    return (PC != nullptr) ? PC->GetPawn() : nullptr;
}

AActor* FCortexQAUtils::FindActorByName(UWorld* World, const FString& ActorIdentifier)
{
    return FCortexEditorUtils::FindActorInPIE(World, ActorIdentifier);
}

FCortexCommandResult FCortexQAUtils::PIENotActiveError()
{
    return FCortexEditorUtils::PIENotActiveError();
}

bool FCortexQAUtils::IsEngineInternalActor(const AActor* Actor)
{
    if (Actor == nullptr)
    {
        return true;
    }

    return Actor->IsA<AWorldSettings>()
        || Actor->IsA<AGameModeBase>()
        || Actor->IsA<AGameStateBase>()
        || Actor->IsA<APlayerStart>()
        || Actor->IsA<AInfo>();
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
