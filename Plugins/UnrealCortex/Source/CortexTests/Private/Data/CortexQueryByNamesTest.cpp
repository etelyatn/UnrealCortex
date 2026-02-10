
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBQueryByNamesTest,
	"UDB.Commands.QueryByNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBQueryByNamesTest::RunTest(const FString& Parameters)
{
	FUDBCommandHandler Handler;

	// --- Test 1: query_datatable with row_names returns only requested rows ---
	{
		// First, query all rows to discover some valid row names
		TSharedPtr<FJsonObject> DiscoverParams = MakeShared<FJsonObject>();
		DiscoverParams->SetStringField(TEXT("table_path"), TEXT("/Script/Ripper"));

		// Use list_datatables to find a real table
		FUDBCommandResult ListResult = Handler.Execute(TEXT("list_datatables"), MakeShared<FJsonObject>());
		TestTrue(TEXT("list_datatables should succeed"), ListResult.bSuccess);

		if (!ListResult.bSuccess || !ListResult.Data.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tables = nullptr;
		if (!ListResult.Data->TryGetArrayField(TEXT("datatables"), Tables) || Tables == nullptr || Tables->Num() == 0)
		{
			AddWarning(TEXT("No DataTables loaded in editor - skipping query_by_names test"));
			return true;
		}

		// Find a table with at least 2 rows
		FString TestTablePath;
		for (const TSharedPtr<FJsonValue>& TableVal : *Tables)
		{
			const TSharedPtr<FJsonObject>* TableObj = nullptr;
			if (!TableVal.IsValid() || !TableVal->TryGetObject(TableObj) || TableObj == nullptr)
			{
				continue;
			}
			double RowCount = 0.0;
			(*TableObj)->TryGetNumberField(TEXT("row_count"), RowCount);
			if (RowCount >= 2.0)
			{
				(*TableObj)->TryGetStringField(TEXT("path"), TestTablePath);
				break;
			}
		}

		if (TestTablePath.IsEmpty())
		{
			AddWarning(TEXT("No DataTable with 2+ rows found - skipping query_by_names test"));
			return true;
		}

		// Get the first 3 row names from that table
		TSharedPtr<FJsonObject> QueryParams = MakeShared<FJsonObject>();
		QueryParams->SetStringField(TEXT("table_path"), TestTablePath);
		QueryParams->SetNumberField(TEXT("limit"), 3);
		FUDBCommandResult QueryResult = Handler.Execute(TEXT("query_datatable"), QueryParams);
		TestTrue(TEXT("Initial query should succeed"), QueryResult.bSuccess);

		if (!QueryResult.bSuccess || !QueryResult.Data.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!QueryResult.Data->TryGetArrayField(TEXT("rows"), Rows) || Rows == nullptr || Rows->Num() < 2)
		{
			AddWarning(TEXT("Need at least 2 rows for test"));
			return true;
		}

		// Extract row names
		TArray<FString> KnownNames;
		for (const TSharedPtr<FJsonValue>& RowVal : *Rows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (RowVal.IsValid() && RowVal->TryGetObject(RowObj) && RowObj != nullptr)
			{
				FString Name;
				if ((*RowObj)->TryGetStringField(TEXT("row_name"), Name))
				{
					KnownNames.Add(Name);
				}
			}
		}

		TestTrue(TEXT("Should have at least 2 known row names"), KnownNames.Num() >= 2);

		// --- Test 2: Query with row_names returns exactly those rows in order ---
		{
			TSharedPtr<FJsonObject> NamedParams = MakeShared<FJsonObject>();
			NamedParams->SetStringField(TEXT("table_path"), TestTablePath);

			TArray<TSharedPtr<FJsonValue>> RowNamesArray;
			// Request in reverse order
			for (int32 i = KnownNames.Num() - 1; i >= 0; --i)
			{
				RowNamesArray.Add(MakeShared<FJsonValueString>(KnownNames[i]));
			}
			NamedParams->SetArrayField(TEXT("row_names"), RowNamesArray);

			FUDBCommandResult NamedResult = Handler.Execute(TEXT("query_datatable"), NamedParams);
			TestTrue(TEXT("query_datatable with row_names should succeed"), NamedResult.bSuccess);

			if (NamedResult.bSuccess && NamedResult.Data.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* NamedRows = nullptr;
				if (NamedResult.Data->TryGetArrayField(TEXT("rows"), NamedRows) && NamedRows != nullptr)
				{
					TestEqual(TEXT("Should return exactly requested number of rows"),
						NamedRows->Num(), KnownNames.Num());

					// Verify order matches request (reversed)
					for (int32 i = 0; i < NamedRows->Num(); ++i)
					{
						const TSharedPtr<FJsonObject>* RowObj = nullptr;
						if ((*NamedRows)[i].IsValid() && (*NamedRows)[i]->TryGetObject(RowObj) && RowObj != nullptr)
						{
							FString ReturnedName;
							(*RowObj)->TryGetStringField(TEXT("row_name"), ReturnedName);
							int32 ExpectedIdx = KnownNames.Num() - 1 - i;
							TestEqual(
								FString::Printf(TEXT("Row %d should match requested order"), i),
								ReturnedName, KnownNames[ExpectedIdx]
							);
						}
					}
				}
			}
		}

		// --- Test 3: row_names with a nonexistent name reports missing_rows ---
		{
			TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
			MissingParams->SetStringField(TEXT("table_path"), TestTablePath);

			TArray<TSharedPtr<FJsonValue>> MixedNames;
			MixedNames.Add(MakeShared<FJsonValueString>(KnownNames[0]));
			MixedNames.Add(MakeShared<FJsonValueString>(TEXT("NonExistentRow_XYZ_12345")));
			MissingParams->SetArrayField(TEXT("row_names"), MixedNames);

			FUDBCommandResult MissingResult = Handler.Execute(TEXT("query_datatable"), MissingParams);
			TestTrue(TEXT("query_datatable with missing names should still succeed"), MissingResult.bSuccess);

			if (MissingResult.bSuccess && MissingResult.Data.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* ReturnedRows = nullptr;
				MissingResult.Data->TryGetArrayField(TEXT("rows"), ReturnedRows);
				TestTrue(TEXT("Should have rows array"), ReturnedRows != nullptr);
				if (ReturnedRows != nullptr)
				{
					TestEqual(TEXT("Should return only the existing row"), ReturnedRows->Num(), 1);
				}

				const TArray<TSharedPtr<FJsonValue>>* MissingRows = nullptr;
				if (MissingResult.Data->TryGetArrayField(TEXT("missing_rows"), MissingRows) && MissingRows != nullptr)
				{
					TestEqual(TEXT("Should report 1 missing row"), MissingRows->Num(), 1);
					if (MissingRows->Num() > 0)
					{
						FString MissingName;
						(*MissingRows)[0]->TryGetString(MissingName);
						TestEqual(TEXT("Missing row name should match"), MissingName, FString(TEXT("NonExistentRow_XYZ_12345")));
					}
				}
				else
				{
					AddError(TEXT("Response should have missing_rows array"));
				}
			}
		}

		// --- Test 4: row_names with fields projection ---
		{
			TSharedPtr<FJsonObject> ProjParams = MakeShared<FJsonObject>();
			ProjParams->SetStringField(TEXT("table_path"), TestTablePath);

			TArray<TSharedPtr<FJsonValue>> SingleName;
			SingleName.Add(MakeShared<FJsonValueString>(KnownNames[0]));
			ProjParams->SetArrayField(TEXT("row_names"), SingleName);

			// Get schema to find a field name
			TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
			SchemaParams->SetStringField(TEXT("table_path"), TestTablePath);
			FUDBCommandResult SchemaResult = Handler.Execute(TEXT("get_datatable_schema"), SchemaParams);

			if (SchemaResult.bSuccess && SchemaResult.Data.IsValid())
			{
				const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
				if (SchemaResult.Data->TryGetObjectField(TEXT("schema"), SchemaObj) && SchemaObj != nullptr)
				{
					const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
					if ((*SchemaObj)->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr != nullptr && FieldsArr->Num() > 0)
					{
						// Get first field name
						const TSharedPtr<FJsonObject>* FieldObj = nullptr;
						if ((*FieldsArr)[0].IsValid() && (*FieldsArr)[0]->TryGetObject(FieldObj) && FieldObj != nullptr)
						{
							FString FirstFieldName;
							(*FieldObj)->TryGetStringField(TEXT("name"), FirstFieldName);

							if (!FirstFieldName.IsEmpty())
							{
								TArray<TSharedPtr<FJsonValue>> FieldsProjection;
								FieldsProjection.Add(MakeShared<FJsonValueString>(FirstFieldName));
								ProjParams->SetArrayField(TEXT("fields"), FieldsProjection);

								FUDBCommandResult ProjResult = Handler.Execute(TEXT("query_datatable"), ProjParams);
								TestTrue(TEXT("query with row_names + fields should succeed"), ProjResult.bSuccess);
							}
						}
					}
				}
			}
		}
	}

	return true;
}
