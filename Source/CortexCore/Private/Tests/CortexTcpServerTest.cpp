
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "CortexTcpServer.h"
#include "CortexCommandRouter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerPingPongTest,
	"Cortex.Core.TcpServer.PingPong",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerExclusivePortTest,
	"Cortex.Core.TcpServer.ExclusivePort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerLargeResponseFramingTest,
	"Cortex.Core.TcpServer.LargeResponseFraming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerPartialSendIsCompletedTest,
	"Cortex.Core.TcpServer.PartialSendIsCompleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerResumesAfterWouldBlockTest,
	"Cortex.Core.TcpServer.ResumesAfterWouldBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerTerminalSendFailureTest,
	"Cortex.Core.TcpServer.TerminalSendFailureClosesConnection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerAcceptsNonBlockingSocketTest,
	"Cortex.Core.TcpServer.AcceptsNonBlockingSocket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerQueuedResponseBacklogLimitTest,
	"Cortex.Core.TcpServer.QueuedResponseBacklogLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerDeferredSendFailureRetiresClientTest,
	"Cortex.Core.TcpServer.DeferredSendFailureRetiresClient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexTcpServerProcessesBufferedRequestsWithoutNewDataTest,
	"Cortex.Core.TcpServer.ProcessesBufferedRequestsWithoutNewData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Test double for FSocket that limits partial writes and can inject one send failure.
 * Production code never constructs this; framing tests use it to drive retry paths
 * deterministically without a runtime send delegate.
 */
class FCortexPartialSendSocket : public FSocket
{
public:
	explicit FCortexPartialSendSocket(
		int32 InMaxBytesPerSend,
		int32 InFailOnSendCall = INDEX_NONE,
		bool bInTerminalFailure = false,
		bool bInWouldBlockReportsNotConnected = false)
		: FSocket(SOCKTYPE_Streaming, TEXT("CortexPartialSendSocket"), NAME_None)
		, MaxBytesPerSend(InMaxBytesPerSend)
		, FailOnSendCall(InFailOnSendCall)
		, bTerminalFailure(bInTerminalFailure)
		, bWouldBlockReportsNotConnected(bInWouldBlockReportsNotConnected)
	{
	}

	/** Accepts partial writes and can simulate one send failure. */
	virtual bool Send(const uint8* Data, int32 Count, int32& BytesSent) override
	{
		++SendCallCount;
		if (SendCallCount == FailOnSendCall)
		{
			BytesSent = 0;
			bSendFailed = true;
			return false;
		}
		BytesSent = FMath::Min(Count, MaxBytesPerSend);
		SentBytes.Append(Data, BytesSent);
		return true;
	}

