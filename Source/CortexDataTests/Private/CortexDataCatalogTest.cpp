
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "CortexTcpServer.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogTest,
	"Cortex.Data.GetDataCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogTest::RunTest(const FString& Parameters)
{
	// Arrange: Start TCP server on a unique test port
	const int32 TestPort = 18744;
	FCortexCommandRouter CommandHandler;
	CommandHandler.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.0"),
		MakeShared<FCortexDataCommandHandler>());
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

	// Create client socket and connect
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

	FPlatformProcess::Sleep(0.1f);

	// Act: Send get_data_catalog command
	FString Command = TEXT("{\"command\":\"data.get_data_catalog\",\"params\":{}}\n");
	FTCHARToUTF8 Utf8Converter(*Command);
	int32 BytesSent = 0;
	const bool bSent = ClientSocket->Send(
		reinterpret_cast<const uint8*>(Utf8Converter.Get()),
		Utf8Converter.Length(),
		BytesSent
	);
	TestTrue(TEXT("Client should send get_data_catalog command"), bSent);

	// Wait for response
	FPlatformProcess::Sleep(0.2f);
	FTSTicker::GetCoreTicker().Tick(0.016f);
	FPlatformProcess::Sleep(0.1f);

	// Read response
	FString ResponseString;
	uint8 RecvBuffer[65536];
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

		if (ResponseString.Contains(TEXT("\n")))
		{
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
			// Verify success
			bool bSuccess = false;
			TestTrue(TEXT("Response should have 'success' field"), JsonObject->TryGetBoolField(TEXT("success"), bSuccess));
			TestTrue(TEXT("Response 'success' should be true"), bSuccess);

			// Verify data object
			const TSharedPtr<FJsonObject>* DataObject = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
			{
				// Verify all 4 required sections exist
				const TArray<TSharedPtr<FJsonValue>>* DatatablesArray = nullptr;
				TestTrue(TEXT("Data should have 'datatables' array"),
					(*DataObject)->TryGetArrayField(TEXT("datatables"), DatatablesArray) && DatatablesArray != nullptr);

				const TArray<TSharedPtr<FJsonValue>>* TagPrefixesArray = nullptr;
				TestTrue(TEXT("Data should have 'tag_prefixes' array"),
					(*DataObject)->TryGetArrayField(TEXT("tag_prefixes"), TagPrefixesArray) && TagPrefixesArray != nullptr);

				const TArray<TSharedPtr<FJsonValue>>* DataAssetClassesArray = nullptr;
				TestTrue(TEXT("Data should have 'data_asset_classes' array"),
					(*DataObject)->TryGetArrayField(TEXT("data_asset_classes"), DataAssetClassesArray) && DataAssetClassesArray != nullptr);

				const TArray<TSharedPtr<FJsonValue>>* StringTablesArray = nullptr;
				TestTrue(TEXT("Data should have 'string_tables' array"),
					(*DataObject)->TryGetArrayField(TEXT("string_tables"), StringTablesArray) && StringTablesArray != nullptr);

				// Verify datatable entries have expected fields (check first entry)
				if (DatatablesArray != nullptr && DatatablesArray->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* EntryObject = nullptr;
					if ((*DatatablesArray)[0].IsValid() && (*DatatablesArray)[0]->TryGetObject(EntryObject) && EntryObject != nullptr)
					{
						TestTrue(TEXT("Datatable entry should have 'name'"), (*EntryObject)->HasField(TEXT("name")));
						TestTrue(TEXT("Datatable entry should have 'path'"), (*EntryObject)->HasField(TEXT("path")));
						TestTrue(TEXT("Datatable entry should have 'row_struct'"), (*EntryObject)->HasField(TEXT("row_struct")));
						TestTrue(TEXT("Datatable entry should have 'row_count'"), (*EntryObject)->HasField(TEXT("row_count")));
						TestTrue(TEXT("Datatable entry should have 'is_composite'"), (*EntryObject)->HasField(TEXT("is_composite")));
						TestTrue(TEXT("Datatable entry should have 'top_fields'"), (*EntryObject)->HasField(TEXT("top_fields")));
					}
				}

				// Verify tag prefix entries have expected fields
				if (TagPrefixesArray != nullptr && TagPrefixesArray->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* EntryObject = nullptr;
					if ((*TagPrefixesArray)[0].IsValid() && (*TagPrefixesArray)[0]->TryGetObject(EntryObject) && EntryObject != nullptr)
					{
						TestTrue(TEXT("Tag prefix entry should have 'prefix'"), (*EntryObject)->HasField(TEXT("prefix")));
						TestTrue(TEXT("Tag prefix entry should have 'count'"), (*EntryObject)->HasField(TEXT("count")));
					}
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
