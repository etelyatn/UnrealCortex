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
	FString TestBPPath = TEXT("/Temp/CortexBPTest_StructOps/BP_StructOpsTest");

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_StructOpsTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Temp/CortexBPTest_StructOps"));
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
		}
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

	return true;
}