	// Unused FSocket operations remain inert in this test double.
	virtual bool Shutdown(ESocketShutdownMode /*Mode*/) override { return true; }
	virtual bool Close() override { bClosed = true; return true; }
	virtual bool Bind(const FInternetAddr& /*Addr*/) override { return true; }
	virtual bool Connect(const FInternetAddr& /*Addr*/) override { return true; }
	virtual bool Listen(int32 /*MaxBacklog*/) override { return true; }
	virtual bool WaitForPendingConnection(bool& /*bHasPendingConnection*/, const FTimespan& /*WaitTime*/) override { return false; }
	virtual bool HasPendingData(uint32& /*PendingDataSize*/) override { return false; }
	virtual FSocket* Accept(const FString& /*InSocketDescription*/) override { return nullptr; }
	virtual FSocket* Accept(FInternetAddr& /*OutAddr*/, const FString& /*InSocketDescription*/) override { return nullptr; }
	virtual bool Wait(ESocketWaitConditions::Type /*Condition*/, FTimespan /*WaitTime*/) override { return false; }
	virtual ESocketConnectionState GetConnectionState() override
	{
		if (bTerminalFailure)
		{
			return SCS_ConnectionError;
		}

		return bWouldBlockReportsNotConnected && bSendFailed ? SCS_NotConnected : SCS_Connected;
	}
	virtual void GetAddress(FInternetAddr& /*OutAddr*/) override {}
	virtual bool GetPeerAddress(FInternetAddr& /*OutAddr*/) override { return true; }
	virtual bool SetNonBlocking(bool bIsNonBlocking) override { bWasSetNonBlocking = bIsNonBlocking; return true; }
	virtual bool SetBroadcast(bool /*bAllowBroadcast*/) override { return true; }
	virtual bool SetNoDelay(bool /*bIsNoDelay*/) override { return true; }
	virtual bool JoinMulticastGroup(const FInternetAddr& /*GroupAddress*/) override { return true; }
	virtual bool JoinMulticastGroup(const FInternetAddr& /*GroupAddress*/, const FInternetAddr& /*InterfaceAddress*/) override { return true; }
	virtual bool LeaveMulticastGroup(const FInternetAddr& /*GroupAddress*/) override { return true; }
	virtual bool LeaveMulticastGroup(const FInternetAddr& /*GroupAddress*/, const FInternetAddr& /*InterfaceAddress*/) override { return true; }
	virtual bool SetMulticastLoopback(bool /*bLoopback*/) override { return true; }
	virtual bool SetMulticastTtl(uint8 /*TimeToLive*/) override { return true; }
	virtual bool SetMulticastInterface(const FInternetAddr& /*InterfaceAddress*/) override { return true; }
	virtual bool SetReuseAddr(bool /*bAllowReuse*/) override { return true; }
	virtual bool SetLinger(bool /*bShouldLinger*/, int32 /*Timeout*/) override { return true; }
	virtual bool SetRecvErr(bool /*bUseErrorQueue*/) override { return true; }
	virtual bool SetSendBufferSize(int32 /*Size*/, int32& NewSize) override { NewSize = 0; return true; }
	virtual bool SetReceiveBufferSize(int32 /*Size*/, int32& NewSize) override { NewSize = 0; return true; }
	virtual int32 GetPortNo() override { return 0; }

	/** Number of Send() calls. */
	int32 SendCallCount = 0;

	/** Every byte handed to Send() across all calls, in order. */
	TArray<uint8> SentBytes;
	bool bClosed = false;
	bool bWasSetNonBlocking = false;

private:
	int32 MaxBytesPerSend;
	int32 FailOnSendCall;
	bool bTerminalFailure;
	bool bWouldBlockReportsNotConnected;
	bool bSendFailed = false;


};
/** Formats raw bytes for failure diagnostics. */
static FString CortexBytesToHex(const TArray<uint8>& Bytes)
{
	FString Result;
	for (const uint8 Byte : Bytes)
	{
		Result += FString::Printf(TEXT("%02X "), Byte);
	}
	return Result.TrimEnd();
}
#endif

