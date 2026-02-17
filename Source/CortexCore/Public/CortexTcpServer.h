
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "CortexTypes.h"

class FSocket;
class FTcpListener;

struct FCortexPendingDeferred
{
	FSocket* ClientSocket = nullptr;
	FString RequestId;
	double StartTime = 0.0;
	double TimeoutSeconds = 30.0;
};

class CORTEXCORE_API FCortexTcpServer
{
public:
	using FCommandDispatcher = TFunction<FCortexCommandResult(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback)>;

	FCortexTcpServer();
	~FCortexTcpServer();

	bool Start(int32 Port, FCommandDispatcher InDispatcher);
	void Stop();
	bool IsRunning() const;
	void SendDeferredResponse(int32 DeferredId, const FCortexCommandResult& Result);

	static constexpr int32 MaxMessageSize = 2 * 1024 * 1024;  // 2MB

private:
	bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);
	void ProcessClientData();

	/** Process data for a single client socket. Returns false if client should be removed. */
	bool ProcessSingleClient(FSocket* InClientSocket);

	/** Send a JSON response string followed by newline delimiter to a specific client */
	void SendResponse(FSocket* InClientSocket, const FString& ResponseString);

	/** Close and destroy a client socket */
	void DestroyClientSocket(FSocket* InClientSocket);

	static constexpr double CommandTimeoutWarningSeconds = 30.0;
	static constexpr double DefaultDeferredTimeoutSeconds = 30.0;
	static constexpr int32 ReceiveBufferSize = 65536;

	TUniquePtr<FTcpListener> Listener;
	TArray<FSocket*> ClientSockets;
	TArray<FSocket*> PendingClientSockets;
	FCriticalSection PendingSocketsCS;
	TMap<FSocket*, FString> ReceiveBuffers;
	void CheckDeferredTimeouts();
	FThreadSafeBool bRunning = false;
	FTSTicker::FDelegateHandle TickDelegateHandle;
	FCommandDispatcher CommandDispatcher;
	TMap<int32, FCortexPendingDeferred> PendingDeferred;
	int32 NextDeferredId = 1;
};
