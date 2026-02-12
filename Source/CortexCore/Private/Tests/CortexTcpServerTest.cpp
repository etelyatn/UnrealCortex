
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

bool FCortexTcpServerPingPongTest::RunTest(const FString& Parameters)
{
	// Arrange: Start the TCP server on a test port
	const int32 TestPort = 18742;
	FCortexCommandRouter CommandHandler;
	FCortexTcpServer Server;
	const bool bStarted = Server.Start(TestPort,
		[&CommandHandler](const FString& Command, const TSharedPtr<FJsonObject>& Params)
		{
			return CommandHandler.Execute(Command, Params);
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