bool FCortexTcpServerPingPongTest::RunTest(const FString& Parameters)
{
	// Arrange: Start the TCP server on a test port
	const int32 TestPort = 18742;
	FCortexCommandRouter CommandHandler;
	FCortexTcpServer Server;
	const bool bStarted = Server.Start(TestPort,
		[&CommandHandler](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return CommandHandler.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("Server should start successfully"), bStarted);

	if (!bStarted)
	{
		return true;
	}

	TestTrue(TEXT("Server should report running"), Server.IsRunning());

	// Create a client socket and connect to the server
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TestNotNull(TEXT("Socket subsystem should exist"), SocketSubsystem);

	FSocket* ClientSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("CortexTestClient"), false);
	TestNotNull(TEXT("Client socket should be created"), ClientSocket);

	if (ClientSocket == nullptr)
	{
		Server.Stop();
		return true;
	}

	FIPv4Endpoint ServerEndpoint(FIPv4Address::InternalLoopback, TestPort);
	const bool bConnected = ClientSocket->Connect(*ServerEndpoint.ToInternetAddr());
	TestTrue(TEXT("Client should connect to server"), bConnected);

	if (!bConnected)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
		Server.Stop();
		return true;
	}

	// Allow time for the accept to process
	FPlatformProcess::Sleep(0.1f);

	// Act: Send a ping command
	FString PingCommand = TEXT("{\"command\":\"ping\"}\n");
	FTCHARToUTF8 Utf8Converter(*PingCommand);
	int32 BytesSent = 0;
	const bool bSent = ClientSocket->Send(
		reinterpret_cast<const uint8*>(Utf8Converter.Get()),
		Utf8Converter.Length(),
		BytesSent
	);
	TestTrue(TEXT("Client should send ping command"), bSent);
	TestEqual(TEXT("All bytes should be sent"), BytesSent, Utf8Converter.Length());

	// Allow time for the server to process and respond
	// We need the game thread tick to fire, so we pump it manually
	FPlatformProcess::Sleep(0.2f);

	// Tick the server's processing (simulate game thread tick)
	// The server uses FTSTicker, which needs explicit tick in test context
	FTSTicker::GetCoreTicker().Tick(0.016f);

	FPlatformProcess::Sleep(0.1f);

	// Assert: Read the response
	FString ResponseString;
	uint8 RecvBuffer[4096];
	int32 BytesRead = 0;

	// Try long enough to avoid timing flakes when the full suite is running.
	bool bReceivedResponse = false;
	for (int32 Attempt = 0; Attempt < 60; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.05f);

		uint32 PendingDataSize = 0;
		if (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
		{
			if (ClientSocket->Recv(RecvBuffer, sizeof(RecvBuffer) - 1, BytesRead))
			{
				RecvBuffer[BytesRead] = 0;
				FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RecvBuffer), BytesRead);
				ResponseString = FString(Converter.Length(), Converter.Get());
				bReceivedResponse = true;
				break;
			}
		}
	}

	TestTrue(TEXT("Should receive response from server"), bReceivedResponse);

	if (bReceivedResponse)
	{
		// Parse JSON response
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
		const bool bParsed = FJsonSerializer::Deserialize(Reader, JsonObject);
		TestTrue(TEXT("Response should be valid JSON"), bParsed);

		if (bParsed && JsonObject.IsValid())
		{
			bool bSuccess = false;
			TestTrue(TEXT("Response should have 'success' field"), JsonObject->TryGetBoolField(TEXT("success"), bSuccess));
			TestTrue(TEXT("Response 'success' should be true"), bSuccess);

			const TSharedPtr<FJsonObject>* DataObject = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
			{
				FString Message;
				TestTrue(TEXT("Data should have 'message' field"), (*DataObject)->TryGetStringField(TEXT("message"), Message));
				TestEqual(TEXT("Message should be 'pong'"), Message, FString(TEXT("pong")));
			}
			else
			{
				AddError(TEXT("Response should have 'data' object field"));
			}
		}
	}

	// Cleanup
	SocketSubsystem->DestroySocket(ClientSocket);
	Server.Stop();
	TestFalse(TEXT("Server should report not running after stop"), Server.IsRunning());

	return true;
}

