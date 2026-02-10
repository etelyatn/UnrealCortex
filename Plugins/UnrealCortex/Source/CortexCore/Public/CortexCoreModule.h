#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FUDBCommandHandler;
class FUDBTcpServer;

class FCortexCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Get the command router for domain modules to register handlers. */
    FUDBCommandHandler& GetCommandRouter();

private:
    TUniquePtr<FUDBCommandHandler> CommandRouter;
    TUniquePtr<FUDBTcpServer> TcpServer;
};
