#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Tests/CortexDataLocalizationTestTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectHash.h"

namespace
{
	const TCHAR* JsonDiffTestRoot = TEXT("/Game/CortexJsonDiffTests");

	FCortexCommandRouter CreateDataJsonDiffTestRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	FCortexDataLocalizationTestRow MakeRow(const FString& Title, const FString& StepText)
	{
		FCortexDataLocalizationTestRow Row;
		Row.Title = FText::FromString(Title);
		Row.row_name = Title;

		FCortexDataLocalizationStepTestRow Step;
		Step.Description = FText::FromString(StepText);
		Row.Steps.Add(Step);
		return Row;
	}

	FString JsonDiffPackagePathForAsset(const FString& RunId, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s/%s"), JsonDiffTestRoot, *RunId, *AssetName);
	}

	bool JsonDiffSupportedCommandNamesContain(const TArray<FCortexCommandInfo>& Commands, const FString& CommandName)
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

	void JsonDiffDeletePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	void JsonDiffCleanupLoadedPackage(const FString& PackageName)
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

	void JsonDiffCleanupPackageByName(const FString& PackageName)
	{
		JsonDiffCleanupLoadedPackage(PackageName);
		JsonDiffDeletePackageFile(PackageName);
	}

	class FCortexDataJsonDiffTestFixture
	{
	public:
		FCortexDataJsonDiffTestFixture()
			: RunId(FString::Printf(TEXT("Run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FCortexDataJsonDiffTestFixture()
		{
			Cleanup();
		}

		UDataTable* CreateRegularDataTable(const FString& AssetName)
		{
			UDataTable* Table = CreateAsset<UDataTable>(AssetName);
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();
			Table->AddRow(TEXT("alpha"), MakeRow(TEXT("Alpha"), TEXT("First step")));
			Table->AddRow(TEXT("beta"), MakeRow(TEXT("Beta"), TEXT("Second step")));
			return Table;
		}

		void UpdateRowTitle(UDataTable* Table, const FName RowName, const FString& NewTitle) const
		{
			if (Table == nullptr)
			{
				return;
			}

			if (FCortexDataLocalizationTestRow* Row = Table->FindRow<FCortexDataLocalizationTestRow>(RowName, TEXT("JsonDiffTest")))
			{
				Row->Title = FText::FromString(NewTitle);
			}
		}

		UStringTable* CreateStringTable(const FString& AssetName)
		{
			UStringTable* Table = CreateAsset<UStringTable>(AssetName);
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->GetMutableStringTable()->SetNamespace(TEXT("CortexJsonDiffTests"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("alpha.key"), TEXT("Alpha text"), TEXT(""));
			Table->GetMutableStringTable()->SetSourceString(TEXT("beta.key"), TEXT("Beta text"), TEXT(""));
			return Table;
		}

		void UpdateStringTableValue(UStringTable* Table, const FString& Key, const FString& Value) const
		{
			if (Table != nullptr)
			{
				Table->GetMutableStringTable()->SetSourceString(Key, Value, TEXT(""));
			}
		}

		FString ExportDatatableJson(UDataTable* Table, const FString& FileName) const
		{
			const FString OutputPath = MakeSavedPath(FileName);
			TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
			ParamsObject->SetStringField(TEXT("table_path"), Table != nullptr ? Table->GetPathName() : TEXT(""));
			ParamsObject->SetStringField(TEXT("out_path"), OutputPath);
			const FCortexCommandResult Result = CreateDataJsonDiffTestRouter().Execute(TEXT("data.export_datatable_json"), ParamsObject);
			return Result.bSuccess ? OutputPath : TEXT("");
		}

		FString ExportStringTableJson(UStringTable* Table, const FString& FileName) const
		{
			const FString OutputPath = MakeSavedPath(FileName);
			TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
			ParamsObject->SetStringField(TEXT("string_table_path"), Table != nullptr ? Table->GetPathName() : TEXT(""));
			ParamsObject->SetStringField(TEXT("out_path"), OutputPath);
			const FCortexCommandResult Result = CreateDataJsonDiffTestRouter().Execute(TEXT("data.export_string_table_json"), ParamsObject);
			return Result.bSuccess ? OutputPath : TEXT("");
		}

		FString WriteJsonFile(const FString& FileName, const FString& Contents) const
		{
			const FString OutputPath = MakeSavedPath(FileName);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
			return FFileHelper::SaveStringToFile(Contents, *OutputPath) ? OutputPath : TEXT("");
		}

		TSharedPtr<FJsonObject> ReadJsonFile(const FString& FilePath) const
		{
			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *FilePath))
			{
				return nullptr;
			}

			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
			return FJsonSerializer::Deserialize(Reader, JsonObject) ? JsonObject : nullptr;
		}

		TArray<uint8> ReadFileBytes(const FString& FilePath) const
		{
			TArray<uint8> Bytes;
			FFileHelper::LoadFileToArray(Bytes, *FilePath);
			return Bytes;
		}

		FCortexCommandResult CompareJson(
			const FString& LeftPath,
			const FString& RightPath,
			const FString& ReportPath,
			const FString& Mode,
			const FString& KeyField = TEXT(""),
			const TArray<FString>& IgnoreFields = TArray<FString>(),
			const bool bIncludeEqual = false) const
		{
			TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
			ParamsObject->SetStringField(TEXT("left_path"), LeftPath);
			ParamsObject->SetStringField(TEXT("right_path"), RightPath);
			ParamsObject->SetStringField(TEXT("report_path"), ReportPath);
			if (!Mode.IsEmpty())
			{
				ParamsObject->SetStringField(TEXT("mode"), Mode);
			}
			if (!KeyField.IsEmpty())
			{
				ParamsObject->SetStringField(TEXT("key_field"), KeyField);
			}
			if (IgnoreFields.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> IgnoreFieldValues;
				for (const FString& IgnoreField : IgnoreFields)
				{
					IgnoreFieldValues.Add(MakeShared<FJsonValueString>(IgnoreField));
				}
				ParamsObject->SetArrayField(TEXT("ignore_fields"), IgnoreFieldValues);
			}
			if (bIncludeEqual)
			{
				ParamsObject->SetBoolField(TEXT("include_equal"), true);
			}
			return CreateDataJsonDiffTestRouter().Execute(TEXT("data.compare_data_json"), ParamsObject);
		}

		FCortexCommandResult CompareJsonWithoutMode(
			const FString& LeftPath,
			const FString& RightPath,
			const FString& ReportPath) const
		{
			return CompareJson(LeftPath, RightPath, ReportPath, TEXT(""));
		}

		FString MakeSavedPath(const FString& FileName) const
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexJsonDiffTests"), RunId, FileName);
		}

		bool FileExists(const FString& FilePath) const
		{
			return IFileManager::Get().FileExists(*FilePath);
		}

	private:
		void Cleanup()
		{
			IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexJsonDiffTests"), RunId), false, true);
			for (int32 Index = CreatedPackageNames.Num() - 1; Index >= 0; --Index)
			{
				JsonDiffCleanupPackageByName(CreatedPackageNames[Index]);
			}
			CreatedPackageNames.Empty();
		}

		template <typename AssetType>
		AssetType* CreateAsset(const FString& AssetName)
		{
			const FString PackageName = JsonDiffPackagePathForAsset(RunId, AssetName);
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
	FCortexDataJsonDiffCommandsRegisteredTest,
	"Cortex.Data.JsonDiff.CommandsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffDatatableAutoCanonicalTest,
	"Cortex.Data.JsonDiff.Datatable.AutoCanonical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffAutoRejectsGenericWrappersTest,
	"Cortex.Data.JsonDiff.AutoRejectsGenericWrappers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffAutoRejectsAmbiguousCanonicalShapeTest,
	"Cortex.Data.JsonDiff.AutoRejectsAmbiguousCanonicalShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffModeOmittedDefaultsToAutoTest,
	"Cortex.Data.JsonDiff.Datatable.ModeOmittedDefaultsToAuto",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffStringTableCanonicalTest,
	"Cortex.Data.JsonDiff.StringTable.Canonical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffDataAssetsCanonicalTest,
	"Cortex.Data.JsonDiff.DataAssets.Canonical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffExplicitWrapperModesTest,
	"Cortex.Data.JsonDiff.External.Wrappers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffExplicitKeyFieldTest,
	"Cortex.Data.JsonDiff.External.KeyField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffStringTableExplicitKeyFieldTest,
	"Cortex.Data.JsonDiff.External.StringTableKeyField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffDataAssetsExplicitKeyFieldTest,
	"Cortex.Data.JsonDiff.External.DataAssetsKeyField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffDuplicateKeysFailTest,
	"Cortex.Data.JsonDiff.External.DuplicateKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffIgnoreFieldsTest,
	"Cortex.Data.JsonDiff.Semantics.IgnoreFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffIncludeEqualTest,
	"Cortex.Data.JsonDiff.Semantics.IncludeEqual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffNullPresenceFlagsTest,
	"Cortex.Data.JsonDiff.Semantics.NullPresenceFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffMalformedJsonFailsWithoutReportTest,
	"Cortex.Data.JsonDiff.Errors.MalformedJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffUnsafePathsFailTest,
	"Cortex.Data.JsonDiff.Errors.UnsafePaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffReportCollisionFailsTest,
	"Cortex.Data.JsonDiff.Errors.ReportCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataJsonDiffFailureClearsStaleReportTest,
	"Cortex.Data.JsonDiff.Errors.FailureClearsStaleReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataJsonDiffCommandsRegisteredTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FCortexDataCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	TestTrue(
		TEXT("compare_data_json is advertised"),
		JsonDiffSupportedCommandNamesContain(Commands, TEXT("compare_data_json")));

	FCortexCommandRouter Router = CreateDataJsonDiffTestRouter();

	TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("left_path"), TEXT("Saved/CortexReports/left.json"));
	ParamsObject->SetStringField(TEXT("right_path"), TEXT("Saved/CortexReports/right.json"));
	ParamsObject->SetStringField(TEXT("report_path"), TEXT("Saved/CortexReports/diff.json"));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.compare_data_json"), ParamsObject);
	TestNotEqual(TEXT("command is routed, not unknown"), Result.ErrorCode, CortexErrorCodes::UnknownCommand);