bool FCortexTcpServerExclusivePortTest::RunTest(const FString& Parameters)
{
	// Arrange: Start first server on a test port.
	const int32 TestPort = 18900;
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TestNotNull(TEXT("Socket subsystem should exist"), SocketSubsystem);
	if (SocketSubsystem == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router1;
	FCortexTcpServer Server1;
	const bool bServer1Started = Server1.Start(TestPort,
		[&Router1](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return Router1.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("First server should start on test port"), bServer1Started);

	if (!bServer1Started)
	{
		return true;
	}

	// FTcpListener binds asynchronously on its worker thread.
	FPlatformProcess::Sleep(0.1f);

	// Occupy the remainder of the auto-increment range so Server2 can only start
	// if it illegally shares TestPort with Server1.
	TArray<FSocket*> BlockingSockets;
	const auto CleanupBlockingSockets = [&BlockingSockets, SocketSubsystem]()
	{
		for (FSocket* Socket : BlockingSockets)
		{
			if (Socket != nullptr)
			{
				Socket->Close();
				SocketSubsystem->DestroySocket(Socket);
			}
		}
		BlockingSockets.Empty();
	};

	for (int32 Port = TestPort + 1; Port < TestPort + 100; ++Port)
	{
		FSocket* BlockingSocket = SocketSubsystem->CreateSocket(
			NAME_Stream,
			*FString::Printf(TEXT("CortexExclusivePortBlocker_%d"), Port),
			false);
		if (BlockingSocket == nullptr)
		{
			AddError(FString::Printf(TEXT("Failed to create blocking socket for port %d"), Port));
			CleanupBlockingSockets();
			Server1.Stop();
			return true;
		}

		BlockingSocket->SetReuseAddr(false);

		FIPv4Endpoint BlockingEndpoint(FIPv4Address::InternalLoopback, Port);
		const bool bBound = BlockingSocket->Bind(*BlockingEndpoint.ToInternetAddr());
		const bool bListening = bBound && BlockingSocket->Listen(1);
		if (!bListening)
		{
			AddError(FString::Printf(TEXT("Failed to block port %d"), Port));
			BlockingSocket->Close();
			SocketSubsystem->DestroySocket(BlockingSocket);
			CleanupBlockingSockets();
			Server1.Stop();
			return true;
		}

		BlockingSockets.Add(BlockingSocket);
	}

	// Act: Attempt to bind a second server on the same port.
	AddExpectedError(
		TEXT("LogCortex: Failed to bind TCP server on ports"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	FCortexCommandRouter Router2;
	FCortexTcpServer Server2;
	const bool bServer2Started = Server2.Start(TestPort,
		[&Router2](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return Router2.Execute(Command, Params, MoveTemp(DeferredCallback));
		});

	// Assert: second bind should fail while the first server is active.
	TestFalse(TEXT("Second server should fail to bind same port"), bServer2Started);

	// Cleanup
	Server1.Stop();
	Server2.Stop();
	CleanupBlockingSockets();

	return true;
}

bool FCortexTcpServerLargeResponseFramingTest::RunTest(const FString& Parameters)
{
	// Arrange: a dispatcher whose response payload exceeds 40,000 characters.
	constexpr int32 TestPort = 18743;
	constexpr int32 PayloadLength = 60 * 1024;

	FCortexTcpServer Server;
	const bool bStarted = Server.Start(
		TestPort,
		[](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("payload"), FString::ChrN(PayloadLength, TEXT('x')));
			return FCortexCommandRouter::Success(Data);
		});
	TestTrue(TEXT("Server should start successfully"), bStarted);

	if (!bStarted)
	{
		return true;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TestNotNull(TEXT("Socket subsystem should exist"), SocketSubsystem);

	FSocket* ClientSocket = SocketSubsystem != nullptr
		? SocketSubsystem->CreateSocket(NAME_Stream, TEXT("CortexLargeResponseTestClient"), false)
		: nullptr;
	TestNotNull(TEXT("Client socket should be created"), ClientSocket);

	if (ClientSocket == nullptr)
	{
		Server.Stop();
		return true;
	}

	FIPv4Endpoint ServerEndpoint(FIPv4Address::InternalLoopback, TestPort);
	const bool bConnected = ClientSocket->Connect(*ServerEndpoint.ToInternetAddr());
	TestTrue(TEXT("Client should connect to server"), bConnected);

	if (!bConnected)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
		Server.Stop();
		return true;
	}

	// Allow time for the accept to process.
	FPlatformProcess::Sleep(0.1f);

	const FString Request = TEXT("{\"command\":\"large_response\"}\n");
	FTCHARToUTF8 Utf8Request(*Request);
	int32 RequestBytesSent = 0;
	TestTrue(TEXT("Client should send request"),
		ClientSocket->Send(reinterpret_cast<const uint8*>(Utf8Request.Get()), Utf8Request.Length(), RequestBytesSent));

	// Act: tick the server and accumulate bytes until the newline delimiter arrives.
	// A single 4 KiB receive buffer is not enough for this payload.
	constexpr int32 RecvChunkSize = 16384;
	TArray<uint8> ReceivedBytes;
	int32 NewlineIndex = INDEX_NONE;
	for (int32 Attempt = 0; Attempt < 200 && NewlineIndex == INDEX_NONE; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.02f);

		uint32 PendingDataSize = 0;
		while (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
		{
			uint8 RecvBuffer[RecvChunkSize];
			int32 BytesRead = 0;
			if (!ClientSocket->Recv(RecvBuffer, RecvChunkSize, BytesRead) || BytesRead <= 0)
			{
				break;
			}

			ReceivedBytes.Append(RecvBuffer, BytesRead);
			NewlineIndex = ReceivedBytes.Find(static_cast<uint8>(0x0A));
			if (NewlineIndex != INDEX_NONE)
			{
				break;
			}
		}
	}

	// Assert: the delimiter and the complete payload arrived in one frame.
	TestTrue(TEXT("Response should be newline terminated"), NewlineIndex != INDEX_NONE);

	if (NewlineIndex != INDEX_NONE)
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(ReceivedBytes.GetData()), NewlineIndex);
		const FString ResponseLine(Converter.Length(), Converter.Get());

		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseLine);
		const bool bParsed = FJsonSerializer::Deserialize(Reader, JsonObject);
		TestTrue(TEXT("Frame before the newline should be complete JSON"), bParsed);

		if (bParsed && JsonObject.IsValid())
		{
			bool bSuccess = false;
			TestTrue(TEXT("Response should have 'success' field"), JsonObject->TryGetBoolField(TEXT("success"), bSuccess));
			TestTrue(TEXT("Response 'success' should be true"), bSuccess);

			const TSharedPtr<FJsonObject>* DataObject = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
			{
				FString Payload;
				TestTrue(TEXT("Data should have 'payload' field"), (*DataObject)->TryGetStringField(TEXT("payload"), Payload));
				TestEqual(TEXT("Payload length should match the dispatched payload exactly"), Payload.Len(), PayloadLength);
			}
			else
			{
				AddError(TEXT("Response should have 'data' object field"));
			}
		}
	}

	// Cleanup
	SocketSubsystem->DestroySocket(ClientSocket);
	Server.Stop();

	return true;
}

