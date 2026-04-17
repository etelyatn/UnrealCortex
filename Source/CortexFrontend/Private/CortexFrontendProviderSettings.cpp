#include "CortexFrontendProviderSettings.h"

UCortexFrontendProviderSettings::UCortexFrontendProviderSettings()
{
    ProviderSelectionHelpText = TEXT("Active AI Provider used by newly created Cortex frontend sessions. Current sessions do not restart when this setting changes.");
}

TArray<FString> UCortexFrontendProviderSettings::GetProviderOptions()
{
    return { TEXT("claude_code"), TEXT("codex") };
}

const UCortexFrontendProviderSettings* UCortexFrontendProviderSettings::Get()
{
    return GetDefault<UCortexFrontendProviderSettings>();
}
