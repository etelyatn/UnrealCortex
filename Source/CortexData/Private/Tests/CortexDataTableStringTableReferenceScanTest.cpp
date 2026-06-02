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
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.title"), TEXT("Fireball"));
	TestStringTable->GetMutableStringTable()->SetSourceString(TEXT("entry.fireball.step_0"), TEXT("Charge flame."));

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
