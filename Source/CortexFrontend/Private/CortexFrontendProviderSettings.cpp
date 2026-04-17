#include "CortexFrontendProviderSettings.h"

#include "Providers/CortexProviderRegistry.h"

UCortexFrontendProviderSettings::UCortexFrontendProviderSettings()
{
    ActiveProviderId = FCortexProviderRegistry::GetDefaultProviderId();
    ProviderSelectionHelpText = TEXT("Active AI Provider used by newly created Cortex frontend sessions. Current sessions do not restart when this setting changes.");
}

TArray<FString> UCortexFrontendProviderSettings::GetProviderOptions()
{
    return FCortexProviderRegistry::GetProviderOptions();
}

FString UCortexFrontendProviderSettings::GetDefaultProviderId()
{
    return FCortexProviderRegistry::GetDefaultProviderId();
}

const UCortexFrontendProviderSettings* UCortexFrontendProviderSettings::Get()
{
    return GetDefault<UCortexFrontendProviderSettings>();
}
