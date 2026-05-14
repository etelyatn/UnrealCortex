#include "CortexFrontendProviderSettings.h"

#include "Providers/CortexProviderRegistry.h"

UCortexFrontendProviderSettings::UCortexFrontendProviderSettings()
{
    ActiveProviderId = FCortexProviderRegistry::GetDefaultProviderId();
    ProviderSelectionHelpText = TEXT("Active AI Provider used by newly created Cortex frontend sessions. Current sessions do not restart when this setting changes.");
}

FName UCortexFrontendProviderSettings::GetContainerName() const
{
    return FName(TEXT("Editor"));
}

FName UCortexFrontendProviderSettings::GetCategoryName() const
{
    return FName(TEXT("Unreal Cortex"));
}

FName UCortexFrontendProviderSettings::GetSectionName() const
{
    return FName(TEXT("Frontend"));
}

TArray<FString> UCortexFrontendProviderSettings::GetProviderOptions()
{
    return FCortexProviderRegistry::GetProviderOptions();
}

FString UCortexFrontendProviderSettings::GetDefaultProviderId()
{
    return FCortexProviderRegistry::GetDefaultProviderId();
}

FString UCortexFrontendProviderSettings::GetEffectiveProviderId() const
{
    return FCortexProviderRegistry::ResolveDefinition(ActiveProviderId).ProviderId.ToString();
}

const UCortexFrontendProviderSettings* UCortexFrontendProviderSettings::Get()
{
    return GetDefault<UCortexFrontendProviderSettings>();
}
