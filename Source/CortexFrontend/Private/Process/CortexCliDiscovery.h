#pragma once

#include "CoreMinimal.h"

struct FCortexCliInfo
{
    FString Path;
    bool bIsCmd = false;
    bool bIsValid = false;
};

class FCortexCliDiscovery
{
public:
    static FCortexCliInfo FindClaude();
    static void ClearCache();

private:
    static FCortexCliInfo CachedInfo;
    static bool bHasSearched;
};
