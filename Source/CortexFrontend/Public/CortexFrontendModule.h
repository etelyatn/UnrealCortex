#pragma once

#include "Modules/ModuleInterface.h"

class SDockTab;
class FSpawnTabArgs;
class FCortexCliSession;

CORTEXFRONTEND_API DECLARE_LOG_CATEGORY_EXTERN(LogCortexFrontend, Log, All);

class CORTEXFRONTEND_API FCortexFrontendModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    TWeakPtr<FCortexCliSession> GetOrCreateSession();

private:
    TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
    void ReleaseSessions();

    static const FName CortexChatTabId;
    FDelegateHandle StartupCallbackHandle;
    TArray<TSharedPtr<FCortexCliSession>> Sessions;
};
