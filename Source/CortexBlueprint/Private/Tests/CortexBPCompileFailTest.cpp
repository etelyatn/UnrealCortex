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
#include "EdGraphSchema_K2.h"
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
	auto CleanupBlueprint = [&]()
	{
		UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (CreatedBP)
		{
			CreatedBP->MarkAsGarbage();
		}
	};

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
		UK2Node_CustomEvent* TriggerEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
		TriggerEvent->CreateNewGuid();
		TriggerEvent->CustomFunctionName = FName(TEXT("RunBrokenCall"));
		EventGraph->AddNode(TriggerEvent, false, false);
		TriggerEvent->AllocateDefaultPins();

		UK2Node_CallFunction* BrokenCall = NewObject<UK2Node_CallFunction>(EventGraph);
		BrokenCall->CreateNewGuid();
		BrokenCall->FunctionReference.SetExternalMember(FName(TEXT("DefinitelyMissingFunction")), UActorComponent::StaticClass());
		EventGraph->AddNode(BrokenCall, false, false);
		BrokenCall->AllocateDefaultPins();

		UEdGraphPin* EventThenPin = TriggerEvent->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* CallExecPin = BrokenCall->FindPin(UEdGraphSchema_K2::PN_Execute);
		if (EventThenPin && CallExecPin)
		{
			EventThenPin->MakeLinkTo(CallExecPin);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(TestBP);
	}

	TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), BlueprintPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("compile"), CompileParams);

	const TSharedPtr<FJsonObject> Payload = Result.bSuccess ? Result.Data : Result.ErrorDetails;
	if (!TestTrue(TEXT("compile payload should exist"), Payload.IsValid()))
	{
		CleanupBlueprint();
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	TestTrue(TEXT("diagnostics present"),
		Payload->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
	TestTrue(TEXT("diagnostics should not be empty"), Diagnostics && Diagnostics->Num() > 0);

	FString CompileStatus;
	TestTrue(TEXT("compile_status should exist"), Payload->TryGetStringField(TEXT("compile_status"), CompileStatus));
	TestTrue(TEXT("compile_status should be warning or error"),
		CompileStatus == TEXT("warning") || CompileStatus == TEXT("error"));

	TSharedPtr<FJsonObject> TypedDiag;
	if (Diagnostics)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
		{
			const TSharedPtr<FJsonObject> Candidate = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Candidate.IsValid())
			{
				continue;
			}

			FString ReferencedClass;
			FString ReferencedMember;
			if (Candidate->TryGetStringField(TEXT("referenced_class"), ReferencedClass)
				&& !ReferencedClass.IsEmpty()
				&& Candidate->TryGetStringField(TEXT("referenced_member"), ReferencedMember)
				&& !ReferencedMember.IsEmpty())
			{
				TypedDiag = Candidate;
				break;
			}
		}
	}

	if (!TestTrue(TEXT("diagnostic with typed references should exist"), TypedDiag.IsValid()))
	{
		CleanupBlueprint();
		return true;
	}

	TestTrue(TEXT("node_class present"), TypedDiag->HasField(TEXT("node_class")));
	TestTrue(TEXT("graph present"), TypedDiag->HasField(TEXT("graph")));
	TestTrue(TEXT("node_id present"), TypedDiag->HasField(TEXT("node_id")));
	TestTrue(TEXT("severity present"), TypedDiag->HasField(TEXT("severity")));
	TestTrue(TEXT("referenced_class present"), TypedDiag->HasField(TEXT("referenced_class")));
	TestTrue(TEXT("referenced_member present"), TypedDiag->HasField(TEXT("referenced_member")));
	FString NodeClass;
	FString ReferencedClass;
	FString ReferencedMember;
	TestTrue(TEXT("node_class should exist"),
		TypedDiag->TryGetStringField(TEXT("node_class"), NodeClass));
	TestTrue(TEXT("referenced_class should exist"),
		TypedDiag->TryGetStringField(TEXT("referenced_class"), ReferencedClass));
	TestEqual(TEXT("referenced_class should be ActorComponent"),
		ReferencedClass, TEXT("ActorComponent"));
	TestTrue(TEXT("referenced_member should exist"),
		TypedDiag->TryGetStringField(TEXT("referenced_member"), ReferencedMember));
	TestEqual(TEXT("referenced_member should be DefinitelyMissingFunction"),
		ReferencedMember, TEXT("DefinitelyMissingFunction"));

	CleanupBlueprint();

	return true;
}
