#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ICortexCommandRegistry.h"
#include "CortexTcpServer.h"
#include "CortexConversionTypes.h"

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

	// Conversion UI delegates
	FOnCortexConversionRequested& OnConversionRequested() { return ConversionRequestedDelegate; }

	// Serialization callback — CortexBlueprint binds at startup, unbinds at shutdown
	void SetSerializationHandler(FOnCortexSerializationRequested Handler)
	{
		checkf(!SerializationHandler.IsBound(), TEXT("Serialization handler already bound — double-bind bug"));
		SerializationHandler = Handler;
	}
	void ClearSerializationHandler() { SerializationHandler.Unbind(); }
	void RequestSerialization(const FCortexSerializationRequest& Request, FOnSerializationComplete Callback);

private:
	TUniquePtr<FCortexCommandRouter> CommandRouter;
	TUniquePtr<FCortexTcpServer> TcpServer;

	FOnCortexConversionRequested ConversionRequestedDelegate;
	FOnCortexSerializationRequested SerializationHandler;
};
