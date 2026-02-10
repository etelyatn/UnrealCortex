#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexTcpServer.h"
#include "CortexSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortex, Log, All);

void FCortexCoreModule::StartupModule()
{
    UE_LOG(LogCortex, Log, TEXT("CortexCore module starting up"));

    const UCortexSettings* Settings = UCortexSettings::Get();
    if (Settings == nullptr || !Settings->bAutoStart)
    {
        return;
    }

    CommandRouter = MakeUnique<FCortexCommandRouter>();

    TcpServer = MakeUnique<FCortexTcpServer>();
    TcpServer->Start(Settings->Port,
        [this](const FString& Command, const TSharedPtr<FJsonObject>& Params)
        {
            return CommandRouter->Execute(Command, Params);
        }
    );

    UE_LOG(LogCortex, Log, TEXT("CortexCore TCP server started on port %d"), Settings->Port);
}

void FCortexCoreModule::ShutdownModule()
{
    UE_LOG(LogCortex, Log, TEXT("CortexCore module shutting down"));

    if (TcpServer.IsValid())
    {
        TcpServer->Stop();
        TcpServer.Reset();
    }

    CommandRouter.Reset();
}

ICortexCommandRegistry& FCortexCoreModule::GetCommandRegistry()
{
    check(CommandRouter.IsValid());
    return *CommandRouter; // FCortexCommandRouter implements ICortexCommandRegistry
}

FCortexCommandRouter& FCortexCoreModule::GetCommandRouter()
{
    check(CommandRouter.IsValid());
    return *CommandRouter;
}

IMPLEMENT_MODULE(FCortexCoreModule, CortexCore)
