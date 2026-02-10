
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "Engine/CurveTable.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// --- list_curve_tables command -----------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBListCurveTablesTest,
	"UDB.CurveTable.ListCurveTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBListCurveTablesTest::RunTest(const FString& Parameters)
{
	FUDBCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FUDBCommandResult Result = Handler.Execute(TEXT("list_curve_tables"), Params);

	TestTrue(TEXT("list_curve_tables should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CurveTablesArray = nullptr;
		TestTrue(TEXT("Data should have curve_tables array"), Result.Data->TryGetArrayField(TEXT("curve_tables"), CurveTablesArray));

		double Count = 0;
		TestTrue(TEXT("Data should have count field"), Result.Data->TryGetNumberField(TEXT("count"), Count));
	}

	return true;
}

// --- get_curve_table on a temp table -----------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBGetCurveTableTest,
	"UDB.CurveTable.GetCurveTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBGetCurveTableTest::RunTest(const FString& Parameters)
{
	// Create a temporary CurveTable with a RichCurve row
	UCurveTable* TempTable = NewObject<UCurveTable>(
		GetTransientPackage(),
		FName(TEXT("CT_UDBTest_GetCurve")),
		RF_Public | RF_Standalone | RF_Transactional
	);
	TestNotNull(TEXT("Temp CurveTable should be created"), TempTable);

	if (TempTable == nullptr)
	{
		return true;
	}

	// Add a RichCurve row
	FRichCurve& Curve = TempTable->AddRichCurve(FName(TEXT("TestCurve")));
	Curve.UpdateOrAddKey(0.0f, 10.0f);
	Curve.UpdateOrAddKey(5.0f, 50.0f);
	Curve.UpdateOrAddKey(10.0f, 120.0f);

	FString TablePath = TempTable->GetPathName();

	FUDBCommandHandler Handler;

	// Test: get all curves
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), TablePath);

	FUDBCommandResult Result = Handler.Execute(TEXT("get_curve_table"), Params);

	TestTrue(TEXT("get_curve_table should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CurvesArray = nullptr;
		if (TestTrue(TEXT("Data should have curves array"), Result.Data->TryGetArrayField(TEXT("curves"), CurvesArray)))
		{
			TestEqual(TEXT("Should have 1 curve"), CurvesArray->Num(), 1);

			if (CurvesArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* CurveObj = nullptr;
				(*CurvesArray)[0]->TryGetObject(CurveObj);
				if (TestNotNull(TEXT("First curve should be an object"), CurveObj))
				{
					FString RowName;
					(*CurveObj)->TryGetStringField(TEXT("row_name"), RowName);
					TestEqual(TEXT("Row name should be TestCurve"), RowName, TEXT("TestCurve"));

					double KeyCount = 0;
					(*CurveObj)->TryGetNumberField(TEXT("key_count"), KeyCount);
					TestEqual(TEXT("Should have 3 keys"), static_cast<int32>(KeyCount), 3);

					const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
					if ((*CurveObj)->TryGetArrayField(TEXT("keys"), KeysArray) && KeysArray->Num() >= 3)
					{
						const TSharedPtr<FJsonObject>* FirstKey = nullptr;
						(*KeysArray)[0]->TryGetObject(FirstKey);
						if (FirstKey != nullptr)
						{
							double Time = -1.0;
							double Value = -1.0;
							(*FirstKey)->TryGetNumberField(TEXT("time"), Time);
							(*FirstKey)->TryGetNumberField(TEXT("value"), Value);
							TestEqual(TEXT("First key time should be 0"), Time, 0.0);
							TestEqual(TEXT("First key value should be 10"), Value, 10.0);
						}
					}
				}
			}
		}
	}

	// Test: get specific row
	TSharedPtr<FJsonObject> RowParams = MakeShared<FJsonObject>();
	RowParams->SetStringField(TEXT("table_path"), TablePath);
	RowParams->SetStringField(TEXT("row_name"), TEXT("TestCurve"));

	FUDBCommandResult RowResult = Handler.Execute(TEXT("get_curve_table"), RowParams);
	TestTrue(TEXT("get_curve_table with row_name should succeed"), RowResult.bSuccess);

	// Test: row not found
	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("table_path"), TablePath);
	MissingParams->SetStringField(TEXT("row_name"), TEXT("NonExistent"));

	FUDBCommandResult MissingResult = Handler.Execute(TEXT("get_curve_table"), MissingParams);
	TestFalse(TEXT("get_curve_table with missing row should fail"), MissingResult.bSuccess);
	TestEqual(TEXT("Error code should be ROW_NOT_FOUND"), MissingResult.ErrorCode, UDBErrorCodes::RowNotFound);

	// Cleanup
	TempTable->MarkAsGarbage();

	return true;
}

