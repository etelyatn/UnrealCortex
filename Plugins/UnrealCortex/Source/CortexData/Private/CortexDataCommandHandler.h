#pragma once

#include "CoreMinimal.h"

struct FUDBCommandResult;

class FCortexDataCommandHandler
{
public:
    static FUDBCommandResult Execute(
        const FString& Command,
        const TSharedPtr<FJsonObject>& Params
    );
};
