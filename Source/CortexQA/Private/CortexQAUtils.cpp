#include "CortexQAUtils.h"

#include "CortexTypes.h"
#include "Editor.h"
#include "EngineUtils.h"
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

APawn* FCortexQAUtils::GetPlayerPawn(UWorld* World)
{
    APlayerController* PC = GetPlayerController(World);
    return (PC != nullptr) ? PC->GetPawn() : nullptr;
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

void FCortexQAUtils::SetVectorObject(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Value)
{
    TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
    ObjectValue->SetNumberField(TEXT("x"), Value.X);
    ObjectValue->SetNumberField(TEXT("y"), Value.Y);
    ObjectValue->SetNumberField(TEXT("z"), Value.Z);
    Json->SetObjectField(FieldName, ObjectValue);
}

void FCortexQAUtils::SetRotatorObject(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FRotator& Value)
{
    TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
    ObjectValue->SetNumberField(TEXT("pitch"), Value.Pitch);
    ObjectValue->SetNumberField(TEXT("yaw"), Value.Yaw);
    ObjectValue->SetNumberField(TEXT("roll"), Value.Roll);
    Json->SetObjectField(FieldName, ObjectValue);
}

bool FCortexQAUtils::ParseVectorParam(const TSharedPtr<FJsonValue>& Value, FVector& OutVector, FString& OutError)
{
    if (!Value.IsValid())
    {
        OutError = TEXT("Value is null");
        return false;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& ObjectValue = Value->AsObject();
        if (!ObjectValue.IsValid())
        {
            OutError = TEXT("Object is invalid");
            return false;
        }

        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!ObjectValue->TryGetNumberField(TEXT("x"), X)
            || !ObjectValue->TryGetNumberField(TEXT("y"), Y)
            || !ObjectValue->TryGetNumberField(TEXT("z"), Z))
        {
            OutError = TEXT("Object must have x, y, z number fields");
            return false;
        }

        OutVector = FVector(X, Y, Z);
        return true;
    }

    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& ArrayValue = Value->AsArray();
        if (ArrayValue.Num() != 3)
        {
            OutError = TEXT("Array must have 3 elements");
            return false;
        }

        if (!ArrayValue[0].IsValid() || !ArrayValue[1].IsValid() || !ArrayValue[2].IsValid())
        {
            OutError = TEXT("Array elements must be numbers");
            return false;
        }

        const double X = ArrayValue[0]->AsNumber();
        const double Y = ArrayValue[1]->AsNumber();
        const double Z = ArrayValue[2]->AsNumber();
        OutVector = FVector(X, Y, Z);
        return true;
    }

    OutError = TEXT("Must be object {x,y,z} or array [x,y,z]");
    return false;
}

bool FCortexQAUtils::ParseRotatorParam(const TSharedPtr<FJsonValue>& Value, FRotator& OutRotator, FString& OutError)
{
    if (!Value.IsValid())
    {
        OutError = TEXT("Value is null");
        return false;
    }

    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject>& ObjectValue = Value->AsObject();
        if (!ObjectValue.IsValid())
        {
            OutError = TEXT("Object is invalid");
            return false;
        }

        double Pitch = 0.0;
        double Yaw = 0.0;
        double Roll = 0.0;
        if (!ObjectValue->TryGetNumberField(TEXT("pitch"), Pitch)
            || !ObjectValue->TryGetNumberField(TEXT("yaw"), Yaw)
            || !ObjectValue->TryGetNumberField(TEXT("roll"), Roll))
        {
            OutError = TEXT("Object must have pitch, yaw, roll number fields");
            return false;
        }

        OutRotator = FRotator(Pitch, Yaw, Roll);
        return true;
    }

    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& ArrayValue = Value->AsArray();
        if (ArrayValue.Num() != 3)
        {
            OutError = TEXT("Array must have 3 elements");
            return false;
        }

        if (!ArrayValue[0].IsValid() || !ArrayValue[1].IsValid() || !ArrayValue[2].IsValid())
        {
            OutError = TEXT("Array elements must be numbers");
            return false;
        }

        OutRotator = FRotator(ArrayValue[0]->AsNumber(), ArrayValue[1]->AsNumber(), ArrayValue[2]->AsNumber());
        return true;
    }

    OutError = TEXT("Must be object {pitch,yaw,roll} or array [pitch,yaw,roll]");
    return false;
}
