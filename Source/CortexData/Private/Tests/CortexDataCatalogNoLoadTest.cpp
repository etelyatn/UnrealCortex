#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Operations/CortexDataTableOps.h"
#include "UObject/Package.h"

namespace
{
	// Stable project content (Content/Data) is the fixture: these commands must enumerate it
	// from registry data without loading it. A save/unload fixture proved nondeterministic
	// here — the editor's async content validation and registry event queue reload freshly
	// created StringTables and drop freshly unloaded registry entries on their own schedule,
	// independent of the command under test. If an asset is already in memory when the test
	// runs, the no-load verification honestly skips via AddInfo.
	constexpr const TCHAR* ProjectTablePackage = TEXT("/Game/Data/DT_TestSimple");
	constexpr const TCHAR* ProjectTablePath = TEXT("/Game/Data/DT_TestSimple.DT_TestSimple");
	constexpr const TCHAR* ProjectStringTablePackage = TEXT("/Game/Data/ST_TestStrings");
	constexpr const TCHAR* ProjectStringTablePath = TEXT("/Game/Data/ST_TestStrings.ST_TestStrings");

	const TSharedPtr<FJsonObject>* FindEntryByPath(
		const TSharedPtr<FJsonObject>& Data,
		const FString& ArrayField,
		const FString& EntryPath)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(ArrayField, Entries) || Entries == nullptr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			FString Path;
			if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject != nullptr
				&& (*EntryObject)->TryGetStringField(TEXT("path"), Path) && Path == EntryPath)
			{
				return EntryObject;
			}
		}
		return nullptr;
	}
}

// list_datatables must not load every DataTable in the project by default (issue #224 item 3):
// row_count is opt-in via include_row_count. Test names sort before the pre-existing TCP
// contract tests (Cortex.Data.GetDataCatalog, Cortex.Data.ListDatatables), which pass
// include_row_count=true and would leave the project tables loaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexListDatatablesNoLoadDefaultTest,
	"Cortex.Data.CatalogNoLoad.ListDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexListDatatablesNoLoadDefaultTest::RunTest(const FString& Parameters)
{
	// The response-shape assertion always runs; only the unload check needs the precondition
	// that the fixture started out of memory.
	const bool bTableInMemory = FindPackage(nullptr, ProjectTablePackage) != nullptr;
	if (bTableInMemory)
	{
		AddInfo(TEXT("Project test table already in memory; skipping the unload assertion only."));
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FCortexCommandResult Result = FCortexDataTableOps::ListDatatables(Params);

	if (!TestTrue(TEXT("list_datatables should succeed"), Result.bSuccess) || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Entry = FindEntryByPath(Result.Data, TEXT("datatables"), ProjectTablePath);
	if (TestNotNull(TEXT("Project table listed"), Entry))
	{
		TestFalse(TEXT("Default list omits row_count"), (*Entry)->HasField(TEXT("row_count")));
		TestTrue(TEXT("Default list still reports is_composite"), (*Entry)->HasField(TEXT("is_composite")));
	}

	if (!bTableInMemory)
	{
		TestNull(TEXT("Project table stays unloaded"), FindPackage(nullptr, ProjectTablePackage));
	}
	return true;
}

// include_row_count=true keeps the full contract: the table is loaded and its row count reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexListDatatablesNoLoadIncludeRowCountTest,
	"Cortex.Data.CatalogNoLoad.ListIncludeRowCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexListDatatablesNoLoadIncludeRowCountTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetBoolField(TEXT("include_row_count"), true);
	const FCortexCommandResult Result = FCortexDataTableOps::ListDatatables(Params);

	if (!TestTrue(TEXT("list_datatables should succeed"), Result.bSuccess) || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Entry = FindEntryByPath(Result.Data, TEXT("datatables"), ProjectTablePath);
	if (TestNotNull(TEXT("Project table listed"), Entry))
	{
		TestTrue(TEXT("include_row_count reports the row count"), (*Entry)->HasField(TEXT("row_count")));
	}
	return true;
}

// get_data_catalog must not load every DataTable and StringTable in the project by default
// (issue #224 item 4): row_count and entry_count are opt-in via include_row_count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCatalogNoLoadDefaultTest,
	"Cortex.Data.CatalogNoLoad.Catalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCatalogNoLoadDefaultTest::RunTest(const FString& Parameters)
{
	const bool bTableInMemory = FindPackage(nullptr, ProjectTablePackage) != nullptr;
	const bool bStringTableInMemory = FindPackage(nullptr, ProjectStringTablePackage) != nullptr;
	if (bTableInMemory || bStringTableInMemory)
	{
		AddInfo(TEXT("A project test asset is already in memory; only its unload assertion is skipped."));
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Sections;
	Sections.Add(MakeShared<FJsonValueString>(TEXT("datatables")));
	Sections.Add(MakeShared<FJsonValueString>(TEXT("string_tables")));
	Params->SetArrayField(TEXT("sections"), Sections);
	const FCortexCommandResult Result = FCortexDataTableOps::GetDataCatalog(Params);

	if (!TestTrue(TEXT("get_data_catalog should succeed"), Result.bSuccess) || !Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* TableEntry = FindEntryByPath(Result.Data, TEXT("datatables"), ProjectTablePath);
	if (TestNotNull(TEXT("Project table catalogued"), TableEntry))
	{
		TestFalse(TEXT("Default catalog omits row_count"), (*TableEntry)->HasField(TEXT("row_count")));
	}
	if (!bTableInMemory)
	{
		TestNull(TEXT("Project table stays unloaded"), FindPackage(nullptr, ProjectTablePackage));
	}

	const TSharedPtr<FJsonObject>* StringTableEntry =
		FindEntryByPath(Result.Data, TEXT("string_tables"), ProjectStringTablePath);
	if (TestNotNull(TEXT("Project StringTable catalogued"), StringTableEntry))
	{
		TestFalse(TEXT("Default catalog omits entry_count"), (*StringTableEntry)->HasField(TEXT("entry_count")));
	}
	if (!bStringTableInMemory)
	{
		TestNull(TEXT("Project StringTable stays unloaded"), FindPackage(nullptr, ProjectStringTablePackage));
	}

	return true;
}
