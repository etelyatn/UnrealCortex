#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Tests/CortexDataLocalizationTestTypes.h"
#include "Tests/CortexTestDataAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectHash.h"

namespace
{
	const TCHAR* ExportTestRoot = TEXT("/Game/CortexExportTests");

	FCortexCommandRouter CreateDataExportTestRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	FCortexDataLocalizationTestRow MakeExportRow(const FString& Title, const FString& StepText)
	{
		FCortexDataLocalizationTestRow Row;
		Row.Title = FText::FromString(Title);
		Row.row_name = FString::Printf(TEXT("Data %s"), *Title);

		FCortexDataLocalizationStepTestRow Step;
		Step.Description = FText::FromString(StepText);
		Row.Steps.Add(Step);

		return Row;
	}

	FString PackagePathForAsset(const FString& RunId, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s/%s"), ExportTestRoot, *RunId, *AssetName);
	}

	bool SupportedCommandNamesContain(const TArray<FCortexCommandInfo>& Commands, const FString& CommandName)
	{
		for (const FCortexCommandInfo& Command : Commands)
		{
			if (Command.Name == CommandName)
			{
				return true;
			}
		}

		return false;
	}

	void DeletePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	void CleanupLoadedPackage(const FString& PackageName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			TArray<UObject*> PackageObjects;
			GetObjectsWithPackage(Package, PackageObjects, false);
			for (UObject* Object : PackageObjects)
			{
				if (Object == nullptr)
				{
					continue;
				}

				FAssetRegistryModule::AssetDeleted(Object);
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}

			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	void CleanupPackageByName(const FString& PackageName)
	{
		CleanupLoadedPackage(PackageName);
		DeletePackageFile(PackageName);
	}

	class FCortexExportSkipPackageWarningCapture final : public FOutputDevice
	{
	public:
		int32 SkipPackageWarnings = 0;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			(void)Category;

			if (Verbosity != ELogVerbosity::Warning || V == nullptr)
			{
				return;
			}

			const FString Message(V);
			if (Message.Contains(TEXT("SkipPackage")))
			{
				++SkipPackageWarnings;
			}
		}
	};

