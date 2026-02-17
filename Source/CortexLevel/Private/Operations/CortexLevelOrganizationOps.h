#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelOrganizationOps
{
public:
    static FCortexCommandResult AttachActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DetachActor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetTags(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetFolder(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GroupActors(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult UngroupActors(const TSharedPtr<FJsonObject>& Params);
};
