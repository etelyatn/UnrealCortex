#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class AActor;
class APawn;
class APlayerController;
class UWorld;

class FCortexQAUtils
{
public:
    static UWorld* GetPIEWorld();
    static APlayerController* GetPlayerController(UWorld* World);
    static APawn* GetPlayerPawn(UWorld* World);
    static AActor* FindActorByName(UWorld* World, const FString& ActorIdentifier);
    static FCortexCommandResult PIENotActiveError();

    /** Check if an actor is engine-internal and should be filtered from QA observe results. */
    static bool IsEngineInternalActor(const AActor* Actor);

    static void SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Value);
    static void SetRotatorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FRotator& Value);
};