bool FCortexTcpServerPartialSendIsCompletedTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	// Hand-derived UTF-8 framing: 'a' 'b' + U+00E9 (0xC3 0xA9) + 'c' 'd', then the server's newline.
	const FString ResponseString = FString(TEXT("ab")) + FString::Chr(0x00E9) + FString(TEXT("cd"));
	const TArray<uint8> ExpectedBytes = { 0x61, 0x62, 0xC3, 0xA9, 0x63, 0x64, 0x0A };

	// Seven bytes at three bytes per write: two partial writes, then the final byte.
	constexpr int32 MaxBytesPerSend = 3;
	constexpr int32 ExpectedSendCalls = 3;

	FCortexPartialSendSocket FakeSocket(MaxBytesPerSend);
	FCortexTcpServer Server;

	// Act
	Server.SendResponse(&FakeSocket, ResponseString);

	// Assert: writes are repeated until every byte, newline included, has been handed to the socket.
	TestEqual(TEXT("Send should be retried until the response is complete"), FakeSocket.SendCallCount, ExpectedSendCalls);
	TestEqual(TEXT("Every response byte including the newline should be written"), FakeSocket.SentBytes.Num(), ExpectedBytes.Num());

	const bool bBytesMatch = FakeSocket.SentBytes.Num() == ExpectedBytes.Num()
		&& FMemory::Memcmp(FakeSocket.SentBytes.GetData(), ExpectedBytes.GetData(), ExpectedBytes.Num()) == 0;
	TestTrue(TEXT("Sent bytes should be the UTF-8 response followed by a newline"), bBytesMatch);

	if (!bBytesMatch)
	{
		AddInfo(FString::Printf(TEXT("expected [%s] but sent [%s]"),
			*CortexBytesToHex(ExpectedBytes),
			*CortexBytesToHex(FakeSocket.SentBytes)));
	}
#endif

	return true;
}

