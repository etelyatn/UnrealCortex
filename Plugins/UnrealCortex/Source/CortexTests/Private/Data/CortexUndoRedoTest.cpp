
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Editor.h"
#include "ScopedTransaction.h"

// ============================================================================
// Test: Verify FScopedTransaction works on DataTable directly (baseline)
// This proves the pattern: FScopedTransaction + Modify() + mutation = undoable.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUndoDirectTest,
	"Cortex.Data.Undo.DirectTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUndoDirectTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddWarning(TEXT("Editor undo system not available"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Direct Test Setup")));

	UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUndoDirectTest"));
	UDataTable* TestTable = NewObject<UDataTable>(TestPackage, TEXT("DT_UndoDirectTest"), RF_Public | RF_Standalone | RF_Transactional);
	TestTable->RowStruct = FTableRowBase::StaticStruct();

	TestEqual(TEXT("Start with 0 rows"), TestTable->GetRowMap().Num(), 0);

	// Manually create a transaction (same pattern as our production code)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Test Add Row")));
		TestTable->Modify();

		uint8* RowMemory = static_cast<uint8*>(FMemory::Malloc(FTableRowBase::StaticStruct()->GetStructureSize()));
		FTableRowBase::StaticStruct()->InitializeStruct(RowMemory);
		TestTable->AddRow(FName(TEXT("DirectRow")), RowMemory, FTableRowBase::StaticStruct());
		FTableRowBase::StaticStruct()->DestroyStruct(RowMemory);
		FMemory::Free(RowMemory);
	}

	TestEqual(TEXT("Should have 1 row after add"), TestTable->GetRowMap().Num(), 1);

	bool bUndone = GEditor->UndoTransaction();
	TestTrue(TEXT("Direct undo should succeed"), bUndone);
	TestEqual(TEXT("Should have 0 rows after undo"), TestTable->GetRowMap().Num(), 0);

	bool bRedone = GEditor->RedoTransaction();
	TestTrue(TEXT("Redo should succeed"), bRedone);
	TestEqual(TEXT("Should have 1 row after redo"), TestTable->GetRowMap().Num(), 1);

	// Cleanup
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Direct Test Cleanup")));

	return true;
}

// ============================================================================
// Test: Undo after add_datatable_row via command handler
// Proves end-to-end: MCP command -> FScopedTransaction -> undo works.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUndoAddRowTest,
	"Cortex.Data.Undo.AddRowViaCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUndoAddRowTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddWarning(TEXT("Editor undo system not available"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex AddRow Test Setup")));

	UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUndoAddRowTest"));
	UDataTable* TestTable = NewObject<UDataTable>(TestPackage, TEXT("DT_UndoAddRowTest"), RF_Public | RF_Standalone | RF_Transactional);
	TestTable->RowStruct = FTableRowBase::StaticStruct();

	const FString TablePath = TestTable->GetPathName();

	TestEqual(TEXT("Table should start empty"), TestTable->GetRowMap().Num(), 0);

	// Add a row via command handler (which wraps in FScopedTransaction internally)
	FCortexCommandRouter Handler;
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("table_path"), TablePath);
	AddParams->SetStringField(TEXT("row_name"), TEXT("TestRow_1"));
	AddParams->SetObjectField(TEXT("row_data"), MakeShared<FJsonObject>());

	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_datatable_row"), AddParams);
	TestTrue(TEXT("Add row should succeed"), AddResult.bSuccess);
	TestEqual(TEXT("Table should have 1 row after add"), TestTable->GetRowMap().Num(), 1);

	// Undo the add operation
	bool bUndone = GEditor->UndoTransaction();
	TestTrue(TEXT("Undo should succeed"), bUndone);
	TestEqual(TEXT("Table should have 0 rows after undo"), TestTable->GetRowMap().Num(), 0);

	// Redo should bring the row back
	bool bRedone = GEditor->RedoTransaction();
	TestTrue(TEXT("Redo should succeed"), bRedone);
	TestEqual(TEXT("Table should have 1 row after redo"), TestTable->GetRowMap().Num(), 1);

	// Cleanup
	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex AddRow Test Cleanup")));

	return true;
}
