#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ICortexCommandRegistry.h"

CORTEXCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogCortex, Log, All);

class FCortexCommandRouter;
class FCortexTcpServer;

class CORTEXCORE_API FCortexCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Get the command registry for domain modules to register handlers. */
    ICortexCommandRegistry& GetCommandRegistry();

    /** Get the command router for backward compat during migration. */
    FCortexCommandRouter& GetCommandRouter();

private:
    TUniquePtr<FCortexCommandRouter> CommandRouter;
    TUniquePtr<FCortexTcpServer> TcpServer;
};
