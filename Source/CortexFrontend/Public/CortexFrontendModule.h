#pragma once

#include "Modules/ModuleInterface.h"

class FMonitoredProcess;
class SDockTab;
class FSpawnTabArgs;
class FCortexCliSession;
class SCortexWorkbench;
struct FCortexConversionPayload;
struct FCortexAnalysisPayload;
struct FCortexSessionConfig;

CORTEXFRONTEND_API DECLARE_LOG_CATEGORY_EXTERN(LogCortexFrontend, Log, All);

class CORTEXFRONTEND_API FCortexFrontendModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    TWeakPtr<FCortexCliSession> GetOrCreateSession();

    /** Create a default session config using project paths. */
    static FCortexSessionConfig CreateDefaultSessionConfig();

    /** Register a conversion session for PreExit cleanup. */
    void RegisterSession(TSharedPtr<FCortexCliSession> Session);

    /** Unregister a conversion session. */
    void UnregisterSession(TSharedPtr<FCortexCliSession> Session);

    /** Register a build process for PreExit cleanup. */
    void RegisterBuildProcess(TSharedPtr<FMonitoredProcess> Process);

    /** Unregister a build process. */
    void UnregisterBuildProcess(TSharedPtr<FMonitoredProcess> Process);

private:
    TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);
    TSharedRef<SDockTab> SpawnGenStudioTab(const FSpawnTabArgs& Args);
    void OnConversionRequested(const FCortexConversionPayload& Payload);
    void OnAnalysisRequested(const FCortexAnalysisPayload& Payload);
    void ReleaseSessions();
    void HandlePreExit();

    static const FName CortexChatTabId;
    static const FName GenStudioTabId;
    FDelegateHandle StartupCallbackHandle;
    FDelegateHandle ConversionDelegateHandle;
    FDelegateHandle AnalysisDelegateHandle;
    TArray<TSharedPtr<FCortexCliSession>> Sessions;
    TArray<TSharedPtr<FMonitoredProcess>> BuildProcesses;
    TWeakPtr<SCortexWorkbench> WorkbenchWeak;
    bool bGenStudioTabRegistered = false;
};
