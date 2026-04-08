#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ICortexCommandRegistry.h"
#include "CortexTcpServer.h"
#include "CortexConversionTypes.h"
#include "CortexAnalysisTypes.h"
#include "CortexCoreDelegates.h"

CORTEXCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogCortex, Log, All);

#include "CortexCommandRouter.h"

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

	// Analysis UI delegates
	FOnCortexAnalysisRequested& OnAnalysisRequested() { return AnalysisRequestedDelegate; }

	// Generic domain progress delegate
	FOnCortexDomainProgress& OnDomainProgress() { return DomainProgressDelegate; }

	// Status bar accessors — null-safe, game-thread-only
	/** Returns true if the TCP server is running. */
	bool IsServerRunning() const;

	/** Returns the port the TCP server is bound to, or 0 if not running. */
	int32 GetServerPort() const;

	/** Returns the number of connected TCP clients, or 0 if server is not running. */
	int32 GetClientCount() const;

	/** Returns the number of registered command domains. */
	int32 GetDomainCount() const;

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
	FOnCortexAnalysisRequested AnalysisRequestedDelegate;
	FOnCortexDomainProgress DomainProgressDelegate;
	FOnCortexSerializationRequested SerializationHandler;
};
