#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/KismetSystemLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphTest,
	"Cortex.Blueprint.RemoveGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRemoveGraphTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_RemoveGraph/BP_RemoveGraphTest");

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_RemoveGraphTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_RemoveGraph"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult R = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create BP"), R.bSuccess);
	}

	// Setup: add a function graph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TestFunction"));
		FCortexCommandResult R = Handler.Execute(TEXT("add_function"), Params);
		TestTrue(TEXT("Setup: add function"), R.bSuccess);
	}

	// Test: remove function graph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TestFunction"));
		Params->SetBoolField(TEXT("compile"), false);

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestTrue(TEXT("remove_graph should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* RemovedObj = nullptr;
			if (TestTrue(TEXT("Should have removed object"),
				Result.Data->TryGetObjectField(TEXT("removed"), RemovedObj)))
			{
				FString Name;
				(*RemovedObj)->TryGetStringField(TEXT("name"), Name);
				TestEqual(TEXT("Removed name"), Name, TEXT("TestFunction"));

				FString Type;
				(*RemovedObj)->TryGetStringField(TEXT("type"), Type);
				TestEqual(TEXT("Removed type"), Type, TEXT("Function"));

				double NodeCount = 0;
				(*RemovedObj)->TryGetNumberField(TEXT("node_count"), NodeCount);
				TestTrue(TEXT("node_count should be > 0"), NodeCount > 0.0);
			}
		}
	}

	// Verify: function no longer in get_info
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("functions"), FuncsArray))
			{
				bool bFoundTestFunc = false;
				for (const TSharedPtr<FJsonValue>& Val : *FuncsArray)
				{
					TSharedPtr<FJsonObject> Obj = Val->AsObject();
					if (Obj.IsValid())
					{
						FString FuncName;
						Obj->TryGetStringField(TEXT("name"), FuncName);
						if (FuncName == TEXT("TestFunction"))
						{
							bFoundTestFunc = true;
							break;
						}
					}
				}
				TestFalse(TEXT("TestFunction should no longer exist"), bFoundTestFunc);
			}
		}
	}

	// Test: remove non-existent graph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("NonExistentGraph"));

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(TEXT("remove non-existent should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be GRAPH_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	// Test: reject primary EventGraph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("EventGraph"));

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(TEXT("remove EventGraph should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_OPERATION"),
			Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// Test: reject ConstructionScript (friendly name)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("ConstructionScript"));

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(TEXT("remove ConstructionScript should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_OPERATION"),
			Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// Test: dry_run should not mutate
	{
		// Add a function to test dry_run against
		{
			TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
			AddParams->SetStringField(TEXT("asset_path"), TestBPPath);
			AddParams->SetStringField(TEXT("name"), TEXT("DryRunFunc"));
			Handler.Execute(TEXT("add_function"), AddParams);
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("DryRunFunc"));
		Params->SetBoolField(TEXT("dry_run"), true);

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestTrue(TEXT("dry_run should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bCompiled = true;
			Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
			TestFalse(TEXT("dry_run compiled should be false"), bCompiled);
		}

		// Verify function still exists
		{
			TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
			InfoParams->SetStringField(TEXT("asset_path"), TestBPPath);
			FCortexCommandResult InfoResult = Handler.Execute(TEXT("get_info"), InfoParams);
			if (InfoResult.Data.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
				if (InfoResult.Data->TryGetArrayField(TEXT("functions"), FuncsArray))
				{
					bool bFound = false;
					for (const TSharedPtr<FJsonValue>& Val : *FuncsArray)
					{
						TSharedPtr<FJsonObject> Obj = Val->AsObject();
						if (Obj.IsValid())
						{
							FString FuncName;
							Obj->TryGetStringField(TEXT("name"), FuncName);
							if (FuncName == TEXT("DryRunFunc")) { bFound = true; break; }
						}
					}
					TestTrue(TEXT("DryRunFunc should still exist after dry_run"), bFound);
				}
			}
		}

		// Clean up: actually remove it
		{
			TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
			RemoveParams->SetStringField(TEXT("asset_path"), TestBPPath);
			RemoveParams->SetStringField(TEXT("name"), TEXT("DryRunFunc"));
			RemoveParams->SetBoolField(TEXT("compile"), false);
			Handler.Execute(TEXT("remove_graph"), RemoveParams);
		}
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->GetOutermost()->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCascadeTest,
	"Cortex.Blueprint.RemoveGraph.Cascade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRemoveGraphCascadeTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_RGCascade/BP_CascadeTest");

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_CascadeTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_RGCascade"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult R = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create BP"), R.bSuccess);
	}

	// Setup: programmatically add a custom event with downstream nodes
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (!TestNotNull(TEXT("BP should load"), BP))
	{
		return true;
	}

	UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("EventGraph should exist"), EventGraph))
	{
		BP->GetOutermost()->MarkAsGarbage();
		return true;
	}

	// Add custom event node
	UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
	CustomEvent->CreateNewGuid();
	CustomEvent->CustomFunctionName = FName("TestCascadeEvent");
	EventGraph->AddNode(CustomEvent, false, false);
	CustomEvent->AllocateDefaultPins();

	// Add a downstream CallFunction node (PrintString)
	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintNode->CreateNewGuid();
	PrintNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	EventGraph->AddNode(PrintNode, false, false);
	PrintNode->AllocateDefaultPins();

	// Connect custom event exec out -> PrintString exec in
	UEdGraphPin* EventExecOut = nullptr;
	for (UEdGraphPin* Pin : CustomEvent->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			EventExecOut = Pin;
			break;
		}
	}

	UEdGraphPin* PrintExecIn = nullptr;
	for (UEdGraphPin* Pin : PrintNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			PrintExecIn = Pin;
			break;
		}
	}

	if (TestNotNull(TEXT("Event exec out pin"), EventExecOut) &&
		TestNotNull(TEXT("Print exec in pin"), PrintExecIn))
	{
		EventExecOut->MakeLinkTo(PrintExecIn);
	}

	const int32 NodeCountBefore = EventGraph->Nodes.Num();

	// Test: remove custom event with cascade=true
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TestCascadeEvent"));
		Params->SetBoolField(TEXT("cascade"), true);
		Params->SetBoolField(TEXT("compile"), false);

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestTrue(TEXT("remove custom event with cascade should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* RemovedObj = nullptr;
			if (TestTrue(TEXT("Should have removed object"),
				Result.Data->TryGetObjectField(TEXT("removed"), RemovedObj)))
			{
				FString Type;
				(*RemovedObj)->TryGetStringField(TEXT("type"), Type);
				TestEqual(TEXT("Type should be CustomEvent"), Type, TEXT("CustomEvent"));

				double NodeCount = 0;
				(*RemovedObj)->TryGetNumberField(TEXT("node_count"), NodeCount);
				TestEqual(TEXT("Should have removed 2 nodes (event + print)"), NodeCount, 2.0);

				bool bCascadeUsed = false;
				(*RemovedObj)->TryGetBoolField(TEXT("cascade"), bCascadeUsed);
				TestTrue(TEXT("cascade should be true"), bCascadeUsed);
			}
		}
	}

	// Verify nodes were removed from the graph
	TestEqual(TEXT("Graph should have 2 fewer nodes"), EventGraph->Nodes.Num(), NodeCountBefore - 2);

	// Cleanup
	BP->GetOutermost()->MarkAsGarbage();

	return true;
}
