
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

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Test double for FSocket that accepts at most MaxBytesPerSend bytes per Send() call.
 * Production code never constructs this; it exists so the framing test can prove a
 * response is delivered completely across partial writes on a single connection.
 */
class FCortexPartialSendSocket : public FSocket
{
public:
	explicit FCortexPartialSendSocket(int32 InMaxBytesPerSend)
		: FSocket(SOCKTYPE_Streaming, TEXT("CortexPartialSendSocket"), NAME_None)
		, MaxBytesPerSend(InMaxBytesPerSend)
	{
	}

	/** Accepts a partial write, records it, and reports how many bytes were taken. */
	virtual bool Send(const uint8* Data, int32 Count, int32& BytesSent) override
	{
		++SendCallCount;
		BytesSent = FMath::Min(Count, MaxBytesPerSend);
		SentBytes.Append(Data, BytesSent);
		return true;
	}

	// The remaining FSocket surface is never exercised by SendResponse.
	virtual bool Shutdown(ESocketShutdownMode /*Mode*/) override { return true; }
	virtual bool Close() override { return true; }
	virtual bool Bind(const FInternetAddr& /*Addr*/) override { return true; }
	virtual bool Connect(const FInternetAddr& /*Addr*/) override { return true; }
	virtual bool Listen(int32 /*MaxBacklog*/) override { return true; }
	virtual bool WaitForPendingConnection(bool& /*bHasPendingConnection*/, const FTimespan& /*WaitTime*/) override { return false; }
	virtual bool HasPendingData(uint32& /*PendingDataSize*/) override { return false; }
	virtual FSocket* Accept(const FString& /*InSocketDescription*/) override { return nullptr; }
	virtual FSocket* Accept(FInternetAddr& /*OutAddr*/, const FString& /*InSocketDescription*/) override { return nullptr; }
	virtual bool Wait(ESocketWaitConditions::Type /*Condition*/, FTimespan /*WaitTime*/) override { return false; }
	virtual ESocketConnectionState GetConnectionState() override { return SCS_Connected; }
	virtual void GetAddress(FInternetAddr& /*OutAddr*/) override {}
	virtual bool GetPeerAddress(FInternetAddr& /*OutAddr*/) override { return true; }
	virtual bool SetNonBlocking(bool /*bIsNonBlocking*/) override { return true; }
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

	/** Number of Send() calls accepted by the socket. */
	int32 SendCallCount = 0;

	/** Every byte handed to Send() across all calls, in order. */
	TArray<uint8> SentBytes;

private:
	int32 MaxBytesPerSend;
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
