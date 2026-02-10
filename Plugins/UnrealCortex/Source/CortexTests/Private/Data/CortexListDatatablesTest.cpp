
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
	FUDBListDatatablesTest,
	"UDB.Commands.ListDatatables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBListDatatablesTest::RunTest(const FString& Parameters)
{
	// Arrange: Start the TCP server on a test port (different from ping test)
	const int32 TestPort = 18743;
	FUDBCommandHandler CommandHandler;
	FUDBTcpServer Server;
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

	FSocket* ClientSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UDBTestClient"), false);
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

	// Act: Send a list_datatables command
	FString ListCommand = TEXT("{\"command\":\"list_datatables\",\"params\":{}}\n");
	FTCHARToUTF8 Utf8Converter(*ListCommand);
	int32 BytesSent = 0;
	const bool bSent = ClientSocket->Send(
		reinterpret_cast<const uint8*>(Utf8Converter.Get()),
		Utf8Converter.Length(),
		BytesSent
	);
	TestTrue(TEXT("Client should send list_datatables command"), bSent);
	TestEqual(TEXT("All bytes should be sent"), BytesSent, Utf8Converter.Length());

	// Allow time for the server to process and respond
	FPlatformProcess::Sleep(0.2f);

	// Tick the server's processing (simulate game thread tick)
	FTSTicker::GetCoreTicker().Tick(0.016f);

	FPlatformProcess::Sleep(0.1f);

	// Assert: Read the response (may arrive in multiple chunks for large payloads)
	FString ResponseString;
	uint8 RecvBuffer[8192];
	int32 BytesRead = 0;

	bool bReceivedResponse = false;
	for (int32 Attempt = 0; Attempt < 30; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.05f);

		uint32 PendingDataSize = 0;
		while (ClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
		{
			if (ClientSocket->Recv(RecvBuffer, sizeof(RecvBuffer) - 1, BytesRead) && BytesRead > 0)
			{
				RecvBuffer[BytesRead] = 0;
				FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RecvBuffer), BytesRead);
				ResponseString.Append(Converter.Get(), Converter.Length());
			}
			else
			{
				break;
			}
		}

		// Check if we have a complete newline-delimited response
		if (ResponseString.Contains(TEXT("\n")))
		{
			// Trim the trailing newline for JSON parsing
			ResponseString.TrimStartAndEndInline();
			bReceivedResponse = true;
			break;
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
			// Verify success field
			bool bSuccess = false;
			TestTrue(TEXT("Response should have 'success' field"), JsonObject->TryGetBoolField(TEXT("success"), bSuccess));
			TestTrue(TEXT("Response 'success' should be true"), bSuccess);

			// Verify data object exists
			const TSharedPtr<FJsonObject>* DataObject = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
			{
				// Verify datatables array exists
				const TArray<TSharedPtr<FJsonValue>>* DatatablesArray = nullptr;
				if ((*DataObject)->TryGetArrayField(TEXT("datatables"), DatatablesArray))
				{
					TestNotNull(TEXT("datatables should be a JSON array"), DatatablesArray);

					// Verify each entry has the expected fields
					for (const TSharedPtr<FJsonValue>& Entry : *DatatablesArray)
					{
						const TSharedPtr<FJsonObject>* EntryObject = nullptr;
						if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject != nullptr)
						{
							TestTrue(TEXT("Entry should have 'name' field"), (*EntryObject)->HasField(TEXT("name")));
							TestTrue(TEXT("Entry should have 'path' field"), (*EntryObject)->HasField(TEXT("path")));
							TestTrue(TEXT("Entry should have 'row_struct' field"), (*EntryObject)->HasField(TEXT("row_struct")));
							TestTrue(TEXT("Entry should have 'row_count' field"), (*EntryObject)->HasField(TEXT("row_count")));

							// Only validate first entry to keep test output clean
							break;
						}
					}
				}
				else
				{
					AddError(TEXT("Data should have 'datatables' array field"));
				}
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
