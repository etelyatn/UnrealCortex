#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexSafeFileContract.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Operations/CortexDataImportQueueOps.h"
#include "Operations/CortexDataMutationHelpers.h"
#include "Tests/CortexDataLocalizationTestTypes.h"
#include "Tests/CortexTestDataAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectHash.h"

namespace
{
	const TCHAR* ImportQueueTestRoot = TEXT("/Game/CortexImportQueueTests");

	FCortexCommandRouter CreateDataImportQueueTestRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	bool ImportQueueSupportedCommandNamesContain(const TArray<FCortexCommandInfo>& Commands, const FString& CommandName)
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

	TArray<FString> GetImportQueueStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		TArray<FString> Values;
		if (!Object.IsValid())
		{
			return Values;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonValues = nullptr;
		if (!Object->TryGetArrayField(FieldName, JsonValues) || JsonValues == nullptr)
		{
			return Values;
		}

		for (const TSharedPtr<FJsonValue>& JsonValue : *JsonValues)
		{
			FString Value;
			if (JsonValue.IsValid() && JsonValue->TryGetString(Value))
			{
				Values.Add(Value);
			}
		}

		return Values;
	}

	void AssertCanonicalImportQueueSummary(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Summary,
		const FString& ExpectedReportPath,
		const TArray<FString>& ExpectedTargetsTouched)
	{
		Test.TestNotNull(TEXT("summary exists"), Summary.Get());
		if (!Summary.IsValid())
		{
			return;
		}

		Test.TestTrue(TEXT("summary has success"), Summary->HasTypedField<EJson::Boolean>(TEXT("success")));
		Test.TestTrue(TEXT("summary has partial"), Summary->HasTypedField<EJson::Boolean>(TEXT("partial")));
		Test.TestTrue(TEXT("summary has warnings"), Summary->HasTypedField<EJson::Array>(TEXT("warnings")));
		Test.TestTrue(TEXT("summary has errors"), Summary->HasTypedField<EJson::Array>(TEXT("errors")));
		Test.TestTrue(TEXT("summary has files_written"), Summary->HasTypedField<EJson::Array>(TEXT("files_written")));
		Test.TestTrue(TEXT("summary has targets_touched"), Summary->HasTypedField<EJson::Array>(TEXT("targets_touched")));
		Test.TestTrue(TEXT("summary has counts"), Summary->HasTypedField<EJson::Object>(TEXT("counts")));

		Test.TestFalse(TEXT("legacy operation_count is removed"), Summary->HasField(TEXT("operation_count")));
		Test.TestFalse(TEXT("legacy applied_count is removed"), Summary->HasField(TEXT("applied_count")));
		Test.TestFalse(TEXT("legacy failed_count is removed"), Summary->HasField(TEXT("failed_count")));
		Test.TestFalse(TEXT("legacy skipped_count is removed"), Summary->HasField(TEXT("skipped_count")));
		Test.TestFalse(TEXT("legacy save_failed_count is removed"), Summary->HasField(TEXT("save_failed_count")));

		Test.TestEqual(
			TEXT("files_written matches report path"),
			GetImportQueueStringArrayField(Summary, TEXT("files_written")),
			TArray<FString>{ ExpectedReportPath });
		Test.TestEqual(
			TEXT("targets_touched matches request scope"),
			GetImportQueueStringArrayField(Summary, TEXT("targets_touched")),
			ExpectedTargetsTouched);
	}

	FCortexDataLocalizationTestRow MakeImportQueueRow(const FString& Title, const FString& StepText)
	{
		FCortexDataLocalizationTestRow Row;
		Row.Title = FText::FromString(Title);
		Row.row_name = FString::Printf(TEXT("Data %s"), *Title);

		FCortexDataLocalizationStepTestRow Step;
		Step.Description = FText::FromString(StepText);
		Row.Steps.Add(Step);

		return Row;
	}

	FString ImportQueuePackagePathForAsset(const FString& RunId, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s/%s"), ImportQueueTestRoot, *RunId, *AssetName);
	}

	void DeleteImportQueuePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	void CleanupImportQueueLoadedPackage(const FString& PackageName)
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

	void CleanupImportQueuePackageByName(const FString& PackageName)
	{
		CleanupImportQueueLoadedPackage(PackageName);
		DeleteImportQueuePackageFile(PackageName);
	}

