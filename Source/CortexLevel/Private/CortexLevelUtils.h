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

    /** Resolve the optional `world` param: absent or "editor" targets the editor world,
     *  "pie" targets GEditor->PlayWorld (PIE_NOT_ACTIVE when PIE is not running), and any
     *  other value is refused with INVALID_VALUE. */
    static UWorld* ResolveWorld(const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError);

    /** Editor-world writes are transactable; PIE writes are transient and must not enter the
     *  editor transaction buffer (PIE teardown resets it and discards editor undo history). */
    static bool ShouldTransact(UWorld* World);

    /** Copy a top-level `world` into every batch item whose params do not already set one. */
    static void ApplyWorldToBatchRequest(const TSharedPtr<FJsonObject>& Params, FCortexBatchMutationRequest& Request);

    static AActor* FindActorByLabelOrPath(UWorld* World, const FString& ActorIdentifier, FCortexCommandResult& OutError);
    static UClass* ResolveActorClass(const FString& ClassIdentifier, FCortexCommandResult& OutError);
    static TSharedPtr<FJsonObject> SerializeActorSummary(AActor* Actor);

    /** Resolve a sublevel by short name (e.g. "LVL_Cubic_Campus_BPs") to its loaded ULevel*. */
    static ULevel* ResolveSublevel(UWorld* World, const FString& SublevelName, FCortexCommandResult& OutError);

    /** Append a lightweight components array (name + class) to an actor JSON object. */
    static void AppendComponentSummary(AActor* Actor, TSharedPtr<FJsonObject> Json);

    /** Collect up to MaxSuggestions actor labels/names that substring-match the query. */
    static TSharedPtr<FJsonObject> CollectActorSuggestions(UWorld* World, const FString& Query, int32 MaxSuggestions = 5);

    /** Collect component names as suggestions for COMPONENT_NOT_FOUND errors. */
    static TSharedPtr<FJsonObject> CollectComponentSuggestions(AActor* Actor, int32 MaxSuggestions = 5);

    /** Parse an optional [X,Y,Z] JSON array field. Missing keeps DefaultValue; a present non-array, wrong length or non-numeric element is a refusal. */
    static bool TryParseVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, const FVector& DefaultValue, FVector& OutVector);

    /** Parse a required [X,Y,Z] JSON array field. Returns false if the field is missing, is not a 3-element array, or has a non-numeric element. */
    static bool ParseVectorField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FVector& OutVector);

    /** Serialize a vector as a [X,Y,Z] JSON array field. */
    static void SetVectorArray(TSharedPtr<FJsonObject> Json, const TCHAR* FieldName, const FVector& Vector);
};
