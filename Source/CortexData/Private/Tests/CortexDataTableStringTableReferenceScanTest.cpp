#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexDataTableOps.h"
#include "CortexDataLocalizationTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"

namespace
{
	TSharedPtr<FJsonObject> MakeImportRowEntry(const FString& RowName, const FString& Title)
	{
		TSharedPtr<FJsonObject> RowData = MakeShared<FJsonObject>();
		RowData->SetStringField(TEXT("Title"), Title);

		TSharedPtr<FJsonObject> RowEntry = MakeShared<FJsonObject>();
		RowEntry->SetStringField(TEXT("row_name"), RowName);
		RowEntry->SetObjectField(TEXT("row_data"), RowData);
		return RowEntry;
	}

	TSharedPtr<FJsonObject> MakeImportParams(UDataTable* DataTable, const FString& Mode, const TArray<TSharedPtr<FJsonValue>>& Rows, bool bDryRun = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), DataTable->GetPathName());
		Params->SetStringField(TEXT("mode"), Mode);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetArrayField(TEXT("rows"), Rows);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableStringTableReferenceScanNestedArrayTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.NestedArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableStringTableReferenceScanNestedArrayTest::RunTest(const FString& Parameters)
{
	UStringTable* TestStringTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("ST_CortexDataReferenceScanTest")));
	TestStringTable->GetMutableStringTable()->SetNamespace(TEXT("CortexDataReferenceScanTest"));
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Fireball"), TEXT(""));
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.step_0"), TEXT("Charge flame."), TEXT(""));

	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataReferenceScanTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	FCortexDataLocalizationTestRow Row;
	Row.Title = FText::FromStringTable(TestStringTable->GetStringTableId(), TEXT("entry.fireball.title"));
	FCortexDataLocalizationStepTestRow Step;
	Step.Description = FText::FromStringTable(TestStringTable->GetStringTableId(), TEXT("entry.fireball.step_0"));
	Row.Steps.Add(Step);
	DataTable->AddRow(TEXT("fireball"), Row);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), DataTable->GetPathName());
	Params->SetStringField(TEXT("search_mode"), TEXT("string_table_refs"));
	Params->SetStringField(TEXT("string_table_path"), TestStringTable->GetStringTableId().ToString());
	Params->SetStringField(TEXT("key_pattern"), TEXT("entry.*"));
	Params->SetNumberField(TEXT("limit"), 10);

	const FCortexCommandResult Result = FCortexDataTableOps::SearchDatatableContent(Params);
	TestTrue(TEXT("Reference scan should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result payload should be valid"), Result.Data.IsValid());
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestStringTable->MarkAsGarbage();
		DataTable->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	TestTrue(TEXT("Results array exists"), Result.Data->TryGetArrayField(TEXT("results"), Results));
	TestEqual(TEXT("Two references found"), Results ? Results->Num() : 0, 2);

	bool bFoundNestedStep = false;
	if (Results != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& EntryValue : *Results)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			if ((*Entry)->GetStringField(TEXT("field_path")) == TEXT("Steps[0].Description")
				&& (*Entry)->GetStringField(TEXT("key")) == TEXT("entry.fireball.step_0"))
			{
				bFoundNestedStep = true;
			}
		}
	}
	TestTrue(TEXT("Nested array FText path should be reported"), bFoundNestedStep);

	TestStringTable->MarkAsGarbage();
	DataTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableImportDryRunCreateSkipsDuplicateIncomingRowsTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.ImportDryRunCreateSkipsDuplicateIncomingRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableImportDryRunCreateSkipsDuplicateIncomingRowsTest::RunTest(const FString& Parameters)
{
	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataImportDryRunCreateDuplicateTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("First"))));
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("Second"))));

	const FCortexCommandResult Result = FCortexDataTableOps::ImportDatatableJson(
		MakeImportParams(DataTable, TEXT("create"), Rows, true));
	TestTrue(TEXT("Duplicate dry-run create import should succeed"), Result.bSuccess);
	TestTrue(TEXT("Duplicate dry-run create import should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("Dry-run should count first duplicate as create"), static_cast<int32>(Result.Data->GetNumberField(TEXT("created"))), 1);
		TestEqual(TEXT("Dry-run should count second duplicate as skipped"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 1);
	}
	TestNull(TEXT("Dry-run create should not mutate table"), DataTable->FindRowUnchecked(TEXT("fireball")));

	DataTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableImportDryRunUpsertCountsDuplicateIncomingRowsTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.ImportDryRunUpsertCountsDuplicateIncomingRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableImportDryRunUpsertCountsDuplicateIncomingRowsTest::RunTest(const FString& Parameters)
{
	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataImportDryRunUpsertDuplicateTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("First"))));
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("Second"))));

	const FCortexCommandResult Result = FCortexDataTableOps::ImportDatatableJson(
		MakeImportParams(DataTable, TEXT("upsert"), Rows, true));
	TestTrue(TEXT("Duplicate dry-run upsert import should succeed"), Result.bSuccess);
	TestTrue(TEXT("Duplicate dry-run upsert import should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("Dry-run should count first duplicate as create"), static_cast<int32>(Result.Data->GetNumberField(TEXT("created"))), 1);
		TestEqual(TEXT("Dry-run should count second duplicate as update"), static_cast<int32>(Result.Data->GetNumberField(TEXT("updated"))), 1);
	}
	TestNull(TEXT("Dry-run upsert should not mutate table"), DataTable->FindRowUnchecked(TEXT("fireball")));

	DataTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableImportCreateSkipsDuplicateIncomingRowsTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.ImportCreateSkipsDuplicateIncomingRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableImportCreateSkipsDuplicateIncomingRowsTest::RunTest(const FString& Parameters)
{
	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataImportCreateDuplicateTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("First"))));
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("Second"))));

	const FCortexCommandResult Result = FCortexDataTableOps::ImportDatatableJson(
		MakeImportParams(DataTable, TEXT("create"), Rows));
	TestTrue(TEXT("Duplicate create import should succeed"), Result.bSuccess);
	TestTrue(TEXT("Duplicate create import should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("Only first duplicate should be created"), static_cast<int32>(Result.Data->GetNumberField(TEXT("created"))), 1);
		TestEqual(TEXT("Second duplicate should be skipped"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 1);
	}

	const uint8* RowPtr = DataTable->FindRowUnchecked(TEXT("fireball"));
	TestNotNull(TEXT("Created row should exist"), RowPtr);
	if (RowPtr != nullptr)
	{
		const FCortexDataLocalizationTestRow* Row = reinterpret_cast<const FCortexDataLocalizationTestRow*>(RowPtr);
		TestEqual(TEXT("Create mode should preserve first duplicate row"), Row->Title.ToString(), TEXT("First"));
	}

	DataTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableImportUpsertCountsDuplicateIncomingRowsTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.ImportUpsertCountsDuplicateIncomingRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableImportUpsertCountsDuplicateIncomingRowsTest::RunTest(const FString& Parameters)
{
	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataImportUpsertDuplicateTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("First"))));
	Rows.Add(MakeShared<FJsonValueObject>(MakeImportRowEntry(TEXT("fireball"), TEXT("Second"))));

	const FCortexCommandResult Result = FCortexDataTableOps::ImportDatatableJson(
		MakeImportParams(DataTable, TEXT("upsert"), Rows));
	TestTrue(TEXT("Duplicate upsert import should succeed"), Result.bSuccess);
	TestTrue(TEXT("Duplicate upsert import should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("First duplicate should be created"), static_cast<int32>(Result.Data->GetNumberField(TEXT("created"))), 1);
		TestEqual(TEXT("Second duplicate should be counted as update"), static_cast<int32>(Result.Data->GetNumberField(TEXT("updated"))), 1);
	}

	const uint8* RowPtr = DataTable->FindRowUnchecked(TEXT("fireball"));
	TestNotNull(TEXT("Upserted row should exist"), RowPtr);
	if (RowPtr != nullptr)
	{
		const FCortexDataLocalizationTestRow* Row = reinterpret_cast<const FCortexDataLocalizationTestRow*>(RowPtr);
		TestEqual(TEXT("Upsert mode should apply the last duplicate row"), Row->Title.ToString(), TEXT("Second"));
	}

	DataTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataTableImportRejectsMalformedNestedArrayBeforeReplaceTest,
	"Cortex.Data.Datatable.StringTableReferenceScan.ImportRejectsMalformedNestedArrayBeforeReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataTableImportRejectsMalformedNestedArrayBeforeReplaceTest::RunTest(const FString& Parameters)
{
	UStringTable* TestStringTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("ST_CortexDataImportRejectTest")));
	TestStringTable->GetMutableStringTable()->SetNamespace(TEXT("CortexDataImportRejectTest"));
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Fireball"), TEXT(""));
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.step_0"), TEXT("Charge flame."), TEXT(""));

	UDataTable* DataTable = NewObject<UDataTable>(
		GetTransientPackage(),
		FName(TEXT("DT_CortexDataImportRejectTest")));
	DataTable->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

	FCortexDataLocalizationTestRow ExistingRow;
	ExistingRow.Title = FText::FromStringTable(TestStringTable->GetStringTableId(), TEXT("entry.fireball.title"));
	FCortexDataLocalizationStepTestRow ExistingStep;
	ExistingStep.Description = FText::FromStringTable(TestStringTable->GetStringTableId(), TEXT("entry.fireball.step_0"));
	ExistingRow.Steps.Add(ExistingStep);
	DataTable->AddRow(TEXT("fireball"), ExistingRow);

	TSharedPtr<FJsonObject> RowData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> MalformedSteps;
	MalformedSteps.Add(MakeShared<FJsonValueString>(TEXT("not a struct object")));
	RowData->SetArrayField(TEXT("Steps"), MalformedSteps);

	TSharedPtr<FJsonObject> RowEntry = MakeShared<FJsonObject>();
	RowEntry->SetStringField(TEXT("row_name"), TEXT("fireball"));
	RowEntry->SetObjectField(TEXT("row_data"), RowData);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), DataTable->GetPathName());
	Params->SetStringField(TEXT("mode"), TEXT("replace"));
	Params->SetBoolField(TEXT("dry_run"), false);
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(RowEntry));
	Params->SetArrayField(TEXT("rows"), Rows);

	const FCortexCommandResult Result = FCortexDataTableOps::ImportDatatableJson(Params);
	TestFalse(TEXT("Malformed nested array import should fail"), Result.bSuccess);

	const uint8* RowPtr = DataTable->FindRowUnchecked(TEXT("fireball"));
	TestNotNull(TEXT("Existing row should remain after failed replace import"), RowPtr);
	if (RowPtr != nullptr)
	{
		const FCortexDataLocalizationTestRow* PreservedRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(RowPtr);
		TestEqual(TEXT("Existing nested array should remain intact"), PreservedRow->Steps.Num(), 1);
		TestEqual(TEXT("Existing title should remain intact"), PreservedRow->Title.ToString(), TEXT("Fireball"));
	}

	TestStringTable->MarkAsGarbage();
	DataTable->MarkAsGarbage();
	return true;
}
