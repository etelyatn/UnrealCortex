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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddVariableInheritedPropertyCollisionTest,
	"Cortex.Blueprint.AddVariable.InheritedPropertyCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAddVariableTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_AddVarTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_AddVar"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_AddVar/BP_AddVarTest");

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

	// Test: add a dispatcher (multicast delegate) variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("OnHealthChanged"));
		Params->SetStringField(TEXT("type"), TEXT("dispatcher"));

		FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestTrue(TEXT("add dispatcher variable should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bAdded = false;
			Result.Data->TryGetBoolField(TEXT("added"), bAdded);
			TestTrue(TEXT("dispatcher added should be true"), bAdded);

			FString VarType;
			Result.Data->TryGetStringField(TEXT("type"), VarType);
			TestEqual(TEXT("dispatcher type should round-trip as dispatcher"), VarType, TEXT("dispatcher"));
		}
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
				bool bFoundDispatcher = false;

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
					if (VarName == TEXT("OnHealthChanged"))
					{
						bFoundDispatcher = true;

						// Verify FriendlyTypeName round-trip: get_info must report "dispatcher", not raw "PC_MCDelegate"
						FString DispatcherType;
						VarObj->TryGetStringField(TEXT("type"), DispatcherType);
						TestEqual(TEXT("OnHealthChanged type should be dispatcher in get_info"), DispatcherType, TEXT("dispatcher"));
					}
				}

				TestTrue(TEXT("Health variable should exist"), bFoundHealth);
				TestTrue(TEXT("bIsAlive variable should exist"), bFoundIsAlive);
				TestTrue(TEXT("OnHealthChanged dispatcher should exist"), bFoundDispatcher);
			}
		}
	}

	// Verify: DelegateSignatureGraph was created for the dispatcher variable
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
		if (BP != nullptr)
		{
			const bool bHasSignatureGraph = BP->DelegateSignatureGraphs.ContainsByPredicate(
				[](const UEdGraph* Graph)
				{
					return Graph && Graph->GetFName() == FName(TEXT("OnHealthChanged"));
				}
			);
			TestTrue(TEXT("OnHealthChanged should have a DelegateSignatureGraph"), bHasSignatureGraph);
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

	// Cleanup: mark the entire package as garbage so the Blueprint, its
	// GeneratedClass, and CDO are all collected together.  Marking only the
	// UBlueprint object leaves the (dirty, uncompiled) GeneratedClass live,
	// which causes a background GC worker crash on the next test.
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->GetOutermost()->MarkAsGarbage();
	}

	return true;
}

bool FCortexBPAddVariableInheritedPropertyCollisionTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_AddVarComponentTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_AddVarComponent"));
		Params->SetStringField(TEXT("type"), TEXT("Component"));

		const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Component Blueprint create should succeed"), CreateResult.bSuccess);
	}

	const FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_AddVarComponent/BP_AddVarComponentTest");

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TickCount"));
		Params->SetStringField(TEXT("type"), TEXT("int"));

		const FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestTrue(TEXT("Non-colliding component variable should succeed"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("bIsActive"));
		Params->SetStringField(TEXT("type"), TEXT("bool"));

		const FCortexCommandResult Result = Handler.Execute(TEXT("add_variable"), Params);
		TestFalse(TEXT("Inherited property collision should be rejected"), Result.bSuccess);
		TestEqual(TEXT("Inherited property collision should use INVALID_VALUE"),
			Result.ErrorCode, CortexErrorCodes::InvalidValue);
		TestTrue(TEXT("Error should explain inherited UPROPERTY collision"),
			Result.ErrorMessage.Contains(TEXT("inherited UPROPERTY")));
	}

	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->GetOutermost()->MarkAsGarbage();
	}

	return true;
}
