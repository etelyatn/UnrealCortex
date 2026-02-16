#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAddNodeTest,
	"Cortex.Graph.AddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAddNodeTest::RunTest(const FString& Parameters)
{
	// Setup: Create a transient Blueprint for testing
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphAddNodeTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_AddNodeTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint should be created"), TestBP);
	if (TestBP == nullptr) return false;

	FString AssetPath = TestBP->GetPathName();

	// Register handler
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Get initial node count via list_nodes
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult ListResult = Router.Execute(TEXT("graph.list_nodes"), ListParams);
	TestTrue(TEXT("list_nodes should succeed"), ListResult.bSuccess);

	int32 InitialNodeCount = 0;
	if (ListResult.bSuccess && ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (ListResult.Data->TryGetArrayField(TEXT("nodes"), Nodes))
		{
			InitialNodeCount = Nodes->Num();
		}
	}

	// Act: Add a PrintString node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	AddParams->SetObjectField(TEXT("params"), NodeParams);

	FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	TestTrue(TEXT("add_node should succeed"), AddResult.bSuccess);

	if (AddResult.bSuccess && AddResult.Data.IsValid())
	{
		TestTrue(TEXT("Result should have node_id"), AddResult.Data->HasField(TEXT("node_id")));
		TestTrue(TEXT("Result should have class"), AddResult.Data->HasField(TEXT("class")));
	}

	// Verify: list_nodes should show one more node
	FCortexCommandResult ListResult2 = Router.Execute(TEXT("graph.list_nodes"), ListParams);
	TestTrue(TEXT("list_nodes after add should succeed"), ListResult2.bSuccess);

	if (ListResult2.bSuccess && ListResult2.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (ListResult2.Data->TryGetArrayField(TEXT("nodes"), Nodes))
		{
			TestEqual(TEXT("Node count should increase by 1"), Nodes->Num(), InitialNodeCount + 1);
		}
	}

	// Test: add an IfThenElse node
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node IfThenElse should succeed"), Result.bSuccess);

		// Verify node count increased by 1 more
		FCortexCommandResult ListResult3 = Router.Execute(TEXT("graph.list_nodes"), ListParams);
		TestTrue(TEXT("list_nodes after IfThenElse should succeed"), ListResult3.bSuccess);
		if (ListResult3.bSuccess && ListResult3.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (ListResult3.Data->TryGetArrayField(TEXT("nodes"), Nodes))
			{
				TestEqual(TEXT("Node count should increase by 1 more"), Nodes->Num(), InitialNodeCount + 2);
			}
		}
	}

	// Test: invalid function owner class should return error
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("function_name"), TEXT("NonExistentClass.FakeFunction"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node with invalid class should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: invalid function name on valid class should return error
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.NonExistentFunction"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node with invalid function should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: display_name should be populated for valid CallFunction node
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("function_name"), TEXT("GameplayStatics.GetGameMode"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node GetGameMode should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			FString DisplayName;
			TestTrue(TEXT("Result should have display_name"), Result.Data->TryGetStringField(TEXT("display_name"), DisplayName));
			TestTrue(TEXT("Display name should contain 'Game Mode'"),
				DisplayName.Contains(TEXT("Game Mode")));
		}
	}

	// Test: missing asset_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node without asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: missing node_class
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node without node_class should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: Add VariableSet node for external class property
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableSet"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
		NParams->SetStringField(TEXT("variable_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node VariableSet should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("VariableSet should have pins"), Result.Data->TryGetArrayField(TEXT("pins"), Pins));
			if (Pins)
			{
				TestTrue(TEXT("VariableSet should have multiple pins"), Pins->Num() >= 3);
			}
		}
	}

	// Test: Add VariableGet node for external class property
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableGet"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
		NParams->SetStringField(TEXT("variable_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node VariableGet should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("VariableGet should have pins"), Result.Data->TryGetArrayField(TEXT("pins"), Pins));
			if (Pins)
			{
				TestTrue(TEXT("VariableGet should have output pin"), Pins->Num() >= 1);
			}
		}
	}

	// Test: Invalid variable name returns error
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableSet"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("variable_name"), TEXT("NonExistentProperty"));
		NParams->SetStringField(TEXT("variable_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node with invalid variable_name should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: Invalid variable class returns error
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableSet"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
		NParams->SetStringField(TEXT("variable_class"), TEXT("NonExistentClass"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("add_node with invalid variable_class should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	TestBP->MarkAsGarbage();

	return true;
}
