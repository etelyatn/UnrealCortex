#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ICortexCommandRegistry.h"

class FUDBCommandHandler;
class FUDBTcpServer;

class CORTEXCORE_API FCortexCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Get the command registry for domain modules to register handlers. */
    ICortexCommandRegistry& GetCommandRegistry();

    /** Get the command router for backward compat during migration. */
    FUDBCommandHandler& GetCommandRouter();

private:
    TUniquePtr<FUDBCommandHandler> CommandRouter;
    TUniquePtr<FUDBTcpServer> TcpServer;
};