bool FCortexTcpServerResumesAfterWouldBlockTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	const TArray<uint8> ExpectedBytes = { 0x61, 0x62, 0xC3, 0xA9, 0x63, 0x64, 0x0A };

	FCortexPartialSendSocket FakeSocket(3, 2, false, true);
	FCortexTcpServer Server;
	FCortexTcpServer::FPendingResponseQueue& PendingQueue =
		Server.PendingResponses.FindOrAdd(&FakeSocket);
	FCortexTcpServer::FPendingResponse& PendingResponse = PendingQueue.Responses.AddDefaulted_GetRef();
	PendingResponse.Bytes = ExpectedBytes;
	PendingResponse.QueuedAt = FPlatformTime::Seconds();

	int32 BytesSent = 0;
	TestTrue(
		TEXT("The first partial send writes the response prefix"),
		FakeSocket.Send(PendingResponse.Bytes.GetData(), PendingResponse.Bytes.Num(), BytesSent));
	PendingResponse.BytesSent = BytesSent;
	TestEqual(TEXT("Three UTF-8 bytes are written before backpressure"), BytesSent, 3);

	int32 FailedBytesSent = 0;
	TestFalse(
		TEXT("The next send reports temporary backpressure"),
		FakeSocket.Send(
			PendingResponse.Bytes.GetData() + PendingResponse.BytesSent,
			PendingResponse.Bytes.Num() - PendingResponse.BytesSent,
			FailedBytesSent));
	TestTrue(
		TEXT("Would-block remains retryable even when connection state is not connected"),
		Server.HandleSendFailure(&FakeSocket, SE_EWOULDBLOCK));
	TestFalse(TEXT("Temporary backpressure does not close the socket"), FakeSocket.bClosed);

	Server.ClientSockets.Add(&FakeSocket);
	Server.ProcessClientData();

	TestEqual(TEXT("The server resumes the unsent suffix on a later tick"), FakeSocket.SentBytes.Num(), ExpectedBytes.Num());
	TestEqual(TEXT("The resumed response uses one retry after backpressure"), FakeSocket.SendCallCount, 4);
	const bool bBytesMatch = FakeSocket.SentBytes.Num() == ExpectedBytes.Num()
		&& FMemory::Memcmp(FakeSocket.SentBytes.GetData(), ExpectedBytes.GetData(), ExpectedBytes.Num()) == 0;
	TestTrue(TEXT("Resumed bytes preserve UTF-8 content and newline framing"), bBytesMatch);

	Server.ClientSockets.Empty();
#endif

	return true;
}

bool FCortexTcpServerTerminalSendFailureTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexPartialSendSocket FakeSocket(3);
	FCortexTcpServer Server;
	FCortexTcpServer::FPendingResponseQueue& PendingQueue =
		Server.PendingResponses.FindOrAdd(&FakeSocket);
	FCortexTcpServer::FPendingResponse& PendingResponse = PendingQueue.Responses.AddDefaulted_GetRef();
	PendingResponse.Bytes = { 0x7B };

	TestFalse(
		TEXT("A terminal send error retires the response queue"),
		Server.HandleSendFailure(&FakeSocket, SE_ECONNRESET));
	TestTrue(TEXT("A terminal send failure closes the client connection"), FakeSocket.bClosed);
	TestFalse(TEXT("A terminal send failure removes queued response data"), Server.PendingResponses.Contains(&FakeSocket));
#endif

	return true;
}

