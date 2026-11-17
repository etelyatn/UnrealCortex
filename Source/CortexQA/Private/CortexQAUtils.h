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

    using FPIEPoll = TFunction<TOptional<FCortexCommandResult>(UWorld& PIEWorld)>;

    /**
     * Completes a deferred QA command by polling on the core ticker every IntervalSeconds while World remains
     * the active PIE world. Paused frames are skipped. The first result Poll returns answers the command; if PIE
     * ends or is replaced first, the command answers PIE_TERMINATED "PIE terminated during {OperationLabel}".
     * Returns the deferred result whose transport deadline covers OperationSeconds plus a grace period.
     */
    static FCortexCommandResult StartDeferredPIEPoll(
        UWorld* World,
        float IntervalSeconds,
        double OperationSeconds,
        const FString& OperationLabel,
        FDeferredResponseCallback DeferredCallback,
        FPIEPoll Poll);

    static void SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Value);
    static void SetRotatorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FRotator& Value);
};
