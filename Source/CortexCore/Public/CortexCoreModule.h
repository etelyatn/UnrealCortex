#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ICortexCommandRegistry.h"
#include "CortexTcpServer.h"

CORTEXCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogCortex, Log, All);

class FCortexCommandRouter;

class CORTEXCORE_API FCortexCoreModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Get the command registry for domain modules to register handlers. */
    ICortexCommandRegistry& GetCommandRegistry();

    /** Get the command router for backward compat during migration. */
    FCortexCommandRouter& GetCommandRouter();

    /** Forward a disconnect callback to the TCP server. */
    void SetClientDisconnectCallback(FCortexTcpServer::FClientDisconnectCallback Callback);

private:
    TUniquePtr<FCortexCommandRouter> CommandRouter;
    TUniquePtr<FCortexTcpServer> TcpServer;
};
