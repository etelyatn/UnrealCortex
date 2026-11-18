#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

// Tests that search_datatable_content matches against the row name itself,
// not only against field values. Regression for Issue 8.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSearchRowNameTest,
	"Cortex.Data.Search.RowNameIncluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSearchRowNameTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Handler;
	Handler.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
		MakeShared<FCortexDataCommandHandler>());

	// Step 1: list available datatables to find one with rows
	FCortexCommandResult ListTablesResult = Handler.Execute(TEXT("data.list_datatables"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_datatables should succeed"), ListTablesResult.bSuccess);

	if (!ListTablesResult.bSuccess || !ListTablesResult.Data.IsValid())
	{
		AddInfo(TEXT("list_datatables failed — skipping row name search test"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* TablesArray = nullptr;
	if (!ListTablesResult.Data->TryGetArrayField(TEXT("datatables"), TablesArray) || TablesArray == nullptr || TablesArray->Num() == 0)
	{
		AddInfo(TEXT("No DataTables found in editor — skipping row name search test"));
		return true;
	}

	// Step 2: find a table with at least one row
	FString TestTablePath;
	for (const TSharedPtr<FJsonValue>& TableVal : *TablesArray)
	{
		const TSharedPtr<FJsonObject>* TableObj = nullptr;
		if (!TableVal.IsValid() || !TableVal->TryGetObject(TableObj) || TableObj == nullptr)
		{
			continue;
		}
		double RowCount = 0.0;
		(*TableObj)->TryGetNumberField(TEXT("row_count"), RowCount);
		if (RowCount >= 1.0)
		{
			(*TableObj)->TryGetStringField(TEXT("path"), TestTablePath);
			break;
		}
	}

	if (TestTablePath.IsEmpty())
	{
		AddInfo(TEXT("No DataTable with rows found — skipping row name search test"));
		return true;
	}

	// Step 3: query the table to get an actual row name
	TSharedPtr<FJsonObject> QueryParams = MakeShared<FJsonObject>();
	QueryParams->SetStringField(TEXT("table_path"), TestTablePath);
	QueryParams->SetNumberField(TEXT("limit"), 1);
	FCortexCommandResult QueryResult = Handler.Execute(TEXT("data.query_datatable"), QueryParams);

	if (!QueryResult.bSuccess || !QueryResult.Data.IsValid())
	{
		AddInfo(TEXT("query_datatable failed — skipping row name search test"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
	if (!QueryResult.Data->TryGetArrayField(TEXT("rows"), RowsArray) || RowsArray == nullptr || RowsArray->Num() == 0)
	{
		AddInfo(TEXT("No rows returned from query — skipping row name search test"));
		return true;
	}

	FString FirstRowName;
	const TSharedPtr<FJsonObject>* FirstRowObj = nullptr;
	if (!(*RowsArray)[0].IsValid() || !(*RowsArray)[0]->TryGetObject(FirstRowObj) || FirstRowObj == nullptr)
	{
		AddInfo(TEXT("Could not access first row object — skipping"));
		return true;
	}
	if (!(*FirstRowObj)->TryGetStringField(TEXT("row_name"), FirstRowName) || FirstRowName.IsEmpty())
	{
		AddInfo(TEXT("Could not get first row name — skipping"));
		return true;
	}

	// Step 4: search using the exact row name — must return at least 1 match
	// This would fail before the fix if the row name doesn't appear in any field value.
	TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("table_path"), TestTablePath);
	SearchParams->SetStringField(TEXT("search_text"), FirstRowName);

	FCortexCommandResult SearchResult = Handler.Execute(TEXT("data.search_datatable_content"), SearchParams);
	TestTrue(TEXT("search_datatable_content should succeed"), SearchResult.bSuccess);

	if (SearchResult.Data.IsValid())
	{
		double TotalMatches = 0.0;
		SearchResult.Data->TryGetNumberField(TEXT("total_matches"), TotalMatches);
		TestTrue(
			FString::Printf(TEXT("Searching for row name '%s' should return at least 1 match"), *FirstRowName),
			TotalMatches >= 1.0
		);

		// Step 5: verify that at least one result has a "row_name" match entry
		// (confirms the fix specifically, not just a coincidental field-value match)
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr)
		{
			bool bFoundRowNameMatch = false;
			for (const TSharedPtr<FJsonValue>& ResultVal : *ResultsArray)
			{
				const TSharedPtr<FJsonObject>* ResultObj = nullptr;
				if (!ResultVal.IsValid() || !ResultVal->TryGetObject(ResultObj) || ResultObj == nullptr)
				{
					continue;
				}

				FString ReturnedRowName;
				(*ResultObj)->TryGetStringField(TEXT("row_name"), ReturnedRowName);
				if (!ReturnedRowName.Equals(FirstRowName, ESearchCase::IgnoreCase))
				{
					continue;
				}

				// Check the matches array for a "row_name" field entry
				const TArray<TSharedPtr<FJsonValue>>* MatchesArray = nullptr;
				if (!(*ResultObj)->TryGetArrayField(TEXT("matches"), MatchesArray) || MatchesArray == nullptr)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& MatchVal : *MatchesArray)
				{
					const TSharedPtr<FJsonObject>* MatchObj = nullptr;
					if (!MatchVal.IsValid() || !MatchVal->TryGetObject(MatchObj) || MatchObj == nullptr)
					{
						continue;
					}

					FString FieldName;
					(*MatchObj)->TryGetStringField(TEXT("field"), FieldName);
					if (FieldName.Equals(TEXT("row_name"), ESearchCase::IgnoreCase))
					{
						bFoundRowNameMatch = true;
						break;
					}
				}

				if (bFoundRowNameMatch)
				{
					break;
				}
			}

			TestTrue(
				FString::Printf(TEXT("At least one result for '%s' should have a 'row_name' match entry"), *FirstRowName),
				bFoundRowNameMatch
			);
		}
	}

	return true;
}

// search_text (or its "query" alias) is a required parameter. The gate runs before the table is
// loaded, so a path that does not exist still exercises it, and the refusal is a missing field
// rather than an invalid value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSearchMissingSearchTextTest,
	"Cortex.Data.Search.MissingSearchText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSearchMissingSearchTextTest::RunTest(const FString& Parameters)
{
	FCortexDataCommandHandler Handler;
	const FString TablePath = FString::Printf(
		TEXT("/Game/Temp/DT_MissingSearchText_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));

	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("table_path"), TablePath);
	const FCortexCommandResult Missing = Handler.Execute(TEXT("search_datatable_content"), MissingParams);
	TestFalse(TEXT("search without search_text is refused"), Missing.bSuccess);
	TestEqual(TEXT("search without search_text uses INVALID_FIELD"), Missing.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	EmptyParams->SetStringField(TEXT("table_path"), TablePath);
	EmptyParams->SetStringField(TEXT("search_text"), FString());
	const FCortexCommandResult Empty = Handler.Execute(TEXT("search_datatable_content"), EmptyParams);
	TestFalse(TEXT("search with an empty search_text is refused"), Empty.bSuccess);
	TestEqual(TEXT("search with an empty search_text uses INVALID_FIELD"), Empty.ErrorCode, CortexErrorCodes::InvalidField);

	// The alias still satisfies the gate: what follows is the missing table, not the missing field.
	TSharedPtr<FJsonObject> AliasParams = MakeShared<FJsonObject>();
	AliasParams->SetStringField(TEXT("table_path"), TablePath);
	AliasParams->SetStringField(TEXT("query"), TEXT("needle"));
	const FCortexCommandResult Alias = Handler.Execute(TEXT("search_datatable_content"), AliasParams);
	TestFalse(TEXT("search through the query alias reaches the table load"), Alias.bSuccess);
	TestEqual(TEXT("the query alias is not reported as a missing field"), Alias.ErrorCode, CortexErrorCodes::TableNotFound);

	return true;
}