bool FCortexTcpServerQueuedResponseBacklogLimitTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexPartialSendSocket AcceptedSocket(FCortexTcpServer::MaxMessageSize * 2);
	FCortexTcpServer AcceptedServer;
	FCortexTcpServer::FPendingResponseQueue& AcceptedQueue =
		AcceptedServer.PendingResponses.FindOrAdd(&AcceptedSocket);
	FCortexTcpServer::FPendingResponse& ActiveAcceptedResponse = AcceptedQueue.Responses.AddDefaulted_GetRef();
	ActiveAcceptedResponse.Bytes = { 0x61, 0x0A };
	ActiveAcceptedResponse.BytesSent = 1;
	ActiveAcceptedResponse.QueuedAt = FPlatformTime::Seconds();
	AcceptedSocket.SentBytes.Add(0x61);

	const FString BoundaryBacklog = FString::ChrN(FCortexTcpServer::MaxMessageSize - 1, TEXT('x'));
	TestTrue(
		TEXT("A response backlog exactly at the per-client byte budget is accepted"),
		AcceptedServer.SendResponse(&AcceptedSocket, BoundaryBacklog));
	TestFalse(TEXT("An in-budget response backlog does not close the client socket"), AcceptedSocket.bClosed);
	TestEqual(
		TEXT("The accepted active and queued frames retain exact newline-framed byte count"),
		AcceptedSocket.SentBytes.Num(),
		FCortexTcpServer::MaxMessageSize + 2);

	FCortexPartialSendSocket FakeSocket(FCortexTcpServer::MaxMessageSize * 2);
	FCortexTcpServer Server;
	FCortexTcpServer::FPendingResponseQueue& PendingQueue =
		Server.PendingResponses.FindOrAdd(&FakeSocket);
	FCortexTcpServer::FPendingResponse& ActiveResponse = PendingQueue.Responses.AddDefaulted_GetRef();
	ActiveResponse.Bytes = { 0x61, 0x0A };
	ActiveResponse.BytesSent = 1;
	ActiveResponse.QueuedAt = FPlatformTime::Seconds();
	FakeSocket.SentBytes.Add(0x61);

	const FString Backlog = FString::ChrN(FCortexTcpServer::MaxMessageSize, TEXT('x'));
	TestFalse(
		TEXT("A response backlog exceeding the per-client byte budget is rejected"),
		Server.SendResponse(&FakeSocket, Backlog));
	TestTrue(TEXT("Backlog overflow closes the client socket"), FakeSocket.bClosed);
	TestFalse(TEXT("Backlog overflow removes queued output"), Server.PendingResponses.Contains(&FakeSocket));
	constexpr int32 MaxExpectedQueuedFrames = 1024;

	FCortexPartialSendSocket FrameBoundarySocket(FCortexTcpServer::MaxMessageSize * 2);
	FCortexTcpServer FrameBoundaryServer;
	FCortexTcpServer::FPendingResponseQueue& FrameBoundaryQueue =
		FrameBoundaryServer.PendingResponses.FindOrAdd(&FrameBoundarySocket);
	FCortexTcpServer::FPendingResponse& ActiveBoundaryFrame =
		FrameBoundaryQueue.Responses.AddDefaulted_GetRef();
	ActiveBoundaryFrame.Bytes = { 0x61, 0x0A };
	ActiveBoundaryFrame.BytesSent = 1;
	ActiveBoundaryFrame.QueuedAt = FPlatformTime::Seconds();
	FrameBoundarySocket.SentBytes.Add(0x61);
	for (int32 QueuedFrameIndex = 0; QueuedFrameIndex < MaxExpectedQueuedFrames - 1; ++QueuedFrameIndex)
	{
		FCortexTcpServer::FPendingResponse& QueuedFrame =
			FrameBoundaryQueue.Responses.AddDefaulted_GetRef();
		QueuedFrame.Bytes = { 0x78, 0x0A };
		QueuedFrame.QueuedAt = FPlatformTime::Seconds();
		FrameBoundaryQueue.BacklogBytes += QueuedFrame.Bytes.Num();
	}
	TestTrue(
		TEXT("Exactly the allowed queued-frame count is accepted"),
		FrameBoundaryServer.SendResponse(&FrameBoundarySocket, TEXT("x")));
	TestFalse(TEXT("In-count responses do not close the client socket"), FrameBoundarySocket.bClosed);

	FCortexPartialSendSocket FrameOverflowSocket(FCortexTcpServer::MaxMessageSize * 2);
	FCortexTcpServer FrameOverflowServer;
	FCortexTcpServer::FPendingResponseQueue& FrameOverflowQueue =
		FrameOverflowServer.PendingResponses.FindOrAdd(&FrameOverflowSocket);
	FCortexTcpServer::FPendingResponse& ActiveOverflowFrame =
		FrameOverflowQueue.Responses.AddDefaulted_GetRef();
	ActiveOverflowFrame.Bytes = { 0x61, 0x0A };
	ActiveOverflowFrame.BytesSent = 1;
	ActiveOverflowFrame.QueuedAt = FPlatformTime::Seconds();
	FrameOverflowSocket.SentBytes.Add(0x61);
	for (int32 QueuedFrameIndex = 0; QueuedFrameIndex < MaxExpectedQueuedFrames; ++QueuedFrameIndex)
	{
		FCortexTcpServer::FPendingResponse& QueuedFrame =
			FrameOverflowQueue.Responses.AddDefaulted_GetRef();
		QueuedFrame.Bytes = { 0x78, 0x0A };
		QueuedFrame.QueuedAt = FPlatformTime::Seconds();
		FrameOverflowQueue.BacklogBytes += QueuedFrame.Bytes.Num();
	}
	TestFalse(
		TEXT("A response beyond the per-client queued-frame limit is rejected"),
		FrameOverflowServer.SendResponse(&FrameOverflowSocket, TEXT("x")));
	TestTrue(TEXT("Queued-frame overflow closes the client socket"), FrameOverflowSocket.bClosed);
	TestFalse(TEXT("Queued-frame overflow removes all retained frames"), FrameOverflowServer.PendingResponses.Contains(&FrameOverflowSocket));


#endif

	return true;
}