	return true;
}

bool FCortexDataJsonDiffDatatableAutoCanonicalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	UDataTable* LeftTable = Fixture.CreateRegularDataTable(TEXT("DT_Left"));
	UDataTable* RightTable = Fixture.CreateRegularDataTable(TEXT("DT_Right"));
	Fixture.UpdateRowTitle(RightTable, TEXT("alpha"), TEXT("Alpha changed"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		Fixture.ExportDatatableJson(LeftTable, TEXT("left.json")),
		Fixture.ExportDatatableJson(RightTable, TEXT("right.json")),
		Fixture.MakeSavedPath(TEXT("report.json")),
		TEXT("auto"));

	TestTrue(TEXT("canonical datatable compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("canonical datatable compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("mode resolves to datatable_rows"), Result.Data->GetStringField(TEXT("mode")), TEXT("datatable_rows"));
	TestEqual(
		TEXT("changed count is one"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		1);
	return true;
}

bool FCortexDataJsonDiffAutoRejectsGenericWrappersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"({"items":[{"row_name":"alpha","Title":"Old"}]})"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"({"items":[{"row_name":"alpha","Title":"New"}]})"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("auto"));
	TestFalse(TEXT("generic wrapper auto detection fails"), Result.bSuccess);
	TestEqual(TEXT("generic wrapper auto failure uses InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestFalse(TEXT("no report is written on auto-detection failure"), Fixture.FileExists(ReportPath));
	return true;
}

bool FCortexDataJsonDiffAutoRejectsAmbiguousCanonicalShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString AmbiguousJson = TEXT(R"({"table_path":"/Game/Data/DT","rows":[],"string_table_path":"/Game/Data/ST","entries":[]})");
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), AmbiguousJson);
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), AmbiguousJson);
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("auto"));

	TestFalse(TEXT("ambiguous canonical auto detection fails"), Result.bSuccess);
	TestEqual(TEXT("ambiguous auto uses InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestFalse(TEXT("ambiguous auto writes no report"), Fixture.FileExists(ReportPath));
	return true;
}

bool FCortexDataJsonDiffModeOmittedDefaultsToAutoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	UDataTable* LeftTable = Fixture.CreateRegularDataTable(TEXT("DT_DefaultLeft"));
	UDataTable* RightTable = Fixture.CreateRegularDataTable(TEXT("DT_DefaultRight"));

	const FCortexCommandResult Result = Fixture.CompareJsonWithoutMode(
		Fixture.ExportDatatableJson(LeftTable, TEXT("left.json")),
		Fixture.ExportDatatableJson(RightTable, TEXT("right.json")),
		Fixture.MakeSavedPath(TEXT("report.json")));

	TestTrue(TEXT("omitted mode defaults to auto"), Result.bSuccess);
	TestTrue(TEXT("omitted mode returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("mode resolves to datatable_rows"), Result.Data->GetStringField(TEXT("mode")), TEXT("datatable_rows"));
	return true;
}

bool FCortexDataJsonDiffStringTableCanonicalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	UStringTable* LeftTable = Fixture.CreateStringTable(TEXT("ST_Left"));
	UStringTable* RightTable = Fixture.CreateStringTable(TEXT("ST_Right"));
	Fixture.UpdateStringTableValue(RightTable, TEXT("alpha.key"), TEXT("Alpha changed"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		Fixture.ExportStringTableJson(LeftTable, TEXT("left.json")),
		Fixture.ExportStringTableJson(RightTable, TEXT("right.json")),
		Fixture.MakeSavedPath(TEXT("report.json")),
		TEXT("auto"));

	TestTrue(TEXT("canonical string table compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("canonical string table compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("mode resolves to string_table_entries"), Result.Data->GetStringField(TEXT("mode")), TEXT("string_table_entries"));
	return true;
}

bool FCortexDataJsonDiffDataAssetsCanonicalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftJson = TEXT(R"({"data_assets":[{"path":"/Game/Data/DA_Quest.DA_Quest","name":"DA_Quest","asset_class":"CortexTestDataAsset","properties":{"TestNumber":26}}]})");
	const FString RightJson = TEXT(R"({"data_assets":[{"path":"/Game/Data/DA_Quest.DA_Quest","name":"DA_Quest","asset_class":"CortexTestDataAsset","properties":{"TestNumber":99}}]})");

	const FCortexCommandResult Result = Fixture.CompareJson(
		Fixture.WriteJsonFile(TEXT("left.json"), LeftJson),
		Fixture.WriteJsonFile(TEXT("right.json"), RightJson),
		Fixture.MakeSavedPath(TEXT("report.json")),
		TEXT("data_assets"));

	TestTrue(TEXT("canonical data asset compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("canonical data asset compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("mode stays data_assets"), Result.Data->GetStringField(TEXT("mode")), TEXT("data_assets"));
	TestEqual(
		TEXT("changed count is one"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		1);
	return true;
}

bool FCortexDataJsonDiffExplicitWrapperModesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"({"items":[{"id":"QuestA","Priority":26}]})"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"({"records":[{"id":"QuestA","Priority":28}]})"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("datatable_rows"), TEXT("id"));
	TestTrue(TEXT("explicit wrapper compare succeeds"), Result.bSuccess);
	return true;
}

bool FCortexDataJsonDiffExplicitKeyFieldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("datatable_rows"), TEXT("id"));
	TestTrue(TEXT("explicit key_field compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("explicit key_field compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("changed count is one"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		1);
	return true;
}

bool FCortexDataJsonDiffStringTableExplicitKeyFieldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"({"items":[{"id":"QuestA","source_string":"Old text","locale":"en"}]})"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"({"records":[{"id":"QuestA","source_string":"New text","locale":"en"}]})"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("string_table_entries"), TEXT("id"));
	TestTrue(TEXT("string table explicit key_field compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("string table explicit key_field compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("string table changed count is one"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		1);
	return true;
}

bool FCortexDataJsonDiffDataAssetsExplicitKeyFieldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Damage":5,"Category":"Quest"}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Damage":7,"Category":"Quest"}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("data_assets"), TEXT("id"));
	TestTrue(TEXT("data asset explicit key_field compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("data asset explicit key_field compare returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("data asset changed count is one"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		1);
	return true;
}

bool FCortexDataJsonDiffDuplicateKeysFailTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26},{"id":"QuestA","Priority":27}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(LeftPath, RightPath, ReportPath, TEXT("datatable_rows"), TEXT("id"));
	TestFalse(TEXT("duplicate keys fail"), Result.bSuccess);
	TestEqual(TEXT("duplicate keys use InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

bool FCortexDataJsonDiffIgnoreFieldsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26,"Title":"Stable"}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28,"Title":"Stable"}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"),
		TArray<FString>{ TEXT("Priority") });

	TestTrue(TEXT("ignore_fields compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("summary has shared fields"), Result.Data.IsValid()
		&& Result.Data->HasTypedField<EJson::Boolean>(TEXT("success"))
		&& Result.Data->HasTypedField<EJson::Array>(TEXT("files_written"))
		&& Result.Data->HasTypedField<EJson::Array>(TEXT("targets_touched"))
		&& Result.Data->HasTypedField<EJson::Object>(TEXT("counts")));
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("ignore_fields suppresses change"),
		static_cast<int32>(Result.Data->GetObjectField(TEXT("counts"))->GetNumberField(TEXT("changed"))),
		0);
	return true;
}

