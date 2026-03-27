#pragma once

#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCortexGen, Log, All);

class FCortexGenJobManager;

class CORTEXGEN_API FCortexGenModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Returns the job manager. Only valid when the module initialized with bEnabled=true.
     *  Calling this when CortexGen was disabled at startup will trigger a checkf. */
    FCortexGenJobManager& GetJobManager() const;

    /** Returns true if CortexGen is enabled in settings. Safe to call anytime. */
    static bool IsEnabled();

private:
    void HandleEnginePreExit();

    TSharedPtr<FCortexGenJobManager> JobManager;
};