	class FCortexDataExportTestFixture
	{
	public:
		FCortexDataExportTestFixture()
			: RunId(FString::Printf(TEXT("Run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FCortexDataExportTestFixture()
		{
			Cleanup();
		}

		UDataTable* CreateRegularDataTable()
		{
			UDataTable* Table = CreateAsset<UDataTable>(TEXT("DT_CortexExportRows"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

			Table->AddRow(TEXT("zeta"), MakeExportRow(TEXT("Zeta"), TEXT("Third inserted")));
			Table->AddRow(TEXT("alpha"), MakeExportRow(TEXT("Alpha"), TEXT("Second inserted")));
			Table->AddRow(TEXT("middle"), MakeExportRow(TEXT("Middle"), TEXT("First inserted")));

			return Table;
		}

		UCompositeDataTable* CreateCompositeDataTable()
		{
			UDataTable* ParentA = CreateAsset<UDataTable>(TEXT("DT_CortexExportParentA"));
			if (ParentA == nullptr)
			{
				return nullptr;
			}

			ParentA->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();
			ParentA->AddRow(TEXT("shared"), MakeExportRow(TEXT("Base Shared"), TEXT("Base parent")));
			ParentA->AddRow(TEXT("alpha"), MakeExportRow(TEXT("Alpha Parent"), TEXT("Base only")));

			UDataTable* ParentB = CreateAsset<UDataTable>(TEXT("DT_CortexExportParentB"));
			if (ParentB == nullptr)
			{
				return nullptr;
			}

			ParentB->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();
			ParentB->AddRow(TEXT("shared"), MakeExportRow(TEXT("Override Shared"), TEXT("Override parent")));
			ParentB->AddRow(TEXT("beta"), MakeExportRow(TEXT("Beta Parent"), TEXT("Override only")));

			UCompositeDataTable* Composite = CreateAsset<UCompositeDataTable>(TEXT("DT_CortexExportComposite"));
			if (Composite == nullptr)
			{
				return nullptr;
			}

			Composite->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

			TArray<UDataTable*> Parents;
			Parents.Add(ParentA);
			Parents.Add(ParentB);
			Composite->AppendParentTables(Parents);

			return Composite;
		}

		UStringTable* CreateStringTable()
		{
			UStringTable* Table = CreateAsset<UStringTable>(TEXT("ST_CortexExportText"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->GetMutableStringTable()->SetNamespace(TEXT("CortexExportTests"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("zeta.key"), TEXT("Zeta text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("alpha.key"), TEXT("Alpha text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("middle.key"), TEXT("Middle text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("ignored.other"), TEXT("Ignored text"));
			return Table;
		}

		UCortexTestDataAsset* CreateDataAsset(const FString& AssetName = TEXT("DA_CortexExportFixture"))
		{
			UCortexTestDataAsset* Asset = CreateAsset<UCortexTestDataAsset>(AssetName);
			if (Asset == nullptr)
			{
				return nullptr;
			}

			Asset->TestProperty = TEXT("Editable export value");
			Asset->TestNumber = 42;
			Asset->ExportTransientProperty = TEXT("Transient value");
			Asset->TransientExportBlocked = TEXT("Non-editable transient value");
#if WITH_EDITORONLY_DATA
			Asset->ExportEditorOnlyProperty = TEXT("Editor-only value");
#endif
			Asset->ExportInternalProperty = TEXT("Internal value");
			Asset->EditableExportAllowed = TEXT("Editable export allowed value");
			return Asset;
		}

		FString MakeSavedOutputPath(const FString& FileName) const
		{
			return FPaths::Combine(GetSavedRunDir(), FileName);
		}

		FString MakeSavedOutputDir(const FString& DirName) const
		{
			return FPaths::Combine(GetSavedRunDir(), DirName);
		}

		bool TryReadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError) const
		{
			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *FilePath))
			{
				OutError = FString::Printf(TEXT("Could not read JSON file: %s"), *FilePath);
				return false;
			}

			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
			if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
			{
				OutError = FString::Printf(TEXT("Could not parse JSON file: %s"), *FilePath);
				return false;
			}

			return true;
		}

		bool TryReadFileBytes(const FString& FilePath, TArray<uint8>& OutBytes, FString& OutError) const
		{
			if (!FFileHelper::LoadFileToArray(OutBytes, *FilePath))
			{
				OutError = FString::Printf(TEXT("Could not read file bytes: %s"), *FilePath);
				return false;
			}

			return true;
		}

		void Cleanup()
		{
			IFileManager::Get().DeleteDirectory(*GetSavedRunDir(), false, true);

			for (int32 Index = CreatedPackageNames.Num() - 1; Index >= 0; --Index)
			{
				CleanupPackageByName(CreatedPackageNames[Index]);
			}
			CreatedPackageNames.Empty();
		}

	private:
		FString GetSavedRunDir() const
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexExportTests"), RunId);
		}

		template <typename AssetType>
		AssetType* CreateAsset(const FString& AssetName)
		{
			const FString PackageName = PackagePathForAsset(RunId, AssetName);
			if (FindPackage(nullptr, *PackageName) != nullptr || FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}

			UPackage* Package = CreatePackage(*PackageName);
			AssetType* Asset = NewObject<AssetType>(
				Package,
				AssetType::StaticClass(),
				FName(*AssetName),
				RF_Public | RF_Standalone);

			FAssetRegistryModule::AssetCreated(Asset);

			CreatedPackageNames.Add(PackageName);
			return Asset;
		}

		FString RunId;
		TArray<FString> CreatedPackageNames;
	};

	void AddStringArrayField(TSharedRef<FJsonObject> Params, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Params->SetArrayField(FieldName, JsonValues);
	}

	TArray<FString> GetRowNamesFromExport(const TArray<TSharedPtr<FJsonValue>>& Rows)
	{
		TArray<FString> RowNames;
		for (const TSharedPtr<FJsonValue>& RowValue : Rows)
		{
			const TSharedPtr<FJsonObject>* RowObject = nullptr;
			if (RowValue.IsValid() && RowValue->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
			{
				RowNames.Add((*RowObject)->GetStringField(TEXT("row_name")));
			}
		}

		return RowNames;
	}

	TSharedPtr<FJsonObject> GetRowDataFromExportRow(const TSharedPtr<FJsonObject>& RowObject)
	{
		if (!RowObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* RowDataObject = nullptr;
		if (!RowObject->TryGetObjectField(TEXT("row_data"), RowDataObject) || RowDataObject == nullptr || !RowDataObject->IsValid())
		{
			return nullptr;
		}

		return *RowDataObject;
	}

	FString GetExportedTextValue(const TSharedPtr<FJsonObject>& RowObject, const FString& FieldName)
	{
		if (!RowObject.IsValid())
		{
			return TEXT("");
		}

		const TSharedPtr<FJsonObject>* TextObject = nullptr;
		if (!RowObject->TryGetObjectField(FieldName, TextObject) || TextObject == nullptr || !TextObject->IsValid())
		{
			return TEXT("");
		}

		FString Value;
		(*TextObject)->TryGetStringField(TEXT("value"), Value);
		return Value;
	}

	TArray<TSharedPtr<FJsonObject>> GetDataAssetEntries(const TSharedPtr<FJsonObject>& FileJson)
	{
		TArray<TSharedPtr<FJsonObject>> Entries;
		if (!FileJson.IsValid())
		{
			return Entries;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataAssets = nullptr;
		if (!FileJson->TryGetArrayField(TEXT("data_assets"), DataAssets) || DataAssets == nullptr)
		{
			return Entries;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *DataAssets)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (EntryValue.IsValid() && EntryValue->TryGetObject(EntryObject) && EntryObject != nullptr && EntryObject->IsValid())
			{
				Entries.Add(*EntryObject);
			}
		}

		return Entries;
	}

	FString MakeMissingDataAssetObjectPath(const UDataAsset* Asset)
	{
		const FString MissingAssetName = TEXT("DA_CortexExportMissing");
		const FString MissingPackageName = FPaths::Combine(FPaths::GetPath(Asset->GetOutermost()->GetName()), MissingAssetName);
		return FString::Printf(TEXT("%s.%s"), *MissingPackageName, *MissingAssetName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportFixtureSmokeTest,
	"Cortex.Data.Export.FixtureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportFixtureSmokeTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;

	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable != nullptr)
	{
		TestEqual(TEXT("regular fixture has deterministic row count"), RegularTable->GetRowMap().Num(), 3);
	}

	UCompositeDataTable* CompositeTable = Fixture.CreateCompositeDataTable();
	TestNotNull(TEXT("CompositeDataTable fixture is created"), CompositeTable);
	if (CompositeTable != nullptr)
	{
		const uint8* SharedRowData = CompositeTable->FindRowUnchecked(TEXT("shared"));
		TestNotNull(TEXT("composite fixture has overridden shared row"), SharedRowData);
		if (SharedRowData != nullptr)
		{
			const FCortexDataLocalizationTestRow* SharedRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(SharedRowData);
			TestEqual(TEXT("later composite parent overrides earlier parent"), SharedRow->Title.ToString(), TEXT("Override Shared"));
		}
	}

	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable != nullptr)
	{
		FString SourceString;
		const bool bFoundAlpha = StringTable->GetStringTable()->GetSourceString(TEXT("alpha.key"), SourceString);
		TestTrue(TEXT("StringTable fixture contains out-of-order alpha key"), bFoundAlpha);
		TestEqual(TEXT("StringTable alpha key has deterministic value"), SourceString, TEXT("Alpha text"));
	}

	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset != nullptr)
	{
		TestEqual(TEXT("DataAsset editable string is populated"), DataAsset->TestProperty, TEXT("Editable export value"));
		TestEqual(TEXT("DataAsset editable number is populated"), DataAsset->TestNumber, 42);
		TestEqual(TEXT("DataAsset transient blocked field is populated"), DataAsset->ExportTransientProperty, TEXT("Transient value"));
		TestEqual(TEXT("DataAsset non-editable transient blocked field is populated"), DataAsset->TransientExportBlocked, TEXT("Non-editable transient value"));
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("DataAsset editor-only blocked field is populated"), DataAsset->ExportEditorOnlyProperty, TEXT("Editor-only value"));
#endif
		TestEqual(TEXT("DataAsset internal blocked field is populated"), DataAsset->ExportInternalProperty, TEXT("Internal value"));
		TestEqual(TEXT("DataAsset editable export field is populated"), DataAsset->EditableExportAllowed, TEXT("Editable export allowed value"));
	}

	const FString ProbePath = Fixture.MakeSavedOutputPath(TEXT("probe.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProbePath), true);
	TestTrue(TEXT("probe JSON file writes to Saved/CortexExportTests"),
		FFileHelper::SaveStringToFile(TEXT("{\"ok\":true}"), *ProbePath));

	TSharedPtr<FJsonObject> ParsedProbe;
	FString ParseError;
	TestTrue(TEXT("fixture helper parses written JSON from disk"), Fixture.TryReadJsonFile(ProbePath, ParsedProbe, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}
	if (ParsedProbe.IsValid())
	{
		TestTrue(TEXT("parsed probe JSON contains expected bool"), ParsedProbe->GetBoolField(TEXT("ok")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportCommandsRegisteredTest,
	"Cortex.Data.Export.CommandsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportCommandsRegisteredTest::RunTest(const FString& Parameters)
{
	FCortexDataCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	TestTrue(TEXT("export_datatable_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_datatable_json")));
	TestTrue(TEXT("export_string_table_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_string_table_json")));
	TestTrue(TEXT("export_data_assets_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_data_assets_json")));
	TestTrue(TEXT("export_bulk_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_bulk_json")));

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
	Params->SetStringField(TEXT("out_path"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexExportTests"), TEXT("registered.json")));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestFalse(TEXT("export_datatable_json is registered and validates the missing table"), Result.bSuccess);
	TestEqual(TEXT("registered command returns domain error, not UnknownCommand"), Result.ErrorCode, CortexErrorCodes::TableNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportPathSafetyTest,
	"Cortex.Data.Export.PathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportPathSafetyTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	auto ExecuteDatatableExport = [&Router](const FString& OutPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
		Params->SetStringField(TEXT("out_path"), OutPath);
		return Router.Execute(TEXT("data.export_datatable_json"), Params);
	};

	const FString TraversalBase = Fixture.MakeSavedOutputDir(TEXT("TraversalBase"));
	const FString TraversalPath = FPaths::Combine(TraversalBase, TEXT(".."), TEXT("TraversalEscape"), TEXT("out.json"));
	const FString UnexpectedTraversalDirectory = FPaths::Combine(FPaths::GetPath(TraversalBase), TEXT("TraversalEscape"));
	const FCortexCommandResult TraversalResult = ExecuteDatatableExport(TraversalPath);
	TestFalse(TEXT("Traversal output paths are rejected"), TraversalResult.bSuccess);
	TestEqual(TEXT("Traversal rejection uses InvalidField"), TraversalResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestFalse(TEXT("Traversal rejection creates no directory"), IFileManager::Get().DirectoryExists(*UnexpectedTraversalDirectory));

	FString ProjectRootForSibling = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectRootForSibling);
	const FString SiblingEscapePath = ProjectRootForSibling + TEXT("_Evil/out.json");
	const FCortexCommandResult SiblingResult = ExecuteDatatableExport(SiblingEscapePath);
	TestFalse(TEXT("Sibling-prefix output paths are rejected"), SiblingResult.bSuccess);
	TestEqual(TEXT("Sibling-prefix rejection uses InvalidField"), SiblingResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DevicePathResult = ExecuteDatatableExport(TEXT("\\\\?\\C:\\CortexExportTests\\out.json"));
	TestFalse(TEXT("Win32 device output paths are rejected"), DevicePathResult.bSuccess);
	TestEqual(TEXT("Win32 device rejection uses InvalidField"), DevicePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DotDevicePathResult = ExecuteDatatableExport(TEXT("\\\\.\\C:\\CortexExportTests\\out.json"));
	TestFalse(TEXT("Win32 dot-device output paths are rejected"), DotDevicePathResult.bSuccess);
	TestEqual(TEXT("Win32 dot-device rejection uses InvalidField"), DotDevicePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult UncPathResult = ExecuteDatatableExport(TEXT("\\\\server\\share\\out.json"));
	TestFalse(TEXT("UNC output paths are rejected"), UncPathResult.bSuccess);
	TestEqual(TEXT("UNC path rejection uses InvalidField"), UncPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DriveRelativePathResult = ExecuteDatatableExport(TEXT("C:relative\\out.json"));
	TestFalse(TEXT("Drive-relative output paths are rejected"), DriveRelativePathResult.bSuccess);
	TestEqual(TEXT("Drive-relative path rejection uses InvalidField"), DriveRelativePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString ExistingDirectoryTarget = Fixture.MakeSavedOutputDir(TEXT("ExistingDirectoryTarget"));
	IFileManager::Get().MakeDirectory(*ExistingDirectoryTarget, true);
	const FCortexCommandResult ExistingDirectoryResult = ExecuteDatatableExport(ExistingDirectoryTarget);
	TestFalse(TEXT("Existing directory output targets are rejected"), ExistingDirectoryResult.bSuccess);
	TestEqual(TEXT("Existing directory rejection uses InvalidField"), ExistingDirectoryResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString ExternalTargetDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CortexExportSymlinkTarget"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString LinkDir = Fixture.MakeSavedOutputDir(TEXT("SymlinkParent"));
	IFileManager::Get().MakeDirectory(*ExternalTargetDir, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*ExternalTargetDir, false, true);
	};

#if PLATFORM_WINDOWS
	FString StdOut;
	FString StdErr;
	int32 MklinkExitCode = INDEX_NONE;
	const FString MklinkArgs = FString::Printf(TEXT("/C mklink /J \"%s\" \"%s\""), *LinkDir, *ExternalTargetDir);
	const bool bMklinkStarted = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *MklinkArgs, &MklinkExitCode, &StdOut, &StdErr);
	if (!bMklinkStarted || MklinkExitCode != 0 || !IFileManager::Get().DirectoryExists(*LinkDir))
	{
		AddInfo(FString::Printf(TEXT("Skipping symlink/junction escape test; mklink /J failed with code %d: %s %s"), MklinkExitCode, *StdOut, *StdErr));
		return true;
	}
#else
	AddInfo(TEXT("Skipping symlink/junction escape test on this platform"));
	return true;
#endif

	const FCortexCommandResult SymlinkResult = ExecuteDatatableExport(FPaths::Combine(LinkDir, TEXT("NewDir"), TEXT("out.json")));
	TestFalse(TEXT("Symlink/junction parent escapes are rejected"), SymlinkResult.bSuccess);
	TestEqual(TEXT("Symlink/junction rejection uses InvalidField"), SymlinkResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableProjectionSchemaTest,
	"Cortex.Data.Export.Datatable.ProjectionSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableProjectionSchemaTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("projection-schema.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	Params->SetStringField(TEXT("row_name_pattern"), TEXT("*a"));
	Params->SetBoolField(TEXT("include_schema"), true);
	AddStringArrayField(Params, TEXT("fields"), TArray<FString>{ TEXT("Title") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("DataTable export succeeds"), Result.bSuccess);
	TestTrue(TEXT("DataTable export returns data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw rows"), !Result.Data->HasField(TEXT("rows")));
		TestTrue(TEXT("summary remains compact"), !Result.Data->HasField(TEXT("schema")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestEqual(TEXT("summary reports exported count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("exported_count"))), 2);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("DataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains stable row array"), FileJson->HasTypedField<EJson::Array>(TEXT("rows")));
		TestTrue(TEXT("file contains row_struct"), FileJson->HasTypedField<EJson::String>(TEXT("row_struct")));
		TestTrue(TEXT("file contains schema only when requested"), FileJson->HasTypedField<EJson::Object>(TEXT("schema")));
		TestTrue(TEXT("file contains projected fields array"), FileJson->HasTypedField<EJson::Array>(TEXT("fields")));
		TestEqual(TEXT("total filtered count is reported"), static_cast<int32>(FileJson->GetNumberField(TEXT("total_count"))), 2);
		TestEqual(TEXT("exported count is reported"), static_cast<int32>(FileJson->GetNumberField(TEXT("exported_count"))), 2);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("row_name_pattern filters matching rows"), Rows->Num(), 2);
			TestTrue(TEXT("filtered rows are sorted by row_name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("zeta") });

			for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
			{
				const TSharedPtr<FJsonObject>* RowObject = nullptr;
				if (RowValue.IsValid() && RowValue->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
				{
					TSharedPtr<FJsonObject> RowData = GetRowDataFromExportRow(*RowObject);
					TestTrue(TEXT("row wrapper includes row_name metadata"), (*RowObject)->HasTypedField<EJson::String>(TEXT("row_name")));
					TestTrue(TEXT("row wrapper includes row_data object"), RowData.IsValid());
					if (RowData.IsValid())
					{
						TestTrue(TEXT("projected row_data includes requested Title"), RowData->HasField(TEXT("Title")));
						TestFalse(TEXT("projected row_data omits unrequested Steps"), RowData->HasField(TEXT("Steps")));
						TestFalse(TEXT("projected row_data omits unrequested row_name field"), RowData->HasField(TEXT("row_name")));
					}
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableRowWrapperTest,
	"Cortex.Data.Export.Datatable.RowWrapperAvoidsNameCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableRowWrapperTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("row-wrapper.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("alpha") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("DataTable export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("DataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("exact row filter exports one row"), Rows->Num(), 1);
			if (Rows->Num() == 1)
			{
				const TSharedPtr<FJsonObject>* RowObject = nullptr;
				if ((*Rows)[0].IsValid() && (*Rows)[0]->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
				{
					TestEqual(TEXT("wrapper row_name is export metadata"), (*RowObject)->GetStringField(TEXT("row_name")), TEXT("alpha"));
					TSharedPtr<FJsonObject> RowData = GetRowDataFromExportRow(*RowObject);
					TestTrue(TEXT("wrapper preserves serialized row_data"), RowData.IsValid());
					if (RowData.IsValid())
					{
						TestEqual(TEXT("row_data preserves real row_name field"), RowData->GetStringField(TEXT("row_name")), TEXT("Data Alpha"));
						TestEqual(TEXT("row_data preserves other fields"), GetExportedTextValue(RowData, TEXT("Title")), TEXT("Alpha"));
					}
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableMissingExplicitRowTest,
	"Cortex.Data.Export.Datatable.MissingExplicitRowFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableMissingExplicitRowTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("missing-explicit-row.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("alpha"), TEXT("missing") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestFalse(TEXT("missing explicit row_names fail export"), Result.bSuccess);
	TestEqual(TEXT("missing explicit row_names use RowNotFound"), Result.ErrorCode, CortexErrorCodes::RowNotFound);
	TestFalse(TEXT("missing explicit row_names do not write partial file"), IFileManager::Get().FileExists(*OutPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableMixedCaseExplicitRowTest,
	"Cortex.Data.Export.Datatable.MixedCaseExplicitRowResolvesActualName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableMixedCaseExplicitRowTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("mixed-case-explicit-row.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("ALPHA") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("mixed-case explicit row_names export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("mixed-case explicit row export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("mixed-case explicit row exports one resolved row"), Rows->Num(), 1);
			TestTrue(TEXT("mixed-case explicit row uses actual stored row name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha") });
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportCompositeDatatableTest,
	"Cortex.Data.Export.Datatable.CompositeResolvedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportCompositeDatatableTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UCompositeDataTable* CompositeTable = Fixture.CreateCompositeDataTable();
	TestNotNull(TEXT("CompositeDataTable fixture is created"), CompositeTable);
	if (CompositeTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("composite.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), CompositeTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("CompositeDataTable export succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw rows"), !Result.Data->HasField(TEXT("rows")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("CompositeDataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	FString FileContents;
	TestTrue(TEXT("CompositeDataTable export file can be read as text"), FFileHelper::LoadFileToString(FileContents, *OutPath));
	TestFalse(TEXT("file does not include parent A table path"), FileContents.Contains(TEXT("DT_CortexExportParentA")));
	TestFalse(TEXT("file does not include parent B table path"), FileContents.Contains(TEXT("DT_CortexExportParentB")));
	TestFalse(TEXT("file does not include parent-table sections"), FileContents.Contains(TEXT("parent_tables")));
	TestFalse(TEXT("file does not include source-table sections"), FileContents.Contains(TEXT("source_tables")));
	TestFalse(TEXT("overridden parent row payload is not exported"), FileContents.Contains(TEXT("Base Shared")));

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains stable row array"), FileJson->HasTypedField<EJson::Array>(TEXT("rows")));
		TestTrue(TEXT("file contains row_struct"), FileJson->HasTypedField<EJson::String>(TEXT("row_struct")));

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("composite export returns merged resolved rows"), Rows->Num(), 3);
			TestTrue(TEXT("composite rows are sorted by row_name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("beta"), TEXT("shared") });

			const TSharedPtr<FJsonObject>* SharedRowObject = nullptr;
			if ((*Rows)[2].IsValid() && (*Rows)[2]->TryGetObject(SharedRowObject) && SharedRowObject != nullptr && SharedRowObject->IsValid())
			{
				TSharedPtr<FJsonObject> SharedRowData = GetRowDataFromExportRow(*SharedRowObject);
				TestTrue(TEXT("composite row wrapper includes row_data"), SharedRowData.IsValid());
				if (SharedRowData.IsValid())
				{
					TestEqual(TEXT("later parent override wins for duplicate row names"), GetExportedTextValue(SharedRowData, TEXT("Title")), TEXT("Override Shared"));
					TestEqual(TEXT("row_data includes overridden real row_name field"), SharedRowData->GetStringField(TEXT("row_name")), TEXT("Data Override Shared"));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableDeterministicBytesTest,
	"Cortex.Data.Export.Datatable.DeterministicBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableDeterministicBytesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString FirstOutPath = Fixture.MakeSavedOutputPath(TEXT("deterministic-a.json"));
	const FString SecondOutPath = Fixture.MakeSavedOutputPath(TEXT("deterministic-b.json"));

	auto ExecuteDatatableExport = [&Router, RegularTable](const FString& OutPath)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
		Params->SetStringField(TEXT("out_path"), OutPath);
		Params->SetBoolField(TEXT("include_schema"), true);
		return Router.Execute(TEXT("data.export_datatable_json"), Params);
	};

	const FCortexCommandResult FirstResult = ExecuteDatatableExport(FirstOutPath);
	TestTrue(TEXT("first DataTable export succeeds"), FirstResult.bSuccess);
	const FCortexCommandResult SecondResult = ExecuteDatatableExport(SecondOutPath);
	TestTrue(TEXT("second DataTable export succeeds"), SecondResult.bSuccess);

	TArray<uint8> FirstBytes;
	TArray<uint8> SecondBytes;
	FString FirstReadError;
	FString SecondReadError;
	TestTrue(TEXT("first export file reads as bytes"), Fixture.TryReadFileBytes(FirstOutPath, FirstBytes, FirstReadError));
	TestTrue(TEXT("second export file reads as bytes"), Fixture.TryReadFileBytes(SecondOutPath, SecondBytes, SecondReadError));
	if (!FirstReadError.IsEmpty())
	{
		AddError(FirstReadError);
	}
	if (!SecondReadError.IsEmpty())
	{
		AddError(SecondReadError);
	}

	TestTrue(TEXT("first export bytes are non-empty"), FirstBytes.Num() > 0);
	TestTrue(TEXT("second export bytes are non-empty"), SecondBytes.Num() > 0);
	TestTrue(TEXT("DataTable exports are byte-for-byte stable"), FirstBytes == SecondBytes);

	FString FirstContents;
	TestTrue(TEXT("first export file reads as text"), FFileHelper::LoadFileToString(FirstContents, *FirstOutPath));
	TestFalse(TEXT("first export text is non-empty"), FirstContents.IsEmpty());
	const int32 ExportedCountIndex = FirstContents.Find(TEXT("\"exported_count\""));
	const int32 FieldsIndex = FirstContents.Find(TEXT("\"fields\""));
	const int32 RowStructIndex = FirstContents.Find(TEXT("\"row_struct\""));
	const int32 RowsIndex = FirstContents.Find(TEXT("\"rows\""));
	const int32 SchemaIndex = FirstContents.Find(TEXT("\"schema\""));
	const int32 TablePathIndex = FirstContents.Find(TEXT("\"table_path\""));
	const int32 TotalCountIndex = FirstContents.Find(TEXT("\"total_count\""));
	TestTrue(TEXT("canonical writer sorts top-level export keys"),
		ExportedCountIndex != INDEX_NONE
		&& ExportedCountIndex < FieldsIndex
		&& FieldsIndex < RowStructIndex
		&& RowStructIndex < RowsIndex
		&& RowsIndex < SchemaIndex
		&& SchemaIndex < TablePathIndex
		&& TablePathIndex < TotalCountIndex);
	const int32 RowDataIndex = RowsIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_data\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, RowsIndex)
		: INDEX_NONE;
	const int32 NestedStepsIndex = RowDataIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"Steps\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, RowDataIndex)
		: INDEX_NONE;
	const int32 NestedTitleIndex = NestedStepsIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"Title\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedStepsIndex)
		: INDEX_NONE;
	const int32 NestedRowNameIndex = NestedTitleIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedTitleIndex)
		: INDEX_NONE;
	const int32 WrapperRowNameIndex = NestedRowNameIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedRowNameIndex + 1)
		: INDEX_NONE;
	TestTrue(TEXT("canonical writer sorts nested row wrapper keys"),
		RowDataIndex != INDEX_NONE
		&& NestedStepsIndex != INDEX_NONE
		&& NestedTitleIndex != INDEX_NONE
		&& NestedRowNameIndex != INDEX_NONE
		&& WrapperRowNameIndex != INDEX_NONE
		&& RowDataIndex < NestedStepsIndex
		&& NestedStepsIndex < NestedTitleIndex
		&& NestedTitleIndex < NestedRowNameIndex
		&& NestedRowNameIndex < WrapperRowNameIndex);

	TSharedPtr<FJsonObject> FirstJson;
	FString ParseError;
	TestTrue(TEXT("first deterministic export parses"), Fixture.TryReadJsonFile(FirstOutPath, FirstJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}
	if (FirstJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FirstJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("deterministic export writes expected row count"), Rows->Num(), 3);
			TestTrue(TEXT("export preserves sorted rows array order"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("middle"), TEXT("zeta") });
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDataAssetsCatalogAndPropertiesTest,
	"Cortex.Data.Export.DataAssets.CatalogAndEditableProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDataAssetsCatalogAndPropertiesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataExportTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString CatalogOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-catalog.json"));

	TSharedRef<FJsonObject> CatalogParams = MakeShared<FJsonObject>();
	CatalogParams->SetStringField(TEXT("out_path"), CatalogOutPath);
	CatalogParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	CatalogParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	CatalogParams->SetBoolField(TEXT("include_properties"), false);

	const FCortexCommandResult CatalogResult = Router.Execute(TEXT("data.export_data_assets_json"), CatalogParams);
	TestTrue(TEXT("DataAsset catalog export succeeds"), CatalogResult.bSuccess);
	TestTrue(TEXT("DataAsset catalog export returns data"), CatalogResult.Data.IsValid());
	if (CatalogResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !CatalogResult.Data->HasField(TEXT("data_assets")));
		TestEqual(TEXT("exported count is one"), static_cast<int32>(CatalogResult.Data->GetNumberField(TEXT("exported_count"))), 1);
	}

	TSharedPtr<FJsonObject> CatalogFileJson;
	FString CatalogParseError;
	TestTrue(TEXT("DataAsset catalog export file parses"), Fixture.TryReadJsonFile(CatalogOutPath, CatalogFileJson, CatalogParseError));
	if (!CatalogParseError.IsEmpty())
	{
		AddError(CatalogParseError);
	}
	if (CatalogFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), CatalogFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("file exported count is one"), static_cast<int32>(CatalogFileJson->GetNumberField(TEXT("exported_count"))), 1);

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(CatalogFileJson);
		TestEqual(TEXT("catalog export writes one entry"), Entries.Num(), 1);
		if (Entries.Num() == 1)
		{
			TestEqual(TEXT("catalog entry path matches fixture"), Entries[0]->GetStringField(TEXT("path")), DataAsset->GetPathName());
			TestEqual(TEXT("catalog entry name matches fixture"), Entries[0]->GetStringField(TEXT("name")), DataAsset->GetName());
			TestEqual(TEXT("catalog entry class matches fixture"), Entries[0]->GetStringField(TEXT("asset_class")), DataAsset->GetClass()->GetName());
			TestFalse(TEXT("catalog entry omits properties when include_properties is false"), Entries[0]->HasField(TEXT("properties")));
		}
	}

	const FString PropertiesOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-properties.json"));
	TSharedRef<FJsonObject> PropertiesParams = MakeShared<FJsonObject>();
	PropertiesParams->SetStringField(TEXT("out_path"), PropertiesOutPath);
	PropertiesParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	PropertiesParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	PropertiesParams->SetBoolField(TEXT("include_properties"), true);

	const FCortexCommandResult PropertiesResult = Router.Execute(TEXT("data.export_data_assets_json"), PropertiesParams);
	TestTrue(TEXT("DataAsset properties export succeeds"), PropertiesResult.bSuccess);
	TestTrue(TEXT("DataAsset properties export returns data"), PropertiesResult.Data.IsValid());
	if (PropertiesResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !PropertiesResult.Data->HasField(TEXT("data_assets")));
		TestEqual(TEXT("exported count is one"), static_cast<int32>(PropertiesResult.Data->GetNumberField(TEXT("exported_count"))), 1);
	}

	TSharedPtr<FJsonObject> PropertiesFileJson;
	FString PropertiesParseError;
	TestTrue(TEXT("DataAsset properties export file parses"), Fixture.TryReadJsonFile(PropertiesOutPath, PropertiesFileJson, PropertiesParseError));
	if (!PropertiesParseError.IsEmpty())
	{
		AddError(PropertiesParseError);
	}
	if (PropertiesFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), PropertiesFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("file exported count is one"), static_cast<int32>(PropertiesFileJson->GetNumberField(TEXT("exported_count"))), 1);

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(PropertiesFileJson);
		TestEqual(TEXT("properties export writes one entry"), Entries.Num(), 1);
		if (Entries.Num() == 1)
		{
			const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
			TestTrue(TEXT("properties entry includes properties object"),
				Entries[0]->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject != nullptr && PropertiesObject->IsValid());
			if (PropertiesObject != nullptr && PropertiesObject->IsValid())
			{
				TestEqual(TEXT("editable string property is exported"), (*PropertiesObject)->GetStringField(TEXT("TestProperty")), TEXT("Editable export value"));
				TestEqual(TEXT("editable number property is exported"), static_cast<int32>((*PropertiesObject)->GetNumberField(TEXT("TestNumber"))), 42);
				TestEqual(TEXT("editable allowed property is exported"), (*PropertiesObject)->GetStringField(TEXT("EditableExportAllowed")), TEXT("Editable export allowed value"));
				TestFalse(TEXT("transient editable property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportTransientProperty")));
				TestFalse(TEXT("transient non-editable property is blocked"), (*PropertiesObject)->HasField(TEXT("TransientExportBlocked")));
#if WITH_EDITORONLY_DATA
				TestFalse(TEXT("editor-only property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportEditorOnlyProperty")));
#endif
				TestFalse(TEXT("internal non-editable property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportInternalProperty")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDataAssetsExplicitPathsTest,
	"Cortex.Data.Export.DataAssets.ExplicitPathsPartial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDataAssetsExplicitPathsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataExportTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	UCortexTestDataAsset* EarlierSortDataAsset = Fixture.CreateDataAsset(TEXT("DA_CortexExportAlpha"));
	TestNotNull(TEXT("second DataAsset fixture is created"), EarlierSortDataAsset);
	if (DataAsset == nullptr || EarlierSortDataAsset == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString AssetPath = DataAsset->GetPathName();
	const FString EarlierSortAssetPath = EarlierSortDataAsset->GetPathName();
	const FString MissingAssetPath = MakeMissingDataAssetObjectPath(DataAsset);

	TSharedRef<FJsonObject> NonStringPathParams = MakeShared<FJsonObject>();
	NonStringPathParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-non-string-path.json")));
	NonStringPathParams->SetArrayField(TEXT("asset_paths"), TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(123.0) });
	const FCortexCommandResult NonStringPathResult = Router.Execute(TEXT("data.export_data_assets_json"), NonStringPathParams);
	TestFalse(TEXT("non-string explicit asset path is rejected"), NonStringPathResult.bSuccess);
	TestEqual(TEXT("non-string explicit asset path uses InvalidField"), NonStringPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> EmptyArrayParams = MakeShared<FJsonObject>();
	EmptyArrayParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-empty-array.json")));
	EmptyArrayParams->SetArrayField(TEXT("asset_paths"), TArray<TSharedPtr<FJsonValue>>());
	const FCortexCommandResult EmptyArrayResult = Router.Execute(TEXT("data.export_data_assets_json"), EmptyArrayParams);
	TestFalse(TEXT("empty explicit asset_paths array is rejected"), EmptyArrayResult.bSuccess);
	TestEqual(TEXT("empty explicit asset_paths array uses InvalidField"), EmptyArrayResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> InvalidPathParams = MakeShared<FJsonObject>();
	InvalidPathParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-invalid-path.json")));
	AddStringArrayField(InvalidPathParams, TEXT("asset_paths"), TArray<FString>{ TEXT("") });
	const FCortexCommandResult InvalidPathResult = Router.Execute(TEXT("data.export_data_assets_json"), InvalidPathParams);
	TestFalse(TEXT("empty explicit asset path is rejected"), InvalidPathResult.bSuccess);
	TestEqual(TEXT("empty explicit asset path uses InvalidField"), InvalidPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> ClassMismatchParams = MakeShared<FJsonObject>();
	ClassMismatchParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-class-mismatch.json")));
	ClassMismatchParams->SetStringField(TEXT("class_name"), TEXT("CortexDerivedTestDataAsset"));
	ClassMismatchParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(ClassMismatchParams, TEXT("asset_paths"), TArray<FString>{ AssetPath });
	const FCortexCommandResult ClassMismatchResult = Router.Execute(TEXT("data.export_data_assets_json"), ClassMismatchParams);
	TestFalse(TEXT("explicit DataAsset class mismatch fails"), ClassMismatchResult.bSuccess);
	TestEqual(TEXT("explicit DataAsset class mismatch uses InvalidField"), ClassMismatchResult.ErrorCode, CortexErrorCodes::InvalidField);

	UCortexTestDataAsset* SiblingPrefixDataAsset = Fixture.CreateDataAsset(TEXT("AB_CortexExportSibling"));
	TestNotNull(TEXT("sibling-prefix DataAsset fixture is created"), SiblingPrefixDataAsset);
	if (SiblingPrefixDataAsset == nullptr)
	{
		return true;
	}

	const FString RunPackagePath = FPaths::GetPath(AssetPath);
	const FString SiblingPrefixFilter = FPaths::Combine(RunPackagePath, TEXT("A"));
	TSharedRef<FJsonObject> SiblingPrefixParams = MakeShared<FJsonObject>();
	SiblingPrefixParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-sibling-prefix.json")));
	SiblingPrefixParams->SetStringField(TEXT("path_filter"), SiblingPrefixFilter);
	SiblingPrefixParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(SiblingPrefixParams, TEXT("asset_paths"), TArray<FString>{ SiblingPrefixDataAsset->GetPathName() });
	const FCortexCommandResult SiblingPrefixResult = Router.Execute(TEXT("data.export_data_assets_json"), SiblingPrefixParams);
	TestFalse(TEXT("explicit DataAsset path_filter rejects sibling prefix"), SiblingPrefixResult.bSuccess);
	TestEqual(TEXT("explicit DataAsset path_filter sibling prefix uses InvalidField"), SiblingPrefixResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString FailOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-missing-fails.json"));
	TSharedRef<FJsonObject> FailParams = MakeShared<FJsonObject>();
	FailParams->SetStringField(TEXT("out_path"), FailOutPath);
	FailParams->SetStringField(TEXT("class_filter"), TEXT("CortexTestDataAsset"));
	FailParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(AssetPath));
	FailParams->SetBoolField(TEXT("include_properties"), true);
	FailParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(FailParams, TEXT("asset_paths"), TArray<FString>{ AssetPath, AssetPath, MissingAssetPath, EarlierSortAssetPath });

	FCortexExportSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);
	const FCortexCommandResult FailResult = Router.Execute(TEXT("data.export_data_assets_json"), FailParams);
	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("missing explicit DataAsset fails when allow_partial is false"), FailResult.bSuccess);
	TestEqual(TEXT("missing explicit DataAsset uses AssetNotFound"), FailResult.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("missing explicit DataAsset emits no SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	TestFalse(TEXT("missing explicit DataAsset does not write partial file"), IFileManager::Get().FileExists(*FailOutPath));

	const FString PartialOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-partial.json"));
	TSharedRef<FJsonObject> PartialParams = MakeShared<FJsonObject>();
	PartialParams->SetStringField(TEXT("out_path"), PartialOutPath);
	PartialParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	PartialParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(AssetPath));
	PartialParams->SetBoolField(TEXT("include_properties"), true);
	PartialParams->SetBoolField(TEXT("allow_partial"), true);
	AddStringArrayField(PartialParams, TEXT("asset_paths"), TArray<FString>{ MissingAssetPath, AssetPath, AssetPath, EarlierSortAssetPath });

	const FCortexCommandResult PartialResult = Router.Execute(TEXT("data.export_data_assets_json"), PartialParams);
	TestTrue(TEXT("explicit DataAsset export succeeds partially when allow_partial is true"), PartialResult.bSuccess);
	TestTrue(TEXT("partial explicit export returns data"), PartialResult.Data.IsValid());
	if (PartialResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !PartialResult.Data->HasField(TEXT("data_assets")));
		TestTrue(TEXT("summary marks partial export"), PartialResult.Data->GetBoolField(TEXT("partial")));
		TestEqual(TEXT("exported count excludes duplicate and missing paths"), static_cast<int32>(PartialResult.Data->GetNumberField(TEXT("exported_count"))), 2);

		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		TestTrue(TEXT("partial export reports missing asset error"),
			PartialResult.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors != nullptr && Errors->Num() == 1);
	}

	TSharedPtr<FJsonObject> PartialFileJson;
	FString PartialParseError;
	TestTrue(TEXT("partial DataAsset export file parses"), Fixture.TryReadJsonFile(PartialOutPath, PartialFileJson, PartialParseError));
	if (!PartialParseError.IsEmpty())
	{
		AddError(PartialParseError);
	}
	if (PartialFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), PartialFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("file exported count excludes duplicate and missing paths"), static_cast<int32>(PartialFileJson->GetNumberField(TEXT("exported_count"))), 2);

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(PartialFileJson);
		TestEqual(TEXT("duplicate explicit DataAsset paths are deduplicated"), Entries.Num(), 2);
		if (Entries.Num() == 2)
		{
			TestEqual(TEXT("explicit DataAsset paths are sorted"), Entries[0]->GetStringField(TEXT("path")), EarlierSortAssetPath);
			TestEqual(TEXT("second sorted explicit DataAsset path is written"), Entries[1]->GetStringField(TEXT("path")), AssetPath);
			TestTrue(TEXT("first valid explicit DataAsset output includes properties"), Entries[0]->HasTypedField<EJson::Object>(TEXT("properties")));
			TestTrue(TEXT("second valid explicit DataAsset output includes properties"), Entries[1]->HasTypedField<EJson::Object>(TEXT("properties")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportStringTableJsonTest,
	"Cortex.Data.Export.StringTable.FilteredEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportStringTableJsonTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("string-table.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("string_table_path"), StringTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	Params->SetStringField(TEXT("key_pattern"), TEXT("*.key"));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_string_table_json"), Params);
	TestTrue(TEXT("StringTable export succeeds"), Result.bSuccess);
	TestTrue(TEXT("StringTable export returns data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw entries"), !Result.Data->HasField(TEXT("entries")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestEqual(TEXT("summary reports exported entry count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("exported_count"))), 3);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("StringTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	FString FileContents;
	TestTrue(TEXT("StringTable export file can be read as text"), FFileHelper::LoadFileToString(FileContents, *OutPath));
	TestFalse(TEXT("key_pattern excludes non-matching keys"), FileContents.Contains(TEXT("ignored.other")));
	TestFalse(TEXT("key_pattern excludes non-matching source strings"), FileContents.Contains(TEXT("Ignored text")));

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains entries array"), FileJson->HasTypedField<EJson::Array>(TEXT("entries")));
		TestTrue(TEXT("file contains count"), FileJson->HasTypedField<EJson::Number>(TEXT("count")));
		TestEqual(TEXT("file reports filtered entry count"), static_cast<int32>(FileJson->GetNumberField(TEXT("count"))), 3);

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (FileJson->TryGetArrayField(TEXT("entries"), Entries) && Entries != nullptr)
		{
			TestEqual(TEXT("string entries are sorted by key"), Entries->Num(), 3);
			if (Entries->Num() == 3)
			{
				const TSharedPtr<FJsonObject>* FirstEntry = nullptr;
				const TSharedPtr<FJsonObject>* SecondEntry = nullptr;
				const TSharedPtr<FJsonObject>* ThirdEntry = nullptr;
				if ((*Entries)[0].IsValid() && (*Entries)[0]->TryGetObject(FirstEntry) && FirstEntry != nullptr && FirstEntry->IsValid()
					&& (*Entries)[1].IsValid() && (*Entries)[1]->TryGetObject(SecondEntry) && SecondEntry != nullptr && SecondEntry->IsValid()
					&& (*Entries)[2].IsValid() && (*Entries)[2]->TryGetObject(ThirdEntry) && ThirdEntry != nullptr && ThirdEntry->IsValid())
				{
					TestEqual(TEXT("first key is alpha"), (*FirstEntry)->GetStringField(TEXT("key")), TEXT("alpha.key"));
					TestEqual(TEXT("second key is middle"), (*SecondEntry)->GetStringField(TEXT("key")), TEXT("middle.key"));
					TestEqual(TEXT("third key is zeta"), (*ThirdEntry)->GetStringField(TEXT("key")), TEXT("zeta.key"));
					TestEqual(TEXT("entry contains source string"), (*FirstEntry)->GetStringField(TEXT("source_string")), TEXT("Alpha text"));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkPathSafetyTest,
	"Cortex.Data.Export.BulkPathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkPathSafetyTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("type"), TEXT("datatable"));
	Item->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
	Item->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("absolute-item.json")));

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(Item));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestFalse(TEXT("Bulk item absolute output paths are rejected"), Result.bSuccess);
	TestEqual(TEXT("Bulk item absolute path rejection uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}
