#pragma once

#include "Modules/ModuleInterface.h"

class SDockTab;
class FSpawnTabArgs;

CORTEXFRONTEND_API DECLARE_LOG_CATEGORY_EXTERN(LogCortexFrontend, Log, All);

class CORTEXFRONTEND_API FCortexFrontendModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);

    static const FName CortexChatTabId;
    FDelegateHandle StartupCallbackHandle;
};