	class FCortexDataImportQueueTestFixture
	{
	public:
		FCortexDataImportQueueTestFixture()
			: RunId(FString::Printf(TEXT("Run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FCortexDataImportQueueTestFixture()
		{
			Cleanup();
		}

		UDataTable* CreateRegularDataTable()
		{
			UDataTable* Table = CreateAsset<UDataTable>(TEXT("DT_CortexImportQueueRows"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

			Table->AddRow(TEXT("zeta"), MakeImportQueueRow(TEXT("Zeta"), TEXT("Third inserted")));
			Table->AddRow(TEXT("alpha"), MakeImportQueueRow(TEXT("Alpha"), TEXT("Second inserted")));
			Table->AddRow(TEXT("middle"), MakeImportQueueRow(TEXT("Middle"), TEXT("First inserted")));

			return Table;
		}

		UCortexTestDataAsset* CreateDataAsset()
		{
			return CreateAsset<UCortexTestDataAsset>(TEXT("DA_CortexImportQueueAsset"));
		}

		UStringTable* CreateStringTable()
		{
			UStringTable* Table = CreateAsset<UStringTable>(TEXT("ST_CortexImportQueueStrings"));
			if (Table != nullptr)
			{
				Table->GetMutableStringTable()->SetNamespace(TEXT("CortexImportQueueTests"));
			}
			return Table;
		}

		FCortexCommandRouter CreateRouter() const
		{
			return CreateDataImportQueueTestRouter();
		}

		FString MakeSavedOutputPath(const FString& FileName) const
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexImportQueueTests"), RunId, FileName);
		}

		void WriteQueueFile(const FString& FilePath, const FString& QueueId, const TArray<TSharedRef<FJsonObject>>& Operations) const
		{
			TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
			Queue->SetNumberField(TEXT("schema_version"), 1);
			Queue->SetStringField(TEXT("queue_id"), QueueId);
			Queue->SetStringField(TEXT("domain"), TEXT("test"));
			Queue->SetBoolField(TEXT("valid"), true);
			Queue->SetStringField(TEXT("generator"), TEXT("CortexDataImportQueueTest"));

			TArray<TSharedPtr<FJsonValue>> OperationValues;
			for (const TSharedRef<FJsonObject>& Operation : Operations)
			{
				OperationValues.Add(MakeShared<FJsonValueObject>(Operation));
			}
			Queue->SetArrayField(TEXT("operations"), OperationValues);

			const FString Contents = FCortexSafeFileContract::SerializeCanonicalJson(Queue);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
			FFileHelper::SaveStringToFile(Contents, *FilePath);
		}

		TSharedRef<FJsonObject> MakeUpdateRowOperation(
			const FString& Id,
			const FString& TablePath,
			const FString& RowName,
			const TSharedRef<FJsonObject>& RowData) const
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("table_path"), TablePath);
			Params->SetStringField(TEXT("row_name"), RowName);
			Params->SetObjectField(TEXT("row_data"), RowData);

			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("id"), Id);
			Operation->SetStringField(TEXT("phase"), TEXT("apply"));
			Operation->SetStringField(TEXT("command"), TEXT("update_datatable_row"));
			Operation->SetObjectField(TEXT("params"), Params);
			Operation->SetStringField(TEXT("source_page"), TEXT("DesignWiki/Test.md"));
			return Operation;
		}

		TSharedRef<FJsonObject> MakeSetTranslationOperation(
			const FString& Id,
			const FString& StringTablePath,
			const FString& Key,
			const FString& Text) const
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("string_table_path"), StringTablePath);
			Params->SetStringField(TEXT("key"), Key);
			Params->SetStringField(TEXT("text"), Text);

			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("id"), Id);
			Operation->SetStringField(TEXT("phase"), TEXT("apply"));
			Operation->SetStringField(TEXT("command"), TEXT("set_translation"));
			Operation->SetObjectField(TEXT("params"), Params);
			Operation->SetStringField(TEXT("source_page"), TEXT("DesignWiki/Test.md"));
			return Operation;
		}

		bool TryReadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError) const
		{
			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *FilePath))
			{
				OutError = FString::Printf(TEXT("Failed to read JSON file: %s"), *FilePath);
				return false;
			}

			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
			if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
			{
				OutError = FString::Printf(TEXT("Failed to parse JSON file: %s"), *FilePath);
				return false;
			}

			return true;
		}

		void Cleanup()
		{
			for (int32 Index = CreatedPackageNames.Num() - 1; Index >= 0; --Index)
			{
				CleanupImportQueuePackageByName(CreatedPackageNames[Index]);
			}
			CreatedPackageNames.Empty();
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(MakeSavedOutputPath(TEXT("cleanup.json"))), false, true);
		}

	private:
		template <typename AssetType>
		AssetType* CreateAsset(const FString& AssetName)
		{
			const FString PackageName = ImportQueuePackagePathForAsset(RunId, AssetName);
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueCommandsRegisteredTest,
	"Cortex.Data.ImportQueue.CommandsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueCommandsRegisteredTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FCortexDataCommandHandler Handler;
	TestTrue(
		TEXT("apply_import_ops_json is advertised"),
		ImportQueueSupportedCommandNamesContain(Handler.GetSupportedCommands(), TEXT("apply_import_ops_json")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), TEXT("Saved/CortexImports/missing.json"));
	Params->SetStringField(TEXT("report_path"), TEXT("Saved/CortexImports/report.json"));

	FCortexCommandRouter Router = CreateDataImportQueueTestRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.apply_import_ops_json"), Params);

	TestFalse(TEXT("missing ops file fails"), Result.bSuccess);
	TestEqual(TEXT("missing ops file returns FileNotFound"), Result.ErrorCode, CortexErrorCodes::FileNotFound);
	TestNotEqual(TEXT("missing ops file is not UnknownCommand"), Result.ErrorCode, CortexErrorCodes::UnknownCommand);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueDryRunDefaultWritesReportTest,
	"Cortex.Data.ImportQueue.DryRunDefaultWritesReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueDryRunDefaultWritesReportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Imported Alpha"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-dry-run"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestTrue(TEXT("dry-run queue succeeds"), Result.bSuccess);
	TestTrue(TEXT("dry-run queue returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (!Result.ErrorMessage.IsEmpty())
		{
			AddError(Result.ErrorMessage);
		}
		return false;
	}

	TestEqual(TEXT("status is dry_run_ok"), Result.Data->GetStringField(TEXT("status")), TEXT("dry_run_ok"));
	TestTrue(TEXT("dry_run defaults true"), Result.Data->GetBoolField(TEXT("dry_run")));
	TestFalse(TEXT("applied is false"), Result.Data->GetBoolField(TEXT("applied")));
	TestEqual(TEXT("summary schema_version is 1"), static_cast<int32>(Result.Data->GetNumberField(TEXT("schema_version"))), 1);
	TestEqual(TEXT("summary queue_id matches"), Result.Data->GetStringField(TEXT("queue_id")), TEXT("queue-dry-run"));
	TestTrue(TEXT("summary contains ops hash"), Result.Data->HasTypedField<EJson::String>(TEXT("ops_sha256")));
	TestTrue(TEXT("summary contains first_error"), Result.Data->HasField(TEXT("first_error")));
	TestTrue(TEXT("summary contains report_bytes"), Result.Data->HasTypedField<EJson::Number>(TEXT("report_bytes")));
	TestTrue(TEXT("summary contains dirty_package_count"), Result.Data->HasTypedField<EJson::Number>(TEXT("dirty_package_count")));
	TestTrue(TEXT("summary contains requires_user_action"), Result.Data->HasTypedField<EJson::Boolean>(TEXT("requires_user_action")));
	TestFalse(TEXT("MCP summary omits operations array"), Result.Data->HasField(TEXT("operations")));
	TestTrue(TEXT("report file is written"), IFileManager::Get().FileExists(*ReportPath));

	TSharedPtr<FJsonObject> Report;
	FString ParseError;
	TestTrue(TEXT("report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
	if (Report.IsValid())
	{
		AssertCanonicalImportQueueSummary(
			*this,
			Result.Data,
			ReportPath,
			GetImportQueueStringArrayField(Report, TEXT("targets_touched")));
		TestEqual(TEXT("report queue_id matches"), Report->GetStringField(TEXT("queue_id")), TEXT("queue-dry-run"));
		TestEqual(TEXT("report ops_path matches request"), Report->GetStringField(TEXT("ops_path")), OpsPath);
		TestEqual(TEXT("report canonical_ops_path matches resolved path"), Report->GetStringField(TEXT("canonical_ops_path")), OpsPath);
		TestEqual(TEXT("report operation_count"), static_cast<int32>(Report->GetNumberField(TEXT("operation_count"))), 1);
		TestEqual(TEXT("report previewed_count"), static_cast<int32>(Report->GetNumberField(TEXT("previewed_count"))), 1);
		TestTrue(TEXT("report contains report_bytes"), Report->HasTypedField<EJson::Number>(TEXT("report_bytes")));
		TestTrue(TEXT("report contains requires_user_action"), Report->HasTypedField<EJson::Boolean>(TEXT("requires_user_action")));
		TestTrue(TEXT("report contains operations array"), Report->HasTypedField<EJson::Array>(TEXT("operations")));
		TestTrue(TEXT("report contains ops hash"), Report->HasTypedField<EJson::String>(TEXT("ops_sha256")));

		const TSharedPtr<FJsonObject> Counts = Result.Data->GetObjectField(TEXT("counts"));
		TestEqual(TEXT("summary counts.operations mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("operations"))), static_cast<int32>(Report->GetNumberField(TEXT("operation_count"))));
		TestEqual(TEXT("summary counts.previewed mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("previewed"))), static_cast<int32>(Report->GetNumberField(TEXT("previewed_count"))));
		TestEqual(TEXT("summary counts.applied mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("applied"))), static_cast<int32>(Report->GetNumberField(TEXT("applied_count"))));
		TestEqual(TEXT("summary counts.failed mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("failed"))), static_cast<int32>(Report->GetNumberField(TEXT("failed_count"))));
		TestEqual(TEXT("summary counts.save_failed mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("save_failed"))), static_cast<int32>(Report->GetNumberField(TEXT("save_failed_count"))));
	}

	const FCortexDataLocalizationTestRow* Row =
		reinterpret_cast<const FCortexDataLocalizationTestRow*>(Table->FindRowUnchecked(TEXT("alpha")));
	TestNotNull(TEXT("row still exists after dry run"), Row);
	if (Row != nullptr)
	{
		TestNotEqual(TEXT("dry-run does not mutate row field"), Row->row_name, TEXT("Imported Alpha"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueApplyRequiresExplicitIntentTest,
	"Cortex.Data.ImportQueue.ApplyRequiresExplicitIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueApplyRequiresExplicitIntentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("apply-intent-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("apply-intent-report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Imported Alpha"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-apply-intent"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	TSharedPtr<FJsonObject> MissingApplyParams = MakeShared<FJsonObject>();
	MissingApplyParams->SetStringField(TEXT("ops_path"), OpsPath);
	MissingApplyParams->SetStringField(TEXT("report_path"), ReportPath);
	MissingApplyParams->SetBoolField(TEXT("dry_run"), false);

	const FCortexCommandResult MissingApplyResult =
		Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), MissingApplyParams);
	TestFalse(TEXT("real apply without apply flag fails"), MissingApplyResult.bSuccess);
	TestEqual(
		TEXT("missing apply uses InvalidOperation"),
		MissingApplyResult.ErrorCode,
		CortexErrorCodes::InvalidOperation);

	TSharedPtr<FJsonObject> ContradictoryParams = MakeShared<FJsonObject>();
	ContradictoryParams->SetStringField(TEXT("ops_path"), OpsPath);
	ContradictoryParams->SetStringField(TEXT("report_path"), ReportPath);
	ContradictoryParams->SetBoolField(TEXT("dry_run"), true);
	ContradictoryParams->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult ContradictoryResult =
		Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), ContradictoryParams);
	TestFalse(TEXT("contradictory dry_run/apply fails"), ContradictoryResult.bSuccess);
	TestEqual(
		TEXT("contradictory dry_run/apply uses InvalidOperation"),
		ContradictoryResult.ErrorCode,
		CortexErrorCodes::InvalidOperation);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueAppliesDataTableRowAndQueryBacksTest,
	"Cortex.Data.ImportQueue.AppliesDataTableRowAndQueryBacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueAppliesDataTableRowAndQueryBacksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("apply-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("apply-report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Imported Alpha"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-apply"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestTrue(TEXT("real apply succeeds"), Result.bSuccess);
	TestTrue(TEXT("real apply returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (!Result.ErrorMessage.IsEmpty())
		{
			AddError(Result.ErrorMessage);
		}
		return false;
	}

	TestEqual(TEXT("status is applied_ok"), Result.Data->GetStringField(TEXT("status")), TEXT("applied_ok"));
	TestEqual(TEXT("summary schema_version is 1"), static_cast<int32>(Result.Data->GetNumberField(TEXT("schema_version"))), 1);
	TestEqual(TEXT("summary queue_id matches"), Result.Data->GetStringField(TEXT("queue_id")), TEXT("queue-apply"));
	TestTrue(TEXT("summary contains ops hash"), Result.Data->HasTypedField<EJson::String>(TEXT("ops_sha256")));

	const FCortexDataLocalizationTestRow* Row =
		reinterpret_cast<const FCortexDataLocalizationTestRow*>(Table->FindRowUnchecked(TEXT("alpha")));
	TestNotNull(TEXT("row still exists after apply"), Row);
	if (Row != nullptr)
	{
		TestEqual(TEXT("real apply mutates row field"), Row->row_name, TEXT("Imported Alpha"));
	}

	TSharedPtr<FJsonObject> Report;
	FString ParseError;
	TestTrue(TEXT("report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
	if (Report.IsValid())
	{
		AssertCanonicalImportQueueSummary(
			*this,
			Result.Data,
			ReportPath,
			GetImportQueueStringArrayField(Report, TEXT("targets_touched")));
		const TSharedPtr<FJsonObject> Counts = Result.Data->GetObjectField(TEXT("counts"));
		TestEqual(TEXT("summary counts.attempted mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("attempted"))), static_cast<int32>(Report->GetNumberField(TEXT("attempted_count"))));
		TestEqual(TEXT("summary counts.applied mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("applied"))), static_cast<int32>(Report->GetNumberField(TEXT("applied_count"))));
		TestEqual(TEXT("summary counts.failed mirrors report"), static_cast<int32>(Counts->GetNumberField(TEXT("failed"))), static_cast<int32>(Report->GetNumberField(TEXT("failed_count"))));

		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		TestTrue(TEXT("report contains operations array"), Report->TryGetArrayField(TEXT("operations"), Operations));
		if (Operations != nullptr && Operations->Num() == 1)
		{
			const TSharedPtr<FJsonObject> OperationObject = (*Operations)[0]->AsObject();
			TestTrue(TEXT("operation report exists"), OperationObject.IsValid());
			if (OperationObject.IsValid())
			{
				TestEqual(TEXT("operation status is applied"), OperationObject->GetStringField(TEXT("status")), TEXT("applied"));
				TestTrue(TEXT("operation has query_back"), OperationObject->HasTypedField<EJson::Object>(TEXT("query_back")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueAppliesSetTranslationAndQueryBacksTest,
	"Cortex.Data.ImportQueue.AppliesSetTranslationAndQueryBacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueAppliesSetTranslationAndQueryBacksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("set-translation-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("set-translation-report.json"));

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("string_table_path"), StringTable->GetPathName());
	ParamsObject->SetStringField(TEXT("key"), TEXT("fireball.body"));
	ParamsObject->SetStringField(TEXT("text"), TEXT("Deals fire damage."));

	TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("id"), TEXT("st.set_translation.fireball"));
	Operation->SetStringField(TEXT("phase"), TEXT("apply"));
	Operation->SetStringField(TEXT("command"), TEXT("set_translation"));
	Operation->SetObjectField(TEXT("params"), ParamsObject);
	Operation->SetStringField(TEXT("source_page"), TEXT("DesignWiki/Test.md"));

	Fixture.WriteQueueFile(OpsPath, TEXT("queue-set-translation"), { Operation });

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestTrue(TEXT("set_translation apply succeeds"), Result.bSuccess);
	TestTrue(TEXT("set_translation apply returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (!Result.ErrorMessage.IsEmpty())
		{
			AddError(Result.ErrorMessage);
		}
		return false;
	}

	FString CurrentValue;
	TestTrue(
		TEXT("string table contains applied key"),
		StringTable->GetStringTable()->GetSourceString(TEXT("fireball.body"), CurrentValue));
	TestEqual(TEXT("string table value changed"), CurrentValue, TEXT("Deals fire damage."));

	TSharedPtr<FJsonObject> Report;
	FString ParseError;
	TestTrue(TEXT("report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
	if (Report.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		TestTrue(TEXT("report contains operations array"), Report->TryGetArrayField(TEXT("operations"), Operations));
		if (Operations != nullptr && Operations->Num() == 1)
		{
			const TSharedPtr<FJsonObject> OperationObject = (*Operations)[0]->AsObject();
			TestTrue(TEXT("operation report exists"), OperationObject.IsValid());
			if (OperationObject.IsValid())
			{
				TestEqual(TEXT("operation status is applied"), OperationObject->GetStringField(TEXT("status")), TEXT("applied"));
				TestTrue(TEXT("operation has target"), OperationObject->HasTypedField<EJson::String>(TEXT("target")));
				if (OperationObject->HasTypedField<EJson::String>(TEXT("target")))
				{
					const FString Target = OperationObject->GetStringField(TEXT("target"));
					TestTrue(TEXT("target contains string table path"), Target.Contains(StringTable->GetPathName()));
					TestTrue(TEXT("target contains key"), Target.Contains(TEXT("fireball.body")));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueAppliesDataAssetAndQueryBacksTest,
	"Cortex.Data.ImportQueue.AppliesDataAssetAndQueryBacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueAppliesDataAssetAndQueryBacksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}

	DataAsset->TestProperty = TEXT("Original Value");

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("asset-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("asset-report.json"));

	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("TestProperty"), TEXT("Imported value"));

	TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("asset_path"), DataAsset->GetPathName());
	ParamsObject->SetObjectField(TEXT("properties"), Properties);

	TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("id"), TEXT("asset.update.name"));
	Operation->SetStringField(TEXT("phase"), TEXT("apply"));
	Operation->SetStringField(TEXT("command"), TEXT("update_data_asset"));
	Operation->SetObjectField(TEXT("params"), ParamsObject);
	Operation->SetStringField(TEXT("source_page"), TEXT("DesignWiki/Test.md"));

	Fixture.WriteQueueFile(OpsPath, TEXT("queue-data-asset"), { Operation });

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestTrue(TEXT("data asset apply succeeds"), Result.bSuccess);
	TestTrue(TEXT("data asset apply returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (!Result.ErrorMessage.IsEmpty())
		{
			AddError(Result.ErrorMessage);
		}
		return false;
	}

	TestEqual(TEXT("data asset property changed"), DataAsset->TestProperty, TEXT("Imported value"));

	TSharedPtr<FJsonObject> Report;
	FString ParseError;
	TestTrue(TEXT("report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
	if (Report.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		TestTrue(TEXT("report contains operations array"), Report->TryGetArrayField(TEXT("operations"), Operations));
		if (Operations != nullptr && Operations->Num() == 1)
		{
			const TSharedPtr<FJsonObject> OperationObject = (*Operations)[0]->AsObject();
			TestTrue(TEXT("operation report exists"), OperationObject.IsValid());
			if (OperationObject.IsValid())
			{
				TestEqual(TEXT("operation status is applied"), OperationObject->GetStringField(TEXT("status")), TEXT("applied"));
				TestTrue(TEXT("operation has query_back"), OperationObject->HasTypedField<EJson::Object>(TEXT("query_back")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueRejectsInvalidQueueShapesTest,
	"Cortex.Data.ImportQueue.RejectsInvalidQueueShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueRejectsInvalidQueueShapesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	struct FInvalidQueueCase
	{
		FString Name;
		TSharedRef<FJsonObject> Queue;
		FString ExpectedErrorCode;
	};

	FCortexDataImportQueueTestFixture Fixture;
	TArray<FInvalidQueueCase> Cases;

	{
		TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
		Queue->SetNumberField(TEXT("schema_version"), 2);
		Queue->SetStringField(TEXT("queue_id"), TEXT("bad-schema"));
		Queue->SetBoolField(TEXT("valid"), true);
		Queue->SetArrayField(TEXT("operations"), {});
		Cases.Add({ TEXT("invalid schema"), Queue, CortexErrorCodes::InvalidQueueShape });
	}

	{
		TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
		Queue->SetNumberField(TEXT("schema_version"), 1);
		Queue->SetBoolField(TEXT("valid"), true);
		Queue->SetArrayField(TEXT("operations"), {});
		Cases.Add({ TEXT("missing queue id"), Queue, CortexErrorCodes::InvalidQueueShape });
	}

	{
		TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
		Queue->SetNumberField(TEXT("schema_version"), 1);
		Queue->SetStringField(TEXT("queue_id"), TEXT("not-valid"));
		Queue->SetBoolField(TEXT("valid"), false);
		Queue->SetArrayField(TEXT("operations"), {});
		Cases.Add({ TEXT("valid false"), Queue, CortexErrorCodes::InvalidQueueShape });
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/Missing"));
		Params->SetStringField(TEXT("row_name"), TEXT("alpha"));
		Params->SetObjectField(TEXT("row_data"), MakeShared<FJsonObject>());

		TSharedRef<FJsonObject> OpA = MakeShared<FJsonObject>();
		OpA->SetStringField(TEXT("id"), TEXT("dup"));
		OpA->SetStringField(TEXT("phase"), TEXT("apply"));
		OpA->SetStringField(TEXT("command"), TEXT("update_datatable_row"));
		OpA->SetObjectField(TEXT("params"), Params);

		TSharedRef<FJsonObject> OpB = MakeShared<FJsonObject>(*OpA);

		TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
		Queue->SetNumberField(TEXT("schema_version"), 1);
		Queue->SetStringField(TEXT("queue_id"), TEXT("dup-ids"));
		Queue->SetBoolField(TEXT("valid"), true);
		Queue->SetArrayField(TEXT("operations"), {
			MakeShared<FJsonValueObject>(OpA),
			MakeShared<FJsonValueObject>(OpB)
		});
		Cases.Add({ TEXT("duplicate ids"), Queue, CortexErrorCodes::InvalidQueueShape });
	}

	{
		TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("id"), TEXT("bad-command"));
		Op->SetStringField(TEXT("phase"), TEXT("apply"));
		Op->SetStringField(TEXT("command"), TEXT("delete_everything"));
		Op->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		TSharedRef<FJsonObject> Queue = MakeShared<FJsonObject>();
		Queue->SetNumberField(TEXT("schema_version"), 1);
		Queue->SetStringField(TEXT("queue_id"), TEXT("unsupported-command"));
		Queue->SetBoolField(TEXT("valid"), true);
		Queue->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Op) });
		Cases.Add({ TEXT("unsupported command"), Queue, CortexErrorCodes::UnsupportedCommand });
	}

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FString OpsPath = Fixture.MakeSavedOutputPath(FString::Printf(TEXT("invalid-queue-%d.json"), Index));
		const FString ReportPath = Fixture.MakeSavedOutputPath(FString::Printf(TEXT("invalid-report-%d.json"), Index));
		const FString Contents = FCortexSafeFileContract::SerializeCanonicalJson(Cases[Index].Queue);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OpsPath), true);
		FFileHelper::SaveStringToFile(Contents, *OpsPath);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("ops_path"), OpsPath);
		Params->SetStringField(TEXT("report_path"), ReportPath);

		const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
		TestFalse(*FString::Printf(TEXT("%s fails"), *Cases[Index].Name), Result.bSuccess);
		TestEqual(
			*FString::Printf(TEXT("%s error code"), *Cases[Index].Name),
			Result.ErrorCode,
			Cases[Index].ExpectedErrorCode);
	}

	TSharedPtr<FJsonObject> TypoParams = MakeShared<FJsonObject>();
	TypoParams->SetStringField(TEXT("ops_path"), Fixture.MakeSavedOutputPath(TEXT("typo-queue.json")));
	TypoParams->SetStringField(TEXT("report_path"), Fixture.MakeSavedOutputPath(TEXT("typo-report.json")));
	TypoParams->SetBoolField(TEXT("dryrun"), true);

	const FCortexCommandResult TypoResult = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), TypoParams);
	TestFalse(TEXT("unknown top-level param fails"), TypoResult.bSuccess);
	TestEqual(TEXT("unknown top-level param uses InvalidField"), TypoResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueRejectsUnsafePathsTest,
	"Cortex.Data.ImportQueue.RejectsUnsafePaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueRejectsUnsafePathsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("safe-queue.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Imported Alpha"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-safe"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	TSharedPtr<FJsonObject> UnsafeReportParams = MakeShared<FJsonObject>();
	UnsafeReportParams->SetStringField(TEXT("ops_path"), OpsPath);
	UnsafeReportParams->SetStringField(TEXT("report_path"), TEXT("C:/CortexUnsafe/report.json"));

	const FCortexCommandResult UnsafeReportResult =
		Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), UnsafeReportParams);
	TestFalse(TEXT("unsafe report path fails"), UnsafeReportResult.bSuccess);
	TestEqual(TEXT("unsafe report path uses InvalidFilePath"), UnsafeReportResult.ErrorCode, CortexErrorCodes::InvalidFilePath);

	TSharedPtr<FJsonObject> SameFileParams = MakeShared<FJsonObject>();
	SameFileParams->SetStringField(TEXT("ops_path"), OpsPath);
	SameFileParams->SetStringField(TEXT("report_path"), OpsPath);

	const FCortexCommandResult SameFileResult =
		Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), SameFileParams);
	TestFalse(TEXT("same ops/report file fails"), SameFileResult.bSuccess);
	TestEqual(TEXT("same ops/report file uses InvalidFilePath"), SameFileResult.ErrorCode, CortexErrorCodes::InvalidFilePath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueuePartialAndStopMatrixTest,
	"Cortex.Data.ImportQueue.PartialAndStopMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueuePartialAndStopMatrixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto RunCase = [this](
		const bool bAllowPartial,
		const bool bStopOnError,
		const FString& ExpectedStatus,
		const int32 ExpectedValidatedCount,
		const int32 ExpectedAttemptedCount,
		const int32 ExpectedAppliedCount,
		const int32 ExpectedFailedCount,
		const int32 ExpectedSkippedCount,
		const bool bExpectRowChanged,
		const bool bExpectStringChanged)
	{
		FCortexDataImportQueueTestFixture Fixture;
		UDataTable* Table = Fixture.CreateRegularDataTable();
		UStringTable* StringTable = Fixture.CreateStringTable();
		TestNotNull(TEXT("matrix DataTable fixture is created"), Table);
		TestNotNull(TEXT("matrix StringTable fixture is created"), StringTable);
		if (Table == nullptr || StringTable == nullptr)
		{
			return false;
		}

		const FString OpsPath = Fixture.MakeSavedOutputPath(FString::Printf(TEXT("matrix-%d-%d-queue.json"), bAllowPartial ? 1 : 0, bStopOnError ? 1 : 0));
		const FString ReportPath = Fixture.MakeSavedOutputPath(FString::Printf(TEXT("matrix-%d-%d-report.json"), bAllowPartial ? 1 : 0, bStopOnError ? 1 : 0));

		TSharedRef<FJsonObject> ValidRowData = MakeShared<FJsonObject>();
		ValidRowData->SetStringField(TEXT("row_name"), TEXT("Matrix Applied"));
		TSharedRef<FJsonObject> InvalidRowData = MakeShared<FJsonObject>();
		InvalidRowData->SetStringField(TEXT("row_name"), TEXT("Should Fail"));

		Fixture.WriteQueueFile(
			OpsPath,
			TEXT("queue-matrix"),
			{
				Fixture.MakeUpdateRowOperation(TEXT("op.valid.row"), Table->GetPathName(), TEXT("alpha"), ValidRowData),
				Fixture.MakeUpdateRowOperation(TEXT("op.invalid.row"), Table->GetPathName(), TEXT("missing_row"), InvalidRowData),
				Fixture.MakeSetTranslationOperation(TEXT("op.valid.translation"), StringTable->GetPathName(), TEXT("matrix.key"), TEXT("Matrix text"))
			});

		TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("ops_path"), OpsPath);
		ParamsObject->SetStringField(TEXT("report_path"), ReportPath);
		ParamsObject->SetBoolField(TEXT("dry_run"), false);
		ParamsObject->SetBoolField(TEXT("apply"), true);
		ParamsObject->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		ParamsObject->SetBoolField(TEXT("stop_on_error"), bStopOnError);

		const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), ParamsObject);
		TestTrue(*FString::Printf(TEXT("matrix case %s returns payload"), *ExpectedStatus), Result.Data.IsValid() || Result.ErrorDetails.IsValid());

		TSharedPtr<FJsonObject> Report;
		FString ParseError;
		TestTrue(TEXT("matrix report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
		if (!Report.IsValid())
		{
			return false;
		}

		TestEqual(TEXT("matrix status matches"), Report->GetStringField(TEXT("status")), ExpectedStatus);
		TestEqual(TEXT("matrix validated_count matches"), static_cast<int32>(Report->GetNumberField(TEXT("validated_count"))), ExpectedValidatedCount);
		TestEqual(TEXT("matrix attempted_count matches"), static_cast<int32>(Report->GetNumberField(TEXT("attempted_count"))), ExpectedAttemptedCount);
		TestEqual(TEXT("matrix applied_count matches"), static_cast<int32>(Report->GetNumberField(TEXT("applied_count"))), ExpectedAppliedCount);
		TestEqual(TEXT("matrix failed_count matches"), static_cast<int32>(Report->GetNumberField(TEXT("failed_count"))), ExpectedFailedCount);
		TestEqual(TEXT("matrix skipped_count matches"), static_cast<int32>(Report->GetNumberField(TEXT("skipped_count"))), ExpectedSkippedCount);

		const FCortexDataLocalizationTestRow* Row =
			reinterpret_cast<const FCortexDataLocalizationTestRow*>(Table->FindRowUnchecked(TEXT("alpha")));
		TestNotNull(TEXT("matrix alpha row exists"), Row);
		if (Row != nullptr)
		{
			if (bExpectRowChanged)
			{
				TestEqual(TEXT("matrix row changed"), Row->row_name, TEXT("Matrix Applied"));
			}
			else
			{
				TestNotEqual(TEXT("matrix row unchanged"), Row->row_name, TEXT("Matrix Applied"));
			}
		}

		FString CurrentValue;
		const bool bHasStringValue = StringTable->GetStringTable()->GetSourceString(TEXT("matrix.key"), CurrentValue);
		if (bExpectStringChanged)
		{
			TestTrue(TEXT("matrix string changed"), bHasStringValue);
			if (bHasStringValue)
			{
				TestEqual(TEXT("matrix string value"), CurrentValue, TEXT("Matrix text"));
			}
		}
		else
		{
			TestFalse(TEXT("matrix string unchanged"), bHasStringValue);
		}

		return true;
	};

	TestTrue(TEXT("strict preflight blocks before mutation"), RunCase(false, true, TEXT("preflight_failed"), 0, 0, 0, 1, 2, false, false));
	TestTrue(TEXT("partial stop_on_error still runs later validated ops"), RunCase(true, true, TEXT("partial_applied"), 2, 2, 2, 1, 0, true, true));
	TestTrue(TEXT("partial continue applies independent later op"), RunCase(true, false, TEXT("partial_applied"), 2, 2, 2, 1, 0, true, true));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueQueryBackDisabledTest,
	"Cortex.Data.ImportQueue.QueryBackDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueQueryBackDisabledTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("queryback-disabled-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("queryback-disabled-report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("No QueryBack"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-queryback-disabled"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);
	Params->SetBoolField(TEXT("query_back"), false);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestTrue(TEXT("query_back=false still applies"), Result.bSuccess);

	TSharedPtr<FJsonObject> Report;
	FString ParseError;
	TestTrue(TEXT("query_back=false report parses"), Fixture.TryReadJsonFile(ReportPath, Report, ParseError));
	if (!Report.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	TestTrue(TEXT("query_back=false report contains operations"), Report->TryGetArrayField(TEXT("operations"), Operations));
	if (Operations == nullptr || Operations->Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> OperationObject = (*Operations)[0]->AsObject();
	TestTrue(TEXT("query_back=false operation exists"), OperationObject.IsValid());
	if (!OperationObject.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("query_back=false has query_back object"), OperationObject->HasTypedField<EJson::Object>(TEXT("query_back")));
	if (OperationObject->HasTypedField<EJson::Object>(TEXT("query_back")))
	{
		const TSharedPtr<FJsonObject> QueryBack = OperationObject->GetObjectField(TEXT("query_back"));
		TestEqual(TEXT("query_back disabled status"), QueryBack->GetStringField(TEXT("status")), TEXT("skipped"));
		TestEqual(TEXT("query_back disabled reason"), QueryBack->GetStringField(TEXT("reason")), TEXT("query_back_disabled"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueReportPathPrecheckFailureTest,
	"Cortex.Data.ImportQueue.ReportPathPrecheckFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueReportPathPrecheckFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("report-precheck-queue.json"));
	const FString BlockerPath = Fixture.MakeSavedOutputPath(TEXT("ReportParentBlocker"));
	const FString ReportPath = FPaths::Combine(BlockerPath, TEXT("report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Should Not Apply"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-report-precheck"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });
	FFileHelper::SaveStringToFile(TEXT("blocker"), *BlockerPath);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	TestFalse(TEXT("blocked report parent fails"), Result.bSuccess);

	const FCortexDataLocalizationTestRow* Row =
		reinterpret_cast<const FCortexDataLocalizationTestRow*>(Table->FindRowUnchecked(TEXT("alpha")));
	TestNotNull(TEXT("row still exists after blocked report parent"), Row);
	if (Row != nullptr)
	{
		TestNotEqual(TEXT("blocked report parent prevents mutation"), Row->row_name, TEXT("Should Not Apply"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueForcedFinalReportWriteFailureTest,
	"Cortex.Data.ImportQueue.ForcedFinalReportWriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueForcedFinalReportWriteFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* Table = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("DataTable fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OpsPath = Fixture.MakeSavedOutputPath(TEXT("forced-report-failure-queue.json"));
	const FString ReportPath = Fixture.MakeSavedOutputPath(TEXT("forced-report-failure-report.json"));
	TSharedRef<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Applied Before Report Failure"));
	Fixture.WriteQueueFile(
		OpsPath,
		TEXT("queue-forced-report-failure"),
		{ Fixture.MakeUpdateRowOperation(TEXT("op.update.row_name"), Table->GetPathName(), TEXT("alpha"), RowData) });

	FCortexDataImportQueueOps::SetForceFinalReportWriteFailureForTests(true);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("ops_path"), OpsPath);
	Params->SetStringField(TEXT("report_path"), ReportPath);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("apply"), true);

	const FCortexCommandResult Result = Fixture.CreateRouter().Execute(TEXT("data.apply_import_ops_json"), Params);
	FCortexDataImportQueueOps::SetForceFinalReportWriteFailureForTests(false);

	TestFalse(TEXT("forced final report write failure returns error"), Result.bSuccess);
	TestEqual(TEXT("forced final report write failure uses SaveFailed"), Result.ErrorCode, CortexErrorCodes::SaveFailed);
	TestTrue(TEXT("forced final report write failure returns details"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("report failure status"), Result.ErrorDetails->GetStringField(TEXT("status")), TEXT("report_write_failed"));
		TestEqual(TEXT("report failure attempted_count"), static_cast<int32>(Result.ErrorDetails->GetNumberField(TEXT("attempted_count"))), 1);
		TestEqual(TEXT("report failure applied_count"), static_cast<int32>(Result.ErrorDetails->GetNumberField(TEXT("applied_count"))), 1);
		TestEqual(TEXT("report failure failed_count"), static_cast<int32>(Result.ErrorDetails->GetNumberField(TEXT("failed_count"))), 0);
		TestTrue(TEXT("report failure has first_error"), Result.ErrorDetails->HasTypedField<EJson::String>(TEXT("first_error")));
	}

	const FCortexDataLocalizationTestRow* Row =
		reinterpret_cast<const FCortexDataLocalizationTestRow*>(Table->FindRowUnchecked(TEXT("alpha")));
	TestNotNull(TEXT("row still exists after forced report failure"), Row);
	if (Row != nullptr)
	{
		TestEqual(TEXT("mutation happened before forced report failure"), Row->row_name, TEXT("Applied Before Report Failure"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityUpdateDatatableRowTest,
	"Cortex.Data.ImportQueue.HelperParity.UpdateDatatableRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityUpdateDatatableRowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), DataTable);
	if (DataTable == nullptr)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Imported Alpha"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), DataTable->GetPathName());
	Params->SetStringField(TEXT("row_name"), TEXT("alpha"));
	Params->SetObjectField(TEXT("row_data"), RowData);
	Params->SetBoolField(TEXT("dry_run"), false);

	FCortexCommandRouter Router = CreateDataImportQueueTestRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.update_datatable_row"), Params);

	TestTrue(TEXT("update_datatable_row succeeds"), Result.bSuccess);
	TestTrue(TEXT("update_datatable_row returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		if (!Result.ErrorMessage.IsEmpty())
		{
			AddError(Result.ErrorMessage);
		}
		return false;
	}

	TestTrue(TEXT("modified_fields exists"), Result.Data->HasTypedField<EJson::Array>(TEXT("modified_fields")));

	const uint8* RowDataPtr = DataTable->FindRowUnchecked(TEXT("alpha"));
	TestNotNull(TEXT("alpha row still exists"), RowDataPtr);
	if (RowDataPtr == nullptr)
	{
		return false;
	}

	const FCortexDataLocalizationTestRow* AlphaRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(RowDataPtr);
	TestEqual(TEXT("alpha row field changed through public router"), AlphaRow->row_name, TEXT("Imported Alpha"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityUpdateDatatableRowRejectsUnknownFieldTest,
	"Cortex.Data.ImportQueue.HelperParity.UpdateDatatableRowRejectsUnknownField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityUpdateDatatableRowRejectsUnknownFieldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), DataTable);
	if (DataTable == nullptr)
	{
		return false;
	}

	const uint8* OriginalRowDataPtr = DataTable->FindRowUnchecked(TEXT("alpha"));
	TestNotNull(TEXT("alpha row exists before update"), OriginalRowDataPtr);
	if (OriginalRowDataPtr == nullptr)
	{
		return false;
	}
	const FCortexDataLocalizationTestRow* OriginalAlphaRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(OriginalRowDataPtr);
	const FString OriginalRowName = OriginalAlphaRow->row_name;

	TSharedPtr<FJsonObject> RowData = MakeShared<FJsonObject>();
	RowData->SetStringField(TEXT("row_name"), TEXT("Should Not Apply"));
	RowData->SetStringField(TEXT("unknown_field"), TEXT("invalid"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), DataTable->GetPathName());
	Params->SetStringField(TEXT("row_name"), TEXT("alpha"));
	Params->SetObjectField(TEXT("row_data"), RowData);
	Params->SetBoolField(TEXT("dry_run"), false);

	FCortexCommandRouter Router = CreateDataImportQueueTestRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.update_datatable_row"), Params);

	TestFalse(TEXT("update_datatable_row rejects unknown fields during planning"), Result.bSuccess);
	TestEqual(TEXT("unknown field failure uses SerializationError"), Result.ErrorCode, CortexErrorCodes::SerializationError);

	const uint8* CurrentRowDataPtr = DataTable->FindRowUnchecked(TEXT("alpha"));
	TestNotNull(TEXT("alpha row still exists after rejected update"), CurrentRowDataPtr);
	if (CurrentRowDataPtr == nullptr)
	{
		return false;
	}
	const FCortexDataLocalizationTestRow* CurrentAlphaRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(CurrentRowDataPtr);
	TestEqual(TEXT("rejected update does not mutate existing row field"), CurrentAlphaRow->row_name, OriginalRowName);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityUpdateDataAssetRejectsUnknownPropertyTest,
	"Cortex.Data.ImportQueue.HelperParity.UpdateDataAssetRejectsUnknownProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityUpdateDataAssetRejectsUnknownPropertyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}

	DataAsset->TestProperty = TEXT("Original Value");
	DataAsset->TestNumber = 7;

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("TestProperty"), TEXT("Should Not Apply"));
	Properties->SetStringField(TEXT("UnknownProperty"), TEXT("invalid"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), DataAsset->GetPathName());
	Params->SetObjectField(TEXT("properties"), Properties);
	Params->SetBoolField(TEXT("dry_run"), false);

	FCortexCommandRouter Router = CreateDataImportQueueTestRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.update_data_asset"), Params);

	TestFalse(TEXT("update_data_asset rejects unknown properties during planning"), Result.bSuccess);
	TestEqual(TEXT("unknown property failure uses SerializationError"), Result.ErrorCode, CortexErrorCodes::SerializationError);
	TestEqual(TEXT("rejected update does not mutate existing string property"), DataAsset->TestProperty, TEXT("Original Value"));
	TestEqual(TEXT("rejected update does not mutate existing numeric property"), DataAsset->TestNumber, 7);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityUpdateStringTableRejectsMalformedOperationTest,
	"Cortex.Data.ImportQueue.HelperParity.UpdateStringTableRejectsMalformedOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityUpdateStringTableRejectsMalformedOperationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable == nullptr)
	{
		return false;
	}

	StringTable->GetMutableStringTable()->SetSourceString(TEXT("existing"), TEXT("Original"));

	TSharedRef<FJsonObject> ValidSetOperation = MakeShared<FJsonObject>();
	ValidSetOperation->SetStringField(TEXT("type"), TEXT("set"));
	ValidSetOperation->SetStringField(TEXT("key"), TEXT("new_key"));
	ValidSetOperation->SetStringField(TEXT("source_string"), TEXT("Should Not Apply"));

	TSharedRef<FJsonObject> MalformedSetOperation = MakeShared<FJsonObject>();
	MalformedSetOperation->SetStringField(TEXT("type"), TEXT("set"));
	MalformedSetOperation->SetStringField(TEXT("source_string"), TEXT("Missing key"));

	TArray<TSharedPtr<FJsonValue>> Operations;
	Operations.Add(MakeShared<FJsonValueObject>(ValidSetOperation));
	Operations.Add(MakeShared<FJsonValueObject>(MalformedSetOperation));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("string_table_path"), StringTable->GetPathName());
	Params->SetArrayField(TEXT("operations"), Operations);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("allow_partial"), false);

	FCortexCommandRouter Router = CreateDataImportQueueTestRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("data.update_string_table"), Params);

	TestFalse(TEXT("update_string_table rejects malformed operations during planning"), Result.bSuccess);
	TestEqual(TEXT("malformed operation failure uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	FString ExistingValue;
	TestTrue(TEXT("existing key remains available"), StringTable->GetStringTable()->GetSourceString(TEXT("existing"), ExistingValue));
	TestEqual(TEXT("existing key remains unchanged"), ExistingValue, TEXT("Original"));

	FString NewValue;
	TestFalse(TEXT("valid operation before malformed operation is not applied"), StringTable->GetStringTable()->GetSourceString(TEXT("new_key"), NewValue));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityUpdateDataAssetNoOpMetadataTest,
	"Cortex.Data.ImportQueue.HelperParity.UpdateDataAssetNoOpMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityUpdateDataAssetNoOpMetadataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}

	DataAsset->TestProperty = TEXT("Same Value");

	FCortexUpdateDataAssetMutationRequest Request;
	Request.AssetPath = DataAsset->GetPathName();
	Request.Properties = MakeShared<FJsonObject>();
	Request.Properties->SetStringField(TEXT("TestProperty"), TEXT("Same Value"));
	Request.bDryRun = false;

	FCortexUpdateDataAssetMutationPlan Plan;
	FCortexDataMutationResult BuildResult = FCortexDataMutationHelpers::BuildUpdateDataAssetPlan(Request, Plan);
	TestTrue(TEXT("no-op DataAsset update plan builds"), BuildResult.bSuccess);
	if (!BuildResult.bSuccess)
	{
		if (BuildResult.Errors.Num() > 0)
		{
			AddError(BuildResult.Errors[0].Message);
		}
		return false;
	}

	FCortexDataMutationResult ApplyResult = FCortexDataMutationHelpers::ApplyUpdateDataAsset(Plan);
	TestTrue(TEXT("no-op DataAsset update applies successfully"), ApplyResult.bSuccess);
	TestFalse(TEXT("no-op DataAsset apply does not report changed"), ApplyResult.bChanged);
	TestTrue(TEXT("no-op DataAsset apply reports no-op"), ApplyResult.bNoOp);
	TestEqual(TEXT("no-op DataAsset apply keeps property unchanged"), DataAsset->TestProperty, TEXT("Same Value"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataImportQueueHelperParityImportDatatableJsonNoOpMetadataTest,
	"Cortex.Data.ImportQueue.HelperParity.ImportDatatableJsonNoOpMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataImportQueueHelperParityImportDatatableJsonNoOpMetadataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataImportQueueTestFixture Fixture;
	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), DataTable);
	if (DataTable == nullptr)
	{
		return false;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	const uint8* ExistingRow = DataTable->FindRowUnchecked(TEXT("alpha"));
	TestNotNull(TEXT("alpha row exists before import"), ExistingRow);
	if (RowStruct == nullptr || ExistingRow == nullptr)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RowData = FCortexSerializer::StructToJson(RowStruct, ExistingRow);
	TestTrue(TEXT("existing row serializes for import no-op test"), RowData.IsValid());
	if (!RowData.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("row_name"), TEXT("alpha"));
	Row->SetObjectField(TEXT("row_data"), RowData);

	FCortexImportDatatableJsonMutationRequest Request;
	Request.TablePath = DataTable->GetPathName();
	Request.Rows.Add(MakeShared<FJsonValueObject>(Row));
	Request.Mode = TEXT("upsert");
	Request.bDryRun = false;

	FCortexImportDatatableJsonMutationPlan Plan;
	FCortexDataMutationResult BuildResult = FCortexDataMutationHelpers::BuildImportDatatableJsonPlan(Request, Plan);
	TestTrue(TEXT("identical upsert DataTable import plan builds"), BuildResult.bSuccess);
	if (!BuildResult.bSuccess)
	{
		if (BuildResult.Errors.Num() > 0)
		{
			AddError(BuildResult.Errors[0].Message);
		}
		return false;
	}

	FCortexDataMutationResult ApplyResult = FCortexDataMutationHelpers::ApplyImportDatatableJson(Plan);
	TestTrue(TEXT("identical upsert DataTable import applies successfully"), ApplyResult.bSuccess);
	TestFalse(TEXT("identical upsert DataTable import does not report changed"), ApplyResult.bChanged);
	TestTrue(TEXT("identical upsert DataTable import reports no-op"), ApplyResult.bNoOp);

	return true;
}
