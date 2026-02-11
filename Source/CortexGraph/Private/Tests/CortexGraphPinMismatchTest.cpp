#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinMismatchTest,
	"Cortex.Graph.PinMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPinMismatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CortexGraphTest_PinMismatch")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint should be created"), TestBP);

	if (TestBP == nullptr)
	{
		return true;
	}

	FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Add two PrintString nodes
	FString Node1Id;
	FString Node2Id;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add first node should succeed"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), Node1Id);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add second node should succeed"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), Node2Id);
		}
	}

	// Attempt to connect an exec output to a string input (type mismatch)
	// "then" (exec output) -> "InString" (string input) should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), Node1Id);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), Node2Id);
		Params->SetStringField(TEXT("target_pin"), TEXT("InString"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		TestFalse(TEXT("Connecting exec to string should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be PIN_TYPE_MISMATCH"), Result.ErrorCode, CortexErrorCodes::PinTypeMismatch);
	}

	// Verify that connecting compatible pins still works (sanity check)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), Node1Id);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), Node2Id);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		TestTrue(TEXT("Connecting compatible exec pins should succeed"), Result.bSuccess);
	}

	// Cleanup
	TestBP->MarkAsGarbage();

	return true;
}