bool FCortexDataJsonDiffIncludeEqualTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Meta":{"b":2,"a":1}}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Meta":{"a":1,"b":3}}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"),
		TArray<FString>(),
		true);

	TestTrue(TEXT("include_equal compare succeeds"), Result.bSuccess);
	TestTrue(TEXT("include_equal returns data"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("counts include equal"), Result.Data->GetObjectField(TEXT("counts"))->HasTypedField<EJson::Number>(TEXT("equal")));

	const TArray<uint8> FirstReport = Fixture.ReadFileBytes(ReportPath);
	const FCortexCommandResult SecondResult = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"),
		TArray<FString>(),
		true);
	TestTrue(TEXT("second compare succeeds"), SecondResult.bSuccess);
	const TArray<uint8> SecondReport = Fixture.ReadFileBytes(ReportPath);
	TestEqual(TEXT("canonical report bytes are stable"), FirstReport, SecondReport);
	return true;
}

bool FCortexDataJsonDiffNullPresenceFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA"}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Meta":null}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"));

	TestTrue(TEXT("null presence compare succeeds"), Result.bSuccess);
	TSharedPtr<FJsonObject> Report = Fixture.ReadJsonFile(ReportPath);
	TestTrue(TEXT("report parses"), Report.IsValid());
	if (!Result.bSuccess || !Report.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>& Changed = Report->GetArrayField(TEXT("changed"));
	TestEqual(TEXT("one changed record is written"), Changed.Num(), 1);
	if (Changed.Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ChangedRecord = Changed[0]->AsObject();
	const TSharedPtr<FJsonObject> Fields = ChangedRecord->GetObjectField(TEXT("fields"));
	const TSharedPtr<FJsonObject> MetaDelta = Fields->GetObjectField(TEXT("Meta"));
	TestFalse(TEXT("missing left value reports not present"), MetaDelta->GetBoolField(TEXT("left_present")));
	TestTrue(TEXT("explicit null right value reports present"), MetaDelta->GetBoolField(TEXT("right_present")));
	TestFalse(TEXT("missing left omits left value"), MetaDelta->HasField(TEXT("left")));
	TestTrue(TEXT("explicit null right includes right value"), MetaDelta->HasField(TEXT("right")));
	return true;
}

bool FCortexDataJsonDiffMalformedJsonFailsWithoutReportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT("{"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"));

	TestFalse(TEXT("malformed JSON compare fails"), Result.bSuccess);
	TestEqual(TEXT("malformed JSON uses MALFORMED_JSON"), Result.ErrorCode, CortexErrorCodes::MalformedJson);
	TestFalse(TEXT("malformed JSON writes no report"), Fixture.FileExists(ReportPath));
	return true;
}

bool FCortexDataJsonDiffUnsafePathsFailTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));
	const FString ReportPath = FPaths::Combine(
		TEXT("Saved"),
		TEXT("CortexJsonDiffTests"),
		TEXT(".."),
		TEXT("TraversalEscape"),
		TEXT("report.json"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"));

	TestFalse(TEXT("unsafe path compare fails"), Result.bSuccess);
	TestEqual(TEXT("unsafe path uses INVALID_FILE_PATH"), Result.ErrorCode, CortexErrorCodes::InvalidFilePath);
	return true;
}

bool FCortexDataJsonDiffReportCollisionFailsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));

	const FCortexCommandResult Result = Fixture.CompareJson(
		LeftPath,
		RightPath,
		LeftPath,
		TEXT("datatable_rows"),
		TEXT("id"));

	TestFalse(TEXT("report collision compare fails"), Result.bSuccess);
	TestEqual(TEXT("report collision uses INVALID_FILE_PATH"), Result.ErrorCode, CortexErrorCodes::InvalidFilePath);
	return true;
}

