#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class AActor;
class UWorld;
class UJsonObject;

class FCortexLevelUtils
{
public:
    static UWorld* GetEditorWorld(FCortexCommandResult& OutError);
    static AActor* FindActorByLabelOrPath(UWorld* World, const FString& ActorIdentifier, FCortexCommandResult& OutError);
    static UClass* ResolveActorClass(const FString& ClassIdentifier, FCortexCommandResult& OutError);
    static TSharedPtr<FJsonObject> SerializeActorSummary(AActor* Actor);
};
