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

    /** Parse a [X,Y,Z] JSON array field with a default fallback. Returns false only if format is invalid. */
    static bool TryParseVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue, FVector& OutVector);

    /** Parse a required [X,Y,Z] JSON array field. Returns false if field is missing or invalid. */
    static bool ParseVectorField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutVector);

    /** Serialize a vector as a [X,Y,Z] JSON array field. */
    static void SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Vector);
};
