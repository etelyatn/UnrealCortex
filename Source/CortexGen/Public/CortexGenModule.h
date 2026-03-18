#pragma once

#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCortexGen, Log, All);

class FCortexGenJobManager;

class CORTEXGEN_API FCortexGenModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    FCortexGenJobManager& GetJobManager() const;

private:
    TSharedPtr<FCortexGenJobManager> JobManager;
};