// --- update_curve_table_row command ------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBUpdateCurveTableRowTest,
	"UDB.CurveTable.UpdateCurveTableRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBUpdateCurveTableRowTest::RunTest(const FString& Parameters)
{
	// Create a temporary CurveTable
	UCurveTable* TempTable = NewObject<UCurveTable>(
		GetTransientPackage(),
		FName(TEXT("CT_UDBTest_UpdateCurve")),
		RF_Public | RF_Standalone | RF_Transactional
	);
	TestNotNull(TEXT("Temp CurveTable should be created"), TempTable);

	if (TempTable == nullptr)
	{
		return true;
	}

	// Add a RichCurve row with initial keys
	FRichCurve& Curve = TempTable->AddRichCurve(FName(TEXT("DamageScaling")));
	Curve.UpdateOrAddKey(0.0f, 1.0f);
	Curve.UpdateOrAddKey(1.0f, 2.0f);

	FString TablePath = TempTable->GetPathName();

	FUDBCommandHandler Handler;

	// Update the curve row with new keys
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), TablePath);
	Params->SetStringField(TEXT("row_name"), TEXT("DamageScaling"));

	TArray<TSharedPtr<FJsonValue>> KeysArray;
	{
		TSharedRef<FJsonObject> Key1 = MakeShared<FJsonObject>();
		Key1->SetNumberField(TEXT("time"), 0.0);
		Key1->SetNumberField(TEXT("value"), 10.0);
		KeysArray.Add(MakeShared<FJsonValueObject>(Key1));

		TSharedRef<FJsonObject> Key2 = MakeShared<FJsonObject>();
		Key2->SetNumberField(TEXT("time"), 5.0);
		Key2->SetNumberField(TEXT("value"), 50.0);
		KeysArray.Add(MakeShared<FJsonValueObject>(Key2));

		TSharedRef<FJsonObject> Key3 = MakeShared<FJsonObject>();
		Key3->SetNumberField(TEXT("time"), 10.0);
		Key3->SetNumberField(TEXT("value"), 120.0);
		KeysArray.Add(MakeShared<FJsonValueObject>(Key3));
	}
	Params->SetArrayField(TEXT("keys"), KeysArray);

	FUDBCommandResult Result = Handler.Execute(TEXT("update_curve_table_row"), Params);

	TestTrue(TEXT("update_curve_table_row should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		double KeysUpdated = 0;
		Result.Data->TryGetNumberField(TEXT("keys_updated"), KeysUpdated);
		TestEqual(TEXT("Should have updated 3 keys"), static_cast<int32>(KeysUpdated), 3);
	}

	// Verify the curve was actually updated
	FRealCurve* UpdatedCurve = TempTable->FindCurve(FName(TEXT("DamageScaling")), TEXT("test"));
	TestNotNull(TEXT("Curve should still exist"), UpdatedCurve);

	if (UpdatedCurve != nullptr)
	{
		TestEqual(TEXT("Updated curve should have 3 keys"), UpdatedCurve->GetNumKeys(), 3);

		// Check values via Eval
		float Val0 = UpdatedCurve->Eval(0.0f);
		float Val5 = UpdatedCurve->Eval(5.0f);
		float Val10 = UpdatedCurve->Eval(10.0f);
		TestEqual(TEXT("Value at t=0 should be 10"), Val0, 10.0f);
		TestEqual(TEXT("Value at t=5 should be 50"), Val5, 50.0f);
		TestEqual(TEXT("Value at t=10 should be 120"), Val10, 120.0f);
	}

	// Test: missing row should error
	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("table_path"), TablePath);
	MissingParams->SetStringField(TEXT("row_name"), TEXT("NonExistent"));
	MissingParams->SetArrayField(TEXT("keys"), KeysArray);

	FUDBCommandResult MissingResult = Handler.Execute(TEXT("update_curve_table_row"), MissingParams);
	TestFalse(TEXT("update with missing row should fail"), MissingResult.bSuccess);
	TestEqual(TEXT("Error code should be ROW_NOT_FOUND"), MissingResult.ErrorCode, UDBErrorCodes::RowNotFound);

	// Cleanup
	TempTable->MarkAsGarbage();

	return true;
}
