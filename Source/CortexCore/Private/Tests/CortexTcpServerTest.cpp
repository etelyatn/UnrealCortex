
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
	for (int32 Port = TestPort + 1; Port < TestPort + 100; ++Port)
	{
		FSocket* BlockingSocket = SocketSubsystem->CreateSocket(
			NAME_Stream,
			*FString::Printf(TEXT("CortexExclusivePortBlocker_%d"), Port),
			false);
		if (BlockingSocket == nullptr)
		{
			AddError(FString::Printf(TEXT("Failed to create blocking socket for port %d"), Port));
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
			for (FSocket* Socket : BlockingSockets)
			{
				Socket->Close();
				SocketSubsystem->DestroySocket(Socket);
			}
			Server1.Stop();
			return true;
		}

		BlockingSockets.Add(BlockingSocket);
	}

	// Act: Attempt to bind a second server on the same port.
	AddExpectedError(
		TEXT("LogCortex: Failed to bind TCP server on ports 18900-18999"),
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
	for (FSocket* Socket : BlockingSockets)
	{
		Socket->Close();
		SocketSubsystem->DestroySocket(Socket);
	}

	return true;
}
