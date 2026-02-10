
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBResolveTagsTest,
	"UDB.Commands.ResolveTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBResolveTagsTest::RunTest(const FString& Parameters)
{
	FUDBCommandHandler Handler;

	// --- Test 1: Missing required params ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FUDBCommandResult Result = Handler.Execute(TEXT("resolve_tags"), Params);
		TestFalse(TEXT("resolve_tags with no params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
	}

	// --- Test 2: Missing tag_field ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/SomeTable.SomeTable"));
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("SomeTag")));
		Params->SetArrayField(TEXT("tags"), Tags);

		FUDBCommandResult Result = Handler.Execute(TEXT("resolve_tags"), Params);
		TestFalse(TEXT("resolve_tags without tag_field should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
	}

	// --- Test 3: Missing tags array ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/SomeTable.SomeTable"));
		Params->SetStringField(TEXT("tag_field"), TEXT("SomeField"));

		FUDBCommandResult Result = Handler.Execute(TEXT("resolve_tags"), Params);
		TestFalse(TEXT("resolve_tags without tags should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
	}

	// --- Test 4: Invalid table path ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/NonExistent/DT_FakeTable.DT_FakeTable"));
		Params->SetStringField(TEXT("tag_field"), TEXT("SomeField"));
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("SomeTag")));
		Params->SetArrayField(TEXT("tags"), Tags);

		FUDBCommandResult Result = Handler.Execute(TEXT("resolve_tags"), Params);
		TestFalse(TEXT("resolve_tags with invalid table should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be TABLE_NOT_FOUND"), Result.ErrorCode, FString(TEXT("TABLE_NOT_FOUND")));
	}

	// --- Test 5: Integration test with real data (if available) ---
	// Find a DataTable that has a GameplayTag or GameplayTagContainer field
	{
		FUDBCommandResult ListResult = Handler.Execute(TEXT("list_datatables"), MakeShared<FJsonObject>());
		if (!ListResult.bSuccess || !ListResult.Data.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tables = nullptr;
		if (!ListResult.Data->TryGetArrayField(TEXT("datatables"), Tables) || Tables == nullptr)
		{
			return true;
		}

		FString TagTablePath;
		FString TagFieldName;

		for (const TSharedPtr<FJsonValue>& TableVal : *Tables)
		{
			const TSharedPtr<FJsonObject>* TableObj = nullptr;
			if (!TableVal.IsValid() || !TableVal->TryGetObject(TableObj) || TableObj == nullptr)
			{
				continue;
			}

			FString TablePath;
			(*TableObj)->TryGetStringField(TEXT("path"), TablePath);
			if (TablePath.IsEmpty())
			{
				continue;
			}

			// Get schema to find a tag field
			TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
			SchemaParams->SetStringField(TEXT("table_path"), TablePath);
			FUDBCommandResult SchemaResult = Handler.Execute(TEXT("get_datatable_schema"), SchemaParams);

			if (!SchemaResult.bSuccess || !SchemaResult.Data.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
			if (!SchemaResult.Data->TryGetObjectField(TEXT("schema"), SchemaObj) || SchemaObj == nullptr)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
			if (!(*SchemaObj)->TryGetArrayField(TEXT("fields"), FieldsArr) || FieldsArr == nullptr)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArr)
			{
				const TSharedPtr<FJsonObject>* FieldObj = nullptr;
				if (!FieldVal.IsValid() || !FieldVal->TryGetObject(FieldObj) || FieldObj == nullptr)
				{
					continue;
				}

				FString FieldType;
				(*FieldObj)->TryGetStringField(TEXT("type"), FieldType);
				if (FieldType == TEXT("FGameplayTag") || FieldType == TEXT("FGameplayTagContainer"))
				{
					(*FieldObj)->TryGetStringField(TEXT("name"), TagFieldName);
					TagTablePath = TablePath;
					break;
				}
			}

			if (!TagTablePath.IsEmpty())
			{
				break;
			}
		}

		if (TagTablePath.IsEmpty())
		{
			AddWarning(TEXT("No DataTable with GameplayTag field found - skipping integration test"));
			return true;
		}

		// --- Test 5a: Invalid tag_field type ---
		{
			// First, get schema to find a non-tag field
			TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
			SchemaParams->SetStringField(TEXT("table_path"), TagTablePath);
			FUDBCommandResult SchemaResult = Handler.Execute(TEXT("get_datatable_schema"), SchemaParams);

			if (SchemaResult.bSuccess && SchemaResult.Data.IsValid())
			{
				const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
				if (SchemaResult.Data->TryGetObjectField(TEXT("schema"), SchemaObj) && SchemaObj != nullptr)
				{
					const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
					if ((*SchemaObj)->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& FieldVal : *FieldsArr)
						{
							const TSharedPtr<FJsonObject>* FieldObj = nullptr;
							if (!FieldVal.IsValid() || !FieldVal->TryGetObject(FieldObj) || FieldObj == nullptr)
							{
								continue;
							}

							FString FieldType;
							(*FieldObj)->TryGetStringField(TEXT("type"), FieldType);
							FString FieldName;
							(*FieldObj)->TryGetStringField(TEXT("name"), FieldName);

							if (FieldType != TEXT("FGameplayTag") && FieldType != TEXT("FGameplayTagContainer")
								&& !FieldName.IsEmpty())
							{
								// Try resolving against a non-tag field
								TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
								BadParams->SetStringField(TEXT("table_path"), TagTablePath);
								BadParams->SetStringField(TEXT("tag_field"), FieldName);
								TArray<TSharedPtr<FJsonValue>> TagArr;
								TagArr.Add(MakeShared<FJsonValueString>(TEXT("SomeTag")));
								BadParams->SetArrayField(TEXT("tags"), TagArr);

								FUDBCommandResult BadResult = Handler.Execute(TEXT("resolve_tags"), BadParams);
								TestFalse(TEXT("resolve_tags with non-tag field should fail"), BadResult.bSuccess);
								TestEqual(TEXT("Error code should be INVALID_FIELD"), BadResult.ErrorCode, FString(TEXT("INVALID_FIELD")));
								break;
							}
						}
					}
				}
			}
		}

		// --- Test 5b: Resolve with a valid tag field but nonexistent tags ---
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("table_path"), TagTablePath);
			Params->SetStringField(TEXT("tag_field"), TagFieldName);
			TArray<TSharedPtr<FJsonValue>> Tags;
			Tags.Add(MakeShared<FJsonValueString>(TEXT("NonExistent.Tag.Value.XYZ")));
			Params->SetArrayField(TEXT("tags"), Tags);

			FUDBCommandResult Result = Handler.Execute(TEXT("resolve_tags"), Params);
			TestTrue(TEXT("resolve_tags with unresolvable tags should succeed"), Result.bSuccess);

			if (Result.bSuccess && Result.Data.IsValid())
			{
				double ResolvedCount = -1.0;
				Result.Data->TryGetNumberField(TEXT("resolved_count"), ResolvedCount);
				TestEqual(TEXT("resolved_count should be 0"), static_cast<int32>(ResolvedCount), 0);

				const TArray<TSharedPtr<FJsonValue>>* UnresolvedTags = nullptr;
				if (Result.Data->TryGetArrayField(TEXT("unresolved_tags"), UnresolvedTags) && UnresolvedTags != nullptr)
				{
					TestEqual(TEXT("Should have 1 unresolved tag"), UnresolvedTags->Num(), 1);
				}
			}
		}
	}

	return true;
}
