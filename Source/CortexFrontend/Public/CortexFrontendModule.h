#pragma once

#include "Modules/ModuleInterface.h"

class SDockTab;
class FSpawnTabArgs;
class FCortexCliSession;
class SCortexWorkbench;
struct FCortexConversionPayload;

CORTEXFRONTEND_API DECLARE_LOG_CATEGORY_EXTERN(LogCortexFrontend, Log, All);

class CORTEXFRONTEND_API FCortexFrontendModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    TWeakPtr<FCortexCliSession> GetOrCreateSession();

    /** Register a conversion session for PreExit cleanup. */
    void RegisterSession(TSharedPtr<FCortexCliSession> Session);

    /** Unregister a conversion session. */
    void UnregisterSession(TSharedPtr<FCortexCliSession> Session);

private:
    TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
    void OnConversionRequested(const FCortexConversionPayload& Payload);
    void ReleaseSessions();
    void HandlePreExit();

    static const FName CortexChatTabId;
    FDelegateHandle StartupCallbackHandle;
    FDelegateHandle ConversionDelegateHandle;
    TArray<TSharedPtr<FCortexCliSession>> Sessions;
    TWeakPtr<SCortexWorkbench> WorkbenchWeak;
};
