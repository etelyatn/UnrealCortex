#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddVariableTest,
	"Cortex.Blueprint.AddVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAddVariableTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_AddVarTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Temp/CortexBPTest_AddVar"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	FString TestBPPath = TEXT("/Temp/CortexBPTest_AddVar/BP_AddVarTest");

	// Test: add a float variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("Health"));
		Params->SetStringField(TEXT("type"), TEXT("float"));
		Params->SetStringField(TEXT("default_value"), TEXT("100.0"));
		Params->SetBoolField(TEXT("is_exposed"), true);
		Params->SetStringField(TEXT("category"), TEXT("Stats"));

		FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestTrue(TEXT("add_variable should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bAdded = false;
			Result.Data->TryGetBoolField(TEXT("added"), bAdded);
			TestTrue(TEXT("added should be true"), bAdded);

			FString VarName;
			Result.Data->TryGetStringField(TEXT("name"), VarName);
			TestEqual(TEXT("name should be Health"), VarName, TEXT("Health"));
		}
	}

	// Test: add a bool variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("bIsAlive"));
		Params->SetStringField(TEXT("type"), TEXT("bool"));

		FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestTrue(TEXT("add bool variable should succeed"), Result.bSuccess);
	}

	// Verify via get_info
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		TestTrue(TEXT("get_info should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray != nullptr)
			{
				bool bFoundHealth = false;
				bool bFoundIsAlive = false;

				for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
				{
					TSharedPtr<FJsonObject> VarObj = VarVal->AsObject();
					if (!VarObj.IsValid())
					{
						continue;
					}

					FString VarName;
					VarObj->TryGetStringField(TEXT("name"), VarName);

					if (VarName == TEXT("Health"))
					{
						bFoundHealth = true;
					}
					if (VarName == TEXT("bIsAlive"))
					{
						bFoundIsAlive = true;
					}
				}

				TestTrue(TEXT("Health variable should exist"), bFoundHealth);
				TestTrue(TEXT("bIsAlive variable should exist"), bFoundIsAlive);
			}
		}
	}

	// Test: duplicate variable name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("Health"));
		Params->SetStringField(TEXT("type"), TEXT("float"));

		FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestFalse(TEXT("duplicate variable should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be VARIABLE_EXISTS"),
			Result.ErrorCode, CortexErrorCodes::VariableExists);
	}

	// Test: missing params
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestFalse(TEXT("add_variable without name should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}
