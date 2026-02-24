#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelComponentOps
{
public:
    /** Handler for properties that need custom write logic (not the generic path). */
    using FPropertyWriteHandler = TFunction<bool(
        UActorComponent* Component,
        const TSharedPtr<FJsonValue>& JsonValue,
        TArray<FString>& OutWarnings
    )>;

    static FCortexCommandResult ListComponents(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult AddComponent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RemoveComponent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetComponentProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetComponentProperty(const TSharedPtr<FJsonObject>& Params);

private:
    /** Registry of property names that need custom write logic instead of generic path. */
    static const TMap<FName, FPropertyWriteHandler>& GetWriteHandlers();
};
