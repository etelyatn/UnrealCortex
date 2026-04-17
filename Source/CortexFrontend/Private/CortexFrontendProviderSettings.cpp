#include "CortexFrontendProviderSettings.h"

UCortexFrontendProviderSettings::UCortexFrontendProviderSettings()
{
    ProviderSelectionHelpText = TEXT("Newly created sessions use the selected provider. Current sessions do not restart when this setting changes.");
}

TArray<FString> UCortexFrontendProviderSettings::GetProviderOptions()
{
    return { TEXT("claude_code"), TEXT("codex") };
}

const UCortexFrontendProviderSettings* UCortexFrontendProviderSettings::Get()
{
    return GetDefault<UCortexFrontendProviderSettings>();
}
