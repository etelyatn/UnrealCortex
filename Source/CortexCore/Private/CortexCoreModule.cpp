#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexCoreCommandHandler.h"
#include "CortexTcpServer.h"
#include "CortexSettings.h"

DEFINE_LOG_CATEGORY(LogCortex);

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
        [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
        {
            return CommandRouter->Execute(Command, Params, MoveTemp(DeferredCallback));
        }
    );

    UE_LOG(LogCortex, Log, TEXT("CortexCore TCP server started on port %d"), Settings->Port);

    CommandRouter->RegisterDomain(
        TEXT("core"),
        TEXT("Cortex Core"),
        TEXT("1.0.0"),
        MakeShared<FCortexCoreCommandHandler>()
    );
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

void FCortexCoreModule::RequestSerialization(const FCortexSerializationRequest& Request, FOnSerializationComplete Callback)
{
	if (SerializationHandler.IsBound())
	{
		SerializationHandler.Execute(Request, Callback);
	}
	else
	{
		UE_LOG(LogCortex, Log, TEXT("RequestSerialization called but no handler bound"));
		FCortexSerializationResult ErrorResult;
		ErrorResult.bSuccess = false;
		ErrorResult.JsonPayload = TEXT("{\"error\":\"No serialization handler bound\"}");
		Callback.Execute(ErrorResult);
	}
}

bool FCortexCoreModule::IsServerRunning() const
{
    return TcpServer.IsValid() && TcpServer->IsRunning();
}

int32 FCortexCoreModule::GetServerPort() const
{
    return TcpServer.IsValid() ? TcpServer->GetBoundPort() : 0;
}

int32 FCortexCoreModule::GetClientCount() const
{
    return TcpServer.IsValid() ? TcpServer->GetClientCount() : 0;
}

int32 FCortexCoreModule::GetDomainCount() const
{
    return CommandRouter.IsValid() ? CommandRouter->GetRegisteredDomains().Num() : 0;
}

void FCortexCoreModule::SetClientDisconnectCallback(FCortexTcpServer::FClientDisconnectCallback Callback)
{
    if (TcpServer.IsValid())
    {
        TcpServer->SetClientDisconnectCallback(MoveTemp(Callback));
    }
    else
    {
        UE_LOG(LogCortex, Warning, TEXT("SetClientDisconnectCallback: TcpServer not ready — callback dropped"));
    }
}

IMPLEMENT_MODULE(FCortexCoreModule, CortexCore)
