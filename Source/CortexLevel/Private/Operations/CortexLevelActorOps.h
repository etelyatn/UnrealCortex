#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelActorOps
{
public:
    static FCortexCommandResult SpawnActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DeleteActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DuplicateActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RenameActor(const TSharedPtr<FJsonObject>& Params);
};
