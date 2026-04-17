#pragma once

#include "CoreMinimal.h"
#include "Providers/CortexCliProvider.h"

class FCortexCliDiscovery
{
public:
    static FCortexCliInfo FindClaude();
    static FCortexCliInfo Find(FName ProviderId);
    static void ClearCache();
#if WITH_DEV_AUTOMATION_TESTS
    static void SetProviderOverrideForTest(FName ProviderId, TSharedPtr<ICortexCliProvider> Provider);
    static void ClearProviderOverridesForTest();
#endif

private:
    static const ICortexCliProvider* GetProvider(const FName& ProviderId);
    static TMap<FName, FCortexCliInfo> CachedInfoByProvider;
#if WITH_DEV_AUTOMATION_TESTS
    static TMap<FName, TSharedPtr<ICortexCliProvider>> ProviderOverridesForTests;
#endif
};