bool FCortexTcpServerProcessesBufferedRequestsWithoutNewDataTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexPartialSendSocket FakeSocket(FCortexTcpServer::MaxMessageSize * 2);
	FCortexTcpServer Server;
	TArray<FString> ProcessedCommands;
	Server.CommandDispatcher =
		[&ProcessedCommands](
			const FString& Command,
			const TSharedPtr<FJsonObject>& /*Params*/,
			FDeferredResponseCallback /*DeferredCallback*/)
		{
			ProcessedCommands.Add(Command);
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		};

	Server.ClientSockets.Add(&FakeSocket);
	Server.ReceiveBuffers.Add(
		&FakeSocket,
		TEXT("{\"command\":\"core.first\",\"id\":\"first\"}\n")
		TEXT("{\"command\":\"core.second\",\"id\":\"second\"}\n"));
	Server.ProcessClientData();

	TestEqual(TEXT("Buffered pipelined commands run without new socket data"), ProcessedCommands.Num(), 2);
	if (ProcessedCommands.Num() == 2)
	{
		TestEqual(TEXT("The first buffered command keeps its order"), ProcessedCommands[0], FString(TEXT("core.first")));
		TestEqual(TEXT("The second buffered command is not stranded"), ProcessedCommands[1], FString(TEXT("core.second")));
	}
	Server.ClientSockets.Empty();
#endif

	return true;
}

bool FCortexTcpServerDeferredSendFailureRetiresClientTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexTcpServer Server;
	FCortexPartialSendSocket* FakeSocket =
		new FCortexPartialSendSocket(FCortexTcpServer::MaxMessageSize * 2);
	Server.ClientSockets.Add(FakeSocket);

	FCortexTcpServer::FPendingResponseQueue& Queue = Server.PendingResponses.FindOrAdd(FakeSocket);
	FCortexTcpServer::FPendingResponse& ActiveResponse = Queue.Responses.AddDefaulted_GetRef();
	ActiveResponse.Bytes = { 0x61, 0x0A };
	ActiveResponse.BytesSent = 1;
	ActiveResponse.QueuedAt = FPlatformTime::Seconds();
	Queue.BacklogBytes = FCortexTcpServer::MaxQueuedResponseBacklogBytesPerClient;
	FakeSocket->SentBytes.Add(0x61);

	FCortexPendingDeferred CurrentDeferred;
	CurrentDeferred.ClientSocket = FakeSocket;
	CurrentDeferred.RequestId = TEXT("current");
	CurrentDeferred.StartTime = FPlatformTime::Seconds();
	Server.PendingDeferred.Add(1, CurrentDeferred);
	FCortexPendingDeferred SiblingDeferred = CurrentDeferred;
	SiblingDeferred.RequestId = TEXT("sibling");
	Server.PendingDeferred.Add(2, SiblingDeferred);

	Server.SendDeferredResponse(
		1,
		FCortexCommandRouter::Error(TEXT("TEST_ERROR"), TEXT("deferred send exceeds backlog")));
	TestTrue(TEXT("Backlog overflow closes the deferred client's socket"), FakeSocket->bClosed);

	Server.ProcessClientData();
	TestTrue(TEXT("A deferred send failure retires the closed client"), Server.ClientSockets.IsEmpty());
	TestTrue(TEXT("Client retirement clears sibling deferred responses"), Server.PendingDeferred.IsEmpty());
	TestFalse(TEXT("Client retirement removes pending response frames"), Server.PendingResponses.Contains(FakeSocket));

	if (Server.ClientSockets.Contains(FakeSocket))
	{
		Server.ClientSockets.RemoveSingle(FakeSocket);
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(FakeSocket);
	}
#endif

	return true;
}

bool FCortexTcpServerAcceptsNonBlockingSocketTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexPartialSendSocket FakeSocket(3);
	FCortexTcpServer Server;

	TestTrue(
		TEXT("The connection callback accepts a nonblocking socket"),
		Server.HandleConnectionAccepted(
			&FakeSocket,
			FIPv4Endpoint(FIPv4Address::InternalLoopback, 8742)));
	TestTrue(TEXT("Accepted sockets are configured nonblocking"), FakeSocket.bWasSetNonBlocking);

	Server.PendingClientSockets.Empty();
#endif

	return true;
}
