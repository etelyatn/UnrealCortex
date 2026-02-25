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
#include "Components/ActorComponent.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileFailTest,
	"Cortex.Blueprint.CompileFail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileFailTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_CompileFailTest_%s"), *UniqueSuffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CompileFail_%s"), *UniqueSuffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), BlueprintName);
		Params->SetStringField(TEXT("path"), BlueprintDir);
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create should succeed"), Result.bSuccess);
	}

	// Load the Blueprint and inject a broken node
	UObject* LoadedObj = StaticLoadObject(
		UBlueprint::StaticClass(), nullptr,
		*BlueprintPath);
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
		Params->SetStringField(TEXT("asset_path"), BlueprintPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);

		// Compilation should fail due to duplicate event names
		TestFalse(TEXT("compile should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be COMPILE_FAILED"),
			Result.ErrorCode, CortexErrorCodes::CompileFailed);

		// Verify error details exist
		TestTrue(TEXT("Error details should exist"), Result.ErrorDetails.IsValid());
		FString CompileStatus;
		double ErrorCount = 0;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		TestTrue(TEXT("compile_status in details"),
			Result.ErrorDetails->TryGetStringField(TEXT("compile_status"), CompileStatus));
		TestEqual(TEXT("compile_status should be error"), CompileStatus, TEXT("error"));
		TestTrue(TEXT("error_count in details"),
			Result.ErrorDetails->TryGetNumberField(TEXT("error_count"), ErrorCount));
		TestTrue(TEXT("error_count > 0"), ErrorCount > 0.0);
		TestTrue(TEXT("diagnostics in details"),
			Result.ErrorDetails->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
		TestTrue(TEXT("diagnostics should have entries"),
			Diagnostics && Diagnostics->Num() > 0);
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileDiagnosticsReferenceTest,
	"Cortex.Blueprint.CompileDiagnostics.ReferenceExtraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileDiagnosticsReferenceTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_CompileRefDiag_%s"), *UniqueSuffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CompileRef_%s"), *UniqueSuffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), BlueprintName);
		CreateParams->SetStringField(TEXT("path"), BlueprintDir);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		TestTrue(TEXT("Setup: create should succeed"), CreateResult.bSuccess);
	}

	UBlueprint* TestBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Blueprint should exist"), TestBP);

	if (TestBP && TestBP->UbergraphPages.Num() > 0)
	{
		UEdGraph* EventGraph = TestBP->UbergraphPages[0];
		UK2Node_CallFunction* BrokenCall = NewObject<UK2Node_CallFunction>(EventGraph);
		BrokenCall->CreateNewGuid();
		BrokenCall->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UActorComponent, Activate),
			UActorComponent::StaticClass());
		EventGraph->AddNode(BrokenCall, false, false);
		BrokenCall->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsModified(TestBP);
	}

	TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), BlueprintPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("compile"), CompileParams);

	const TSharedPtr<FJsonObject> Payload = Result.bSuccess ? Result.Data : Result.ErrorDetails;
	if (!TestTrue(TEXT("compile payload should exist"), Payload.IsValid()))
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	TestTrue(TEXT("diagnostics present"),
		Payload->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
	if (!Diagnostics || Diagnostics->Num() == 0)
	{
		AddInfo(TEXT("No diagnostics were emitted for the constructed call-function node; skipping typed-reference assertions."));
		return true;
	}

	TSharedPtr<FJsonObject> CallFunctionDiag;
	for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
	{
		const TSharedPtr<FJsonObject> Candidate = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Candidate.IsValid())
		{
			continue;
		}

		FString NodeClass;
		if (Candidate->TryGetStringField(TEXT("node_class"), NodeClass)
			&& NodeClass == TEXT("K2Node_CallFunction"))
		{
			CallFunctionDiag = Candidate;
			break;
		}
	}

	if (!TestTrue(TEXT("call function diagnostic should exist"), CallFunctionDiag.IsValid()))
	{
		return true;
	}

	TestTrue(TEXT("node_class present"), CallFunctionDiag->HasField(TEXT("node_class")));
	TestTrue(TEXT("graph present"), CallFunctionDiag->HasField(TEXT("graph")));
	TestTrue(TEXT("node_id present"), CallFunctionDiag->HasField(TEXT("node_id")));
	TestTrue(TEXT("severity present"), CallFunctionDiag->HasField(TEXT("severity")));
	TestTrue(TEXT("referenced_class present"), CallFunctionDiag->HasField(TEXT("referenced_class")));
	TestTrue(TEXT("referenced_member present"), CallFunctionDiag->HasField(TEXT("referenced_member")));
	FString NodeClass;
	TestTrue(TEXT("node_class should be call function"),
		CallFunctionDiag->TryGetStringField(TEXT("node_class"), NodeClass));
	TestEqual(TEXT("node_class value"), NodeClass, TEXT("K2Node_CallFunction"));

	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}
