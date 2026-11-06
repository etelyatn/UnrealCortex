
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "CortexTcpServer.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Operations/CortexDataTableOps.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Tests/CortexTestDataAsset.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
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
	CommandHandler.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());
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
						// top_fields is only present for loaded assets (row_count >= 0)
						const double RowCount = (*EntryObject)->GetNumberField(TEXT("row_count"));
						if (RowCount >= 0)
						{
							TestTrue(TEXT("Loaded datatable entry should have 'top_fields'"), (*EntryObject)->HasField(TEXT("top_fields")));
						}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogNoEngineAssetsTest,
	"Cortex.Data.Catalog.NoEngineAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCortexDataCatalogNoEngineAssetsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FCortexCommandResult Result = FCortexDataTableOps::GetDataCatalog(Params);

	TestTrue(TEXT("GetDataCatalog succeeds"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddError(TEXT("GetDataCatalog returned no data"));
		return false;
	}

	// Check DataAsset example_path starts with /Game/
	const TArray<TSharedPtr<FJsonValue>>* AssetArray = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("data_asset_classes"), AssetArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *AssetArray)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (Entry->TryGetObject(EntryObj))
			{
				FString ExamplePath;
				if ((*EntryObj)->TryGetStringField(TEXT("example_path"), ExamplePath))
				{
					TestTrue(
						FString::Printf(TEXT("DataAsset example_path starts with /Game/: %s"), *ExamplePath),
						ExamplePath.StartsWith(TEXT("/Game/"))
					);
				}
			}
		}
	}

	// Check StringTable paths start with /Game/
	const TArray<TSharedPtr<FJsonValue>>* StringArray = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("string_tables"), StringArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *StringArray)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (Entry->TryGetObject(EntryObj))
			{
				FString Path;
				if ((*EntryObj)->TryGetStringField(TEXT("path"), Path))
				{
					TestTrue(
						FString::Printf(TEXT("StringTable path starts with /Game/: %s"), *Path),
						Path.StartsWith(TEXT("/Game/"))
					);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogNoEngineTablesTest,
	"Cortex.Data.Catalog.NoEngineDataTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter
)

bool FCortexDataCatalogNoEngineTablesTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FCortexCommandResult Result = FCortexDataTableOps::GetDataCatalog(Params);

	TestTrue(TEXT("GetDataCatalog succeeds"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddError(TEXT("GetDataCatalog returned no data"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* DatatablesArray = nullptr;
	TestTrue(TEXT("Has datatables array"), Result.Data->TryGetArrayField(TEXT("datatables"), DatatablesArray));

	if (DatatablesArray != nullptr)
	{
		TestTrue(TEXT("DataTables array is non-empty (CortexSandbox has tables in Content/Data/)"), DatatablesArray->Num() > 0);
		for (const TSharedPtr<FJsonValue>& Entry : *DatatablesArray)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (Entry->TryGetObject(EntryObj))
			{
				FString Path = (*EntryObj)->GetStringField(TEXT("path"));
				TestTrue(
					FString::Printf(TEXT("DataTable path starts with /Game/: %s"), *Path),
					Path.StartsWith(TEXT("/Game/"))
				);
			}
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Task 8 — lean Data catalog: section selection and opt-in asset path inventory
//
// Section selectors come from docs/plans/2026-10-13-token-efficiency-design.md §3.4:
// datatables / gameplay_tags / data_assets / string_tables / curve_tables, all by default,
// with include_asset_paths=false by default. The class entry under test is identified through
// this test's own fixture class, never through an assumption about the project's class
// population.
// ---------------------------------------------------------------------------

namespace
{
	/** One in-memory DataAsset fixture of the existing CortexData test class under /Game, so the
	 *  catalog has a deterministic class entry to assert against. It is never saved to disk: the
	 *  AssetRegistry sees it through AssetCreated, so no package-file watcher events occur. */
	class FCortexCatalogFixture
	{
	public:
		explicit FCortexCatalogFixture(const FString& NameSuffix)
		{
			const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexDataCatalogFixture_%s"), *NameSuffix);
			const FString AssetName = FString::Printf(TEXT("DA_CortexDataCatalogFixture_%s"), *NameSuffix);

			Package = CreatePackage(*PackageName);
			Asset = NewObject<UCortexTestDataAsset>(
				Package,
				UCortexTestDataAsset::StaticClass(),
				FName(*AssetName),
				RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Asset);
		}

		~FCortexCatalogFixture()
		{
			if (Asset == nullptr)
			{
				return;
			}

			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();

			if (Package != nullptr)
			{
				Package->ClearFlags(RF_Public | RF_Standalone);
				Package->MarkAsGarbage();
			}
		}

		FString ObjectPath() const { return Asset != nullptr ? Asset->GetPathName() : FString(); }
		static FString ClassName() { return UCortexTestDataAsset::StaticClass()->GetName(); }

	private:
		UPackage* Package = nullptr;
		UCortexTestDataAsset* Asset = nullptr;
	};

	/** The fixture class entry of the catalog's data-asset class section, matched by class name. */
	TSharedPtr<FJsonObject> FindCatalogDataAssetClassEntry(
		const TSharedPtr<FJsonObject>& CatalogData,
		const FString& ClassName)
	{
		const TArray<TSharedPtr<FJsonValue>>* ClassEntries = nullptr;
		if (!CatalogData.IsValid()
			|| !CatalogData->TryGetArrayField(TEXT("data_asset_classes"), ClassEntries)
			|| ClassEntries == nullptr)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& ClassValue : *ClassEntries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!ClassValue.IsValid() || !ClassValue->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}

			FString EntryClassName;
			if ((*Entry)->TryGetStringField(TEXT("class_name"), EntryClassName) && EntryClassName == ClassName)
			{
				return *Entry;
			}
		}

		return nullptr;
	}

	/** Entry count of one catalog section. A section that was not selected is not built, so an
	 *  absent array and an empty array both mean zero entries. */
	int32 CatalogSectionEntryCount(const TSharedPtr<FJsonObject>& CatalogData, const FString& FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!CatalogData.IsValid() || !CatalogData->TryGetArrayField(FieldName, Entries) || Entries == nullptr)
		{
			return 0;
		}

		return Entries->Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogDefaultClassInventoryTest,
	"Cortex.Data.Catalog.DefaultClassInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogDefaultClassInventoryTest::RunTest(const FString& Parameters)
{
	FCortexCatalogFixture Fixture(TEXT("DefaultClassInventory"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_data_catalog"), Params);
	TestTrue(TEXT("GetDataCatalog succeeds"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddError(TEXT("GetDataCatalog returned no data"));
		return false;
	}

	const TSharedPtr<FJsonObject> ClassEntry =
		FindCatalogDataAssetClassEntry(Result.Data, FCortexCatalogFixture::ClassName());
	TestTrue(TEXT("Default catalog exposes the fixture's DataAsset class"), ClassEntry.IsValid());

	if (!ClassEntry.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Class entry keeps its count"), ClassEntry->HasField(TEXT("count")));
	TestTrue(TEXT("Class entry keeps its example_path"), ClassEntry->HasField(TEXT("example_path")));
	TestTrue(TEXT("Class count includes the fixture asset"), ClassEntry->GetIntegerField(TEXT("count")) >= 1);
	TestFalse(TEXT("Default catalog omits the full asset path inventory"),
		ClassEntry->HasField(TEXT("asset_paths")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogSelectedSectionsTest,
	"Cortex.Data.Catalog.SelectedSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogSelectedSectionsTest::RunTest(const FString& Parameters)
{
	FCortexCatalogFixture Fixture(TEXT("SelectedSections"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueString>(TEXT("datatables")) });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_data_catalog"), Params);
	TestTrue(TEXT("Selected-section catalog succeeds"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddError(TEXT("Selected-section catalog returned no data"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Datatables = nullptr;
	TestTrue(TEXT("Selected datatables section is built"),
		Result.Data->TryGetArrayField(TEXT("datatables"), Datatables) && Datatables != nullptr);
	TestTrue(TEXT("Selected datatables section is populated"),
		Datatables != nullptr && Datatables->Num() > 0);

	TestEqual(TEXT("Unselected data_assets section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("data_asset_classes")), 0);
	TestEqual(TEXT("Unselected string_tables section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("string_tables")), 0);
	TestEqual(TEXT("Unselected gameplay_tags section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("tag_prefixes")), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogIncludeAssetPathsTest,
	"Cortex.Data.Catalog.IncludeAssetPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogIncludeAssetPathsTest::RunTest(const FString& Parameters)
{
	FCortexCatalogFixture Fixture(TEXT("IncludeAssetPaths"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetBoolField(TEXT("include_asset_paths"), true);
	const FCortexCommandResult Full = Router.Execute(TEXT("data.get_data_catalog"), Params);
	TestTrue(TEXT("Full catalog succeeds"), Full.bSuccess);

	if (!Full.bSuccess || !Full.Data.IsValid())
	{
		AddError(TEXT("Full catalog returned no data"));
		return false;
	}

	const TSharedPtr<FJsonObject> ClassEntry =
		FindCatalogDataAssetClassEntry(Full.Data, FCortexCatalogFixture::ClassName());
	TestTrue(TEXT("Full catalog exposes the fixture's DataAsset class"), ClassEntry.IsValid());

	if (!ClassEntry.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Explicit include_asset_paths=true restores the path inventory"),
		ClassEntry->HasField(TEXT("asset_paths")));
	TestEqual(TEXT("path inventory matches class count"),
		ClassEntry->GetArrayField(TEXT("asset_paths")).Num(), ClassEntry->GetIntegerField(TEXT("count")));

	bool bContainsFixturePath = false;
	for (const TSharedPtr<FJsonValue>& AssetPathValue : ClassEntry->GetArrayField(TEXT("asset_paths")))
	{
		FString AssetPath;
		if (AssetPathValue.IsValid() && AssetPathValue->TryGetString(AssetPath) && AssetPath == Fixture.ObjectPath())
		{
			bContainsFixturePath = true;
		}
	}
	TestTrue(TEXT("Full path inventory contains the fixture's exact object path"), bContainsFixturePath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogDataAssetsSectionTest,
	"Cortex.Data.Catalog.DataAssetsSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogDataAssetsSectionTest::RunTest(const FString& Parameters)
{
	FCortexCatalogFixture Fixture(TEXT("DataAssetsSection"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueString>(TEXT("data_assets")) });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_data_catalog"), Params);
	TestTrue(TEXT("Data-assets-section catalog succeeds"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddError(TEXT("Data-assets-section catalog returned no data"));
		return false;
	}

	const TSharedPtr<FJsonObject> ClassEntry =
		FindCatalogDataAssetClassEntry(Result.Data, FCortexCatalogFixture::ClassName());
	TestTrue(TEXT("Selected data_assets section is built"), ClassEntry.IsValid());

	if (!ClassEntry.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("Section selection keeps the default path inventory out"),
		ClassEntry->HasField(TEXT("asset_paths")));
	TestEqual(TEXT("Unselected datatables section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("datatables")), 0);
	TestEqual(TEXT("Unselected string_tables section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("string_tables")), 0);
	TestEqual(TEXT("Unselected gameplay_tags section is not built"),
		CatalogSectionEntryCount(Result.Data, TEXT("tag_prefixes")), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogInvalidSectionTest,
	"Cortex.Data.Catalog.InvalidSectionRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogInvalidSectionTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueString>(TEXT("not_a_catalog_section")) });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_data_catalog"), Params);
	TestFalse(TEXT("Unknown catalog section refuses instead of building every section"), Result.bSuccess);
	TestFalse(TEXT("Refusal carries a machine-readable error code"), Result.ErrorCode.IsEmpty());
	TestFalse(TEXT("Refusal carries an error message"), Result.ErrorMessage.IsEmpty());

	return true;
}
