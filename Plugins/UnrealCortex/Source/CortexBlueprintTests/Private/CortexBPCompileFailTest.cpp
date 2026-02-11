#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileFailTest,
	"Cortex.Blueprint.CompileFail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileFailTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_CompileFailTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Temp/CortexBPTest_CompileFail"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create should succeed"), Result.bSuccess);
	}

	// Load the Blueprint and inject a broken node
	UObject* LoadedObj = StaticLoadObject(
		UBlueprint::StaticClass(), nullptr,
		TEXT("/Temp/CortexBPTest_CompileFail/BP_CompileFailTest"));
	UBlueprint* TestBP = Cast<UBlueprint>(LoadedObj);
	TestNotNull(TEXT("Blueprint should exist"), TestBP);

	if (TestBP != nullptr && TestBP->UbergraphPages.Num() > 0)
	{
		UEdGraph* EventGraph = TestBP->UbergraphPages[0];

		// Add two custom events with the same name — guaranteed compile error
		UK2Node_CustomEvent* Event1 = NewObject<UK2Node_CustomEvent>(EventGraph);
		Event1->CreateNewGuid();
		Event1->CustomFunctionName = FName("DuplicateEventName");
		EventGraph->AddNode(Event1, false, false);
		Event1->AllocateDefaultPins();

		UK2Node_CustomEvent* Event2 = NewObject<UK2Node_CustomEvent>(EventGraph);
		Event2->CreateNewGuid();
		Event2->CustomFunctionName = FName("DuplicateEventName");
		EventGraph->AddNode(Event2, false, false);
		Event2->AllocateDefaultPins();

		FBlueprintEditorUtils::MarkBlueprintAsModified(TestBP);
	}

	// Test: compile the broken Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),
			TEXT("/Temp/CortexBPTest_CompileFail/BP_CompileFailTest"));

		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);

		// Compilation should fail due to duplicate event names
		TestFalse(TEXT("compile should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be COMPILE_FAILED"),
			Result.ErrorCode, CortexErrorCodes::CompileFailed);

		// Verify error details exist
		if (Result.ErrorDetails.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ErrorsArray = nullptr;
			if (Result.ErrorDetails->TryGetArrayField(TEXT("errors"), ErrorsArray))
			{
				TestTrue(TEXT("Should have at least one error"), ErrorsArray->Num() >= 1);
			}
		}
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, TEXT("/Temp/CortexBPTest_CompileFail/BP_CompileFailTest"));
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}
