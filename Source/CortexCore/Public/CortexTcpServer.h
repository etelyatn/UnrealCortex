
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

	using FClientDisconnectCallback = TFunction<void()>;

	FCortexTcpServer();
	~FCortexTcpServer();

	bool Start(int32 Port, FCommandDispatcher InDispatcher);
	void Stop();
	bool IsRunning() const;
	void SendDeferredResponse(int32 DeferredId, const FCortexCommandResult& Result);
	void SetClientDisconnectCallback(FClientDisconnectCallback Callback);

	/** Get the port the server is currently bound to. Returns 0 if not running. Game-thread-only. */
	int32 GetBoundPort() const;

	/** Get number of active clients. Excludes sockets accepted since the last tick (pending promotion). Game-thread-only. */
	int32 GetClientCount() const;

	static constexpr int32 MaxMessageSize = 2 * 1024 * 1024;  // 2MB

private:
	bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);
	void ProcessClientData();

	/** Process data for a single client socket. Returns false if client should be removed. */
	bool ProcessSingleClient(FSocket* InClientSocket);

	/** Queue a JSON response and flush it without blocking the Game Thread. */
	bool SendResponse(FSocket* InClientSocket, const FString& ResponseString);

	/** Try to flush queued response bytes for a socket; false means it must be disconnected. */
	bool FlushPendingResponses(FSocket* InClientSocket);
	/** Keep queued bytes on retryable send errors; retire the queue on terminal errors. */
	bool HandleSendFailure(FSocket* InClientSocket, int32 SocketErrorCode);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test-only access for deterministic accepted-socket and response framing tests. */
	friend class FCortexTcpServerPartialSendIsCompletedTest;
	friend class FCortexTcpServerAcceptsNonBlockingSocketTest;
	friend class FCortexTcpServerResumesAfterWouldBlockTest;
	friend class FCortexTcpServerTerminalSendFailureTest;
	friend class FCortexTcpServerQueuedResponseBacklogLimitTest;
	friend class FCortexTcpServerDeferredSendFailureRetiresClientTest;
	friend class FCortexTcpServerProcessesBufferedRequestsWithoutNewDataTest;
#endif

	/** Close and destroy a client socket */
	void DestroyClientSocket(FSocket* InClientSocket);
	static constexpr double ResponseSendTimeoutSeconds = 30.0;
	/** Bounds response frames queued behind the active frame; the active frame remains uncapped. */
	static constexpr int32 MaxQueuedResponseBacklogBytesPerClient = MaxMessageSize;
	static constexpr int32 MaxQueuedResponseFramesPerClient = 1024;

	static constexpr double CommandTimeoutWarningSeconds = 30.0;
	static constexpr double DefaultDeferredTimeoutSeconds = 30.0;
	static constexpr int32 ReceiveBufferSize = 65536;

	TUniquePtr<FTcpListener> Listener;
	struct FPendingResponse
	{
		TArray<uint8> Bytes;
		int32 BytesSent = 0;
		double QueuedAt = 0.0;
	};

	struct FPendingResponseQueue
	{
		TArray<FPendingResponse> Responses;
		int64 BacklogBytes = 0;
	};
	TMap<FSocket*, FPendingResponseQueue> PendingResponses;
	TArray<FSocket*> ClientSockets;
	TSet<FSocket*> PendingClientDisconnects;
	TArray<FSocket*> PendingClientSockets;
	FCriticalSection PendingSocketsCS;
	TMap<FSocket*, FString> ReceiveBuffers;
	void CheckDeferredTimeouts();
	/** Thread-safe flag for running state. Overall server state queries (GetBoundPort, GetClientCount) remain game-thread-only. */
	FThreadSafeBool bRunning = false;
	FTSTicker::FDelegateHandle TickDelegateHandle;
	FCommandDispatcher CommandDispatcher;
	TMap<int32, FCortexPendingDeferred> PendingDeferred;
	int32 NextDeferredId = 1;

	FClientDisconnectCallback ClientDisconnectCallback;

	/** Port the server successfully bound to. Written in Start() before bRunning is set, reset to 0 in Stop(). */
	int32 BoundPort = 0;

	/** Full path to this editor's port file (CortexPort-{PID}.txt) */
	FString PortFilePath;

	/** Timestamp of last successful tick (game thread heartbeat). Updated atomically for cross-thread reads. */
	TAtomic<double> LastTickTime{0.0};

	/** Threshold in seconds after which a tick gap is considered a stall */
	static constexpr double StallWarningThresholdSeconds = 5.0;

	/** Remove port files whose PIDs are no longer running */
	static void CleanupStalePortFiles();
};