bool FCortexDataJsonDiffFailureClearsStaleReportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataJsonDiffTestFixture Fixture;
	const FString LeftPath = Fixture.WriteJsonFile(TEXT("left.json"), TEXT(R"([{"id":"QuestA","Priority":26}])"));
	const FString RightPath = Fixture.WriteJsonFile(TEXT("right.json"), TEXT(R"([{"id":"QuestA","Priority":28}])"));
	const FString ReportPath = Fixture.MakeSavedPath(TEXT("report.json"));

	const FCortexCommandResult SuccessResult = Fixture.CompareJson(
		LeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"));
	TestTrue(TEXT("initial compare writes report"), SuccessResult.bSuccess);
	TestTrue(TEXT("initial report exists"), Fixture.FileExists(ReportPath));

	const FString BadLeftPath = Fixture.WriteJsonFile(TEXT("bad_left.json"), TEXT("{"));
	const FCortexCommandResult FailureResult = Fixture.CompareJson(
		BadLeftPath,
		RightPath,
		ReportPath,
		TEXT("datatable_rows"),
		TEXT("id"));

	TestFalse(TEXT("malformed rerun fails"), FailureResult.bSuccess);
	TestEqual(TEXT("malformed rerun uses MALFORMED_JSON"), FailureResult.ErrorCode, CortexErrorCodes::MalformedJson);
	TestFalse(TEXT("malformed rerun clears stale report"), Fixture.FileExists(ReportPath));
	return true;
}
