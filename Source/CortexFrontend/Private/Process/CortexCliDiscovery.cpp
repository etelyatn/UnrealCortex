#include "Process/CortexCliDiscovery.h"

#include "CortexFrontendModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Providers/CortexClaudeCliProvider.h"
#include "Providers/CortexCodexCliProvider.h"
#include "Providers/CortexProviderRegistry.h"

TMap<FName, FCortexCliInfo> FCortexCliDiscovery::CachedInfoByProvider;
#if WITH_DEV_AUTOMATION_TESTS
TMap<FName, TSharedPtr<ICortexCliProvider>> FCortexCliDiscovery::ProviderOverridesForTests;
#endif

namespace
{
    const ICortexCliProvider& GetClaudeProvider()
    {
        static FCortexClaudeCliProvider Provider;
        return Provider;
    }

    const ICortexCliProvider& GetCodexProvider()
    {
        static FCortexCodexCliProvider Provider;
        return Provider;
    }
}

void FCortexCliDiscovery::ClearCache()
{
    CachedInfoByProvider.Reset();
}

FCortexCliInfo FCortexCliDiscovery::FindClaude()
{
    return Find(FName(*FCortexProviderRegistry::GetDefaultProviderId()));
}

FCortexCliInfo FCortexCliDiscovery::Find(FName ProviderId)
{
    if (const FCortexCliInfo* CachedInfo = CachedInfoByProvider.Find(ProviderId))
    {
        return *CachedInfo;
    }

    const ICortexCliProvider* Provider = GetProvider(ProviderId);
    if (Provider == nullptr)
    {
        FCortexCliInfo Info;
        Info.ProviderId = ProviderId;
        CachedInfoByProvider.Add(ProviderId, Info);
        return Info;
    }

    const FCortexCliInfo Info = Provider->FindCli();
    CachedInfoByProvider.Add(ProviderId, Info);
    return Info;
}

const ICortexCliProvider* FCortexCliDiscovery::GetProvider(const FName& ProviderId)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (const TSharedPtr<ICortexCliProvider>* Override = ProviderOverridesForTests.Find(ProviderId))
    {
        return Override->Get();
    }
#endif

    if (ProviderId == FName(TEXT("codex")))
    {
        return &GetCodexProvider();
    }

    if (ProviderId == FName(*FCortexProviderRegistry::GetDefaultProviderId()))
    {
        return &GetClaudeProvider();
    }

    if (ProviderId == FName(TEXT("claude_code")))
    {
        return &GetClaudeProvider();
    }

    return nullptr;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexCliDiscovery::SetProviderOverrideForTest(FName ProviderId, TSharedPtr<ICortexCliProvider> Provider)
{
    if (Provider.IsValid())
    {
        ProviderOverridesForTests.Add(ProviderId, MoveTemp(Provider));
    }
    else
    {
        ProviderOverridesForTests.Remove(ProviderId);
    }
}

void FCortexCliDiscovery::ClearProviderOverridesForTest()
{
    ProviderOverridesForTests.Reset();
}
#endif
