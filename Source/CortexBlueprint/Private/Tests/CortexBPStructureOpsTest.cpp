#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPStructureOpsTest,
	"Cortex.Blueprint.StructureOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPStructureOpsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint with a variable
	FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_StructOps/BP_StructOpsTest");

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_StructOpsTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_StructOps"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TempVar"));
		Params->SetStringField(TEXT("type"), TEXT("float"));
		Handler.Execute(TEXT("add_variable"), Params);
	}

	// Test: remove_variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TempVar"));

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_variable"), Params);
		TestTrue(TEXT("remove_variable should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bRemoved = false;
			Result.Data->TryGetBoolField(TEXT("removed"), bRemoved);
			TestTrue(TEXT("removed should be true"), bRemoved);
		}
	}

	// Test: remove non-existent variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("NonExistentVar"));

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_variable"), Params);
		TestFalse(TEXT("remove non-existent should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be VARIABLE_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::VariableNotFound);
	}

	// Test: add_function
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("CalculateDamage"));

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TSharedPtr<FJsonObject> InputParam = MakeShared<FJsonObject>();
		InputParam->SetStringField(TEXT("name"), TEXT("BaseDamage"));
		InputParam->SetStringField(TEXT("type"), TEXT("float"));
		Inputs.Add(MakeShared<FJsonValueObject>(InputParam));
		Params->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		TSharedPtr<FJsonObject> OutputParam = MakeShared<FJsonObject>();
		OutputParam->SetStringField(TEXT("name"), TEXT("FinalDamage"));
		OutputParam->SetStringField(TEXT("type"), TEXT("float"));
		Outputs.Add(MakeShared<FJsonValueObject>(OutputParam));
		Params->SetArrayField(TEXT("outputs"), Outputs);

		FCortexCommandResult Result = Handler.Execute(TEXT("add_function"), Params);
		TestTrue(TEXT("add_function should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bAdded = false;
			Result.Data->TryGetBoolField(TEXT("added"), bAdded);
			TestTrue(TEXT("added should be true"), bAdded);

			FString FuncName;
			Result.Data->TryGetStringField(TEXT("name"), FuncName);
			TestEqual(TEXT("name should match"), FuncName, TEXT("CalculateDamage"));

			// Verify inputs array in response (read back from actual pins)
			const TArray<TSharedPtr<FJsonValue>>* ResponseInputs = nullptr;
			if (TestTrue(TEXT("Should have inputs array"),
				Result.Data->TryGetArrayField(TEXT("inputs"), ResponseInputs)))
			{
				TestEqual(TEXT("Should have 1 input"), ResponseInputs->Num(), 1);
				if (ResponseInputs->Num() > 0)
				{
					const TSharedPtr<FJsonObject>& InputObj = (*ResponseInputs)[0]->AsObject();
					FString InputName, InputType;
					InputObj->TryGetStringField(TEXT("name"), InputName);
					InputObj->TryGetStringField(TEXT("type"), InputType);
					TestEqual(TEXT("Input name"), InputName, TEXT("BaseDamage"));
					TestEqual(TEXT("Input type"), InputType, TEXT("float"));
				}
			}

			// Verify outputs array in response
			const TArray<TSharedPtr<FJsonValue>>* ResponseOutputs = nullptr;
			if (TestTrue(TEXT("Should have outputs array"),
				Result.Data->TryGetArrayField(TEXT("outputs"), ResponseOutputs)))
			{
				TestEqual(TEXT("Should have 1 output"), ResponseOutputs->Num(), 1);
				if (ResponseOutputs->Num() > 0)
				{
					const TSharedPtr<FJsonObject>& OutputObj = (*ResponseOutputs)[0]->AsObject();
					FString OutputName, OutputType;
					OutputObj->TryGetStringField(TEXT("name"), OutputName);
					OutputObj->TryGetStringField(TEXT("type"), OutputType);
					TestEqual(TEXT("Output name"), OutputName, TEXT("FinalDamage"));
					TestEqual(TEXT("Output type"), OutputType, TEXT("float"));
				}
			}
		}
	}

	// Test: add_function with invalid type should fail (all-or-nothing validation)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("BadFunc"));

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TSharedPtr<FJsonObject> InputParam = MakeShared<FJsonObject>();
		InputParam->SetStringField(TEXT("name"), TEXT("GoodParam"));
		InputParam->SetStringField(TEXT("type"), TEXT("float"));
		Inputs.Add(MakeShared<FJsonValueObject>(InputParam));

		TSharedPtr<FJsonObject> BadParam = MakeShared<FJsonObject>();
		BadParam->SetStringField(TEXT("name"), TEXT("BadParam"));
		BadParam->SetStringField(TEXT("type"), TEXT("nonexistent_type_xyz"));
		Inputs.Add(MakeShared<FJsonValueObject>(BadParam));

		Params->SetArrayField(TEXT("inputs"), Inputs);

		FCortexCommandResult BadResult = Handler.Execute(TEXT("add_function"), Params);
		TestFalse(TEXT("add_function with invalid type should fail"), BadResult.bSuccess);
	}

	// Verify function appears in get_info
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);

		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("functions"), FuncsArray) && FuncsArray != nullptr)
			{
				bool bFoundCalcDamage = false;
				for (const TSharedPtr<FJsonValue>& FuncVal : *FuncsArray)
				{
					TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
					if (FuncObj.IsValid())
					{
						FString FuncName;
						FuncObj->TryGetStringField(TEXT("name"), FuncName);
						if (FuncName == TEXT("CalculateDamage"))
						{
							bFoundCalcDamage = true;

							// BP-002c: Verify function inputs/outputs in get_info
							const TArray<TSharedPtr<FJsonValue>>* FuncInputs = nullptr;
							if (TestTrue(TEXT("Function should have inputs array"),
								FuncObj->TryGetArrayField(TEXT("inputs"), FuncInputs)))
							{
								TestEqual(TEXT("Function should have 1 input"), FuncInputs->Num(), 1);
								if (FuncInputs->Num() > 0)
								{
									const TSharedPtr<FJsonObject>& InObj = (*FuncInputs)[0]->AsObject();
									FString InName, InType;
									InObj->TryGetStringField(TEXT("name"), InName);
									InObj->TryGetStringField(TEXT("type"), InType);
									TestEqual(TEXT("get_info input name"), InName, TEXT("BaseDamage"));
									TestEqual(TEXT("get_info input type"), InType, TEXT("float"));
								}
							}

							const TArray<TSharedPtr<FJsonValue>>* FuncOutputs = nullptr;
							if (TestTrue(TEXT("Function should have outputs array"),
								FuncObj->TryGetArrayField(TEXT("outputs"), FuncOutputs)))
							{
								TestEqual(TEXT("Function should have 1 output"), FuncOutputs->Num(), 1);
								if (FuncOutputs->Num() > 0)
								{
									const TSharedPtr<FJsonObject>& OutObj = (*FuncOutputs)[0]->AsObject();
									FString OutName, OutType;
									OutObj->TryGetStringField(TEXT("name"), OutName);
									OutObj->TryGetStringField(TEXT("type"), OutType);
									TestEqual(TEXT("get_info output name"), OutName, TEXT("FinalDamage"));
									TestEqual(TEXT("get_info output type"), OutType, TEXT("float"));
								}
							}

							break;
						}
					}
				}
				TestTrue(TEXT("CalculateDamage function should exist"), bFoundCalcDamage);
			}
		}
	}

	// Test: duplicate function
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("CalculateDamage"));

		FCortexCommandResult Result = Handler.Execute(TEXT("add_function"), Params);
		TestFalse(TEXT("duplicate function should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be FUNCTION_EXISTS"),
			Result.ErrorCode, CortexErrorCodes::FunctionExists);
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}
