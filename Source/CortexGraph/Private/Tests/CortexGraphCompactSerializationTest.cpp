#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"

// ---------------------------------------------------------------------------
// Test 1: ShouldSkipPinCompact skips hidden unconnected pins with no defaults.
// K2Node_CallFunction (PrintString) has a hidden self pin — use that.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphCompactPinSkippingTest,
	"Cortex.Graph.CompactSerialization.PinSkipping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphCompactPinSkippingTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexCompactPinSkipTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_CompactPinSkipTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const FString AssetPath = TestBP->GetPathName();

	// Add a PrintString node — K2Node_CallFunction whose self pin is marked bHidden=true
	FString AddedNodeId;
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		AddParams->SetObjectField(TEXT("params"), NodeParams);
		const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
		if (!TestTrue(TEXT("add_node (setup) should succeed"), AddResult.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
		if (AddResult.Data.IsValid())
		{
			AddResult.Data->TryGetStringField(TEXT("node_id"), AddedNodeId);
		}
	}

	TestFalse(TEXT("Added node has a node_id"), AddedNodeId.IsEmpty());
	if (AddedNodeId.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	// Find the node directly on the graph so we can inspect raw pins
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : TestBP->UbergraphPages)
	{
		if (Graph)
		{
			EventGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("EventGraph exists"), EventGraph);
	if (!EventGraph)
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	UK2Node_CallFunction* CallNode = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (Node && Node->GetName() == AddedNodeId)
		{
			CallNode = Cast<UK2Node_CallFunction>(Node);
			break;
		}
	}
	TestNotNull(TEXT("K2Node_CallFunction found by node_id"), CallNode);
	if (!CallNode)
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	// Count total pins vs how many would be skipped in compact mode
	int32 TotalPins = 0;
	int32 SkippedPins = 0;
	int32 HiddenPins = 0;
	for (const UEdGraphPin* Pin : CallNode->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		++TotalPins;
		if (Pin->bHidden)
		{
			++HiddenPins;
		}
		if (FCortexGraphNodeOps::ShouldSkipPinCompact(Pin))
		{
			++SkippedPins;
		}
	}

	// PrintString K2Node_CallFunction has a hidden self pin
	TestTrue(TEXT("K2Node_CallFunction should have at least one pin"), TotalPins > 0);
	TestTrue(TEXT("PrintString node should have at least one hidden pin"), HiddenPins > 0);
	TestTrue(TEXT("At least one hidden unconnected pin should be skipped in compact mode"), SkippedPins > 0);
	TestTrue(TEXT("Not all pins should be skipped (exec/value pins are visible)"), SkippedPins < TotalPins);

	TestBP->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: list_nodes compact=true omits position/node_class/pin_count;
//         compact=false includes them
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphListNodesCompactTest,
	"Cortex.Graph.CompactSerialization.ListNodesCompact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphListNodesCompactTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexListNodesCompactTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_ListNodesCompactTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const FString AssetPath = TestBP->GetPathName();

	// Add a node so the list is non-trivial
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		AddParams->SetObjectField(TEXT("params"), NodeParams);
		const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
		if (!TestTrue(TEXT("add_node should succeed (setup)"), AddResult.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
	}

	// --- Compact mode (default: no compact param = compact=true) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		// compact is default (true), no explicit param needed

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TestTrue(TEXT("get_subgraph compact (default) should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (TestTrue(TEXT("Data has nodes array"), Result.Data->TryGetArrayField(TEXT("nodes"), NodesArray))
				&& NodesArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				(*NodesArray)[0]->TryGetObject(NodeObj);
				if (TestNotNull(TEXT("First node is object"), NodeObj))
				{
					TestFalse(TEXT("Compact: no 'position' field"), (*NodeObj)->HasField(TEXT("position")));
					TestFalse(TEXT("Compact: no 'node_class' field"), (*NodeObj)->HasField(TEXT("node_class")));
					TestFalse(TEXT("Compact: no 'pin_count' field"), (*NodeObj)->HasField(TEXT("pin_count")));
					// Fields that should still be present
					TestTrue(TEXT("Compact: 'class' field present"), (*NodeObj)->HasField(TEXT("class")));
					TestTrue(TEXT("Compact: 'node_id' field present"), (*NodeObj)->HasField(TEXT("node_id")));
					TestTrue(TEXT("Compact: 'display_name' field present"), (*NodeObj)->HasField(TEXT("display_name")));
				}
			}
		}
	}

	// --- Verbose mode (compact=false) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("compact"), false);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TestTrue(TEXT("get_subgraph compact=false should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (TestTrue(TEXT("Data has nodes array (verbose)"), Result.Data->TryGetArrayField(TEXT("nodes"), NodesArray))
				&& NodesArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				(*NodesArray)[0]->TryGetObject(NodeObj);
				if (TestNotNull(TEXT("First node is object (verbose)"), NodeObj))
				{
					TestTrue(TEXT("Verbose: 'position' field present"), (*NodeObj)->HasField(TEXT("position")));
					TestTrue(TEXT("Verbose: 'node_class' field present"), (*NodeObj)->HasField(TEXT("node_class")));
					TestTrue(TEXT("Verbose: 'pin_count' field present"), (*NodeObj)->HasField(TEXT("pin_count")));
				}
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: get_node compact=true filters hidden pins and omits position/node_class.
// Uses K2Node_CallFunction (PrintString) which has a hidden self pin.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphGetNodeCompactTest,
	"Cortex.Graph.CompactSerialization.GetNodeCompact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphGetNodeCompactTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGetNodeCompactTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_GetNodeCompactTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const FString AssetPath = TestBP->GetPathName();

	// Add a PrintString node — K2Node_CallFunction with a hidden self pin
	FString NodeId;
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		AddParams->SetObjectField(TEXT("params"), NodeParams);
		const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
		if (!TestTrue(TEXT("add_node (setup) should succeed"), AddResult.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
		if (AddResult.Data.IsValid())
		{
			AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}
	}

	TestFalse(TEXT("Added node has a node_id"), NodeId.IsEmpty());
	if (NodeId.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	// Count total raw pins (including hidden self pin) directly on the node
	int32 TotalRawPins = 0;
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : TestBP->UbergraphPages)
	{
		if (Graph)
		{
			EventGraph = Graph;
			break;
		}
	}
	if (EventGraph)
	{
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						++TotalRawPins;
					}
				}
				break;
			}
		}
	}
	TestTrue(TEXT("PrintString node has at least one pin"), TotalRawPins > 0);

	// --- Compact mode (default) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TestTrue(TEXT("get_subgraph compact (default) should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestFalse(TEXT("Compact: no 'position' field"), Result.Data->HasField(TEXT("position")));
			TestFalse(TEXT("Compact: no 'node_class' field"), Result.Data->HasField(TEXT("node_class")));
			TestTrue(TEXT("Compact: 'class' field present"), Result.Data->HasField(TEXT("class")));

			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			if (TestTrue(TEXT("Compact: 'pins' field present"), Result.Data->TryGetArrayField(TEXT("pins"), PinsArray)))
			{
				// Compact mode should skip the hidden self pin
				TestTrue(TEXT("Compact: fewer pins than total raw pins"), PinsArray->Num() < TotalRawPins);

				// Compact mode should omit is_connected when false and default_value when empty
				for (int32 i = 0; i < PinsArray->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (!(*PinsArray)[i]->TryGetObject(PinObj) || !PinObj)
					{
						continue;
					}

					// is_connected should be absent for unconnected pins (all are unconnected in this test)
					bool bIsConnected = false;
					if (!(*PinObj)->TryGetBoolField(TEXT("is_connected"), bIsConnected) || !bIsConnected)
					{
						TestFalse(
							FString::Printf(TEXT("Compact: pin[%d] 'is_connected' should be omitted when false"), i),
							(*PinObj)->HasField(TEXT("is_connected"))
						);
					}

					// default_value should be absent when empty
					FString DefaultValue;
					if (!(*PinObj)->TryGetStringField(TEXT("default_value"), DefaultValue) || DefaultValue.IsEmpty())
					{
						TestFalse(
							FString::Printf(TEXT("Compact: pin[%d] 'default_value' should be omitted when empty"), i),
							(*PinObj)->HasField(TEXT("default_value"))
						);
					}
				}
			}
		}
	}

	// --- Verbose mode (compact=false) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetBoolField(TEXT("compact"), false);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
		TestTrue(TEXT("get_subgraph compact=false should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestTrue(TEXT("Verbose: 'position' field present"), Result.Data->HasField(TEXT("position")));
			TestTrue(TEXT("Verbose: 'node_class' field present"), Result.Data->HasField(TEXT("node_class")));

			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			if (TestTrue(TEXT("Verbose: 'pins' field present"), Result.Data->TryGetArrayField(TEXT("pins"), PinsArray)))
			{
				// Verbose mode shows all pins including hidden ones
				TestEqual(TEXT("Verbose: all raw pins present"), PinsArray->Num(), TotalRawPins);

				// Verbose mode should include is_connected and default_value on every pin
				for (int32 i = 0; i < PinsArray->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (!(*PinsArray)[i]->TryGetObject(PinObj) || !PinObj)
					{
						continue;
					}
					TestTrue(
						FString::Printf(TEXT("Verbose: pin[%d] 'is_connected' field present"), i),
						(*PinObj)->HasField(TEXT("is_connected"))
					);
					TestTrue(
						FString::Printf(TEXT("Verbose: pin[%d] 'default_value' field present"), i),
						(*PinObj)->HasField(TEXT("default_value"))
					);
				}
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: search_nodes compact=true omits node_class; compact=false includes it
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesCompactTest,
	"Cortex.Graph.CompactSerialization.SearchNodesCompact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesCompactTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexSearchNodesCompactTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_SearchNodesCompactTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const FString AssetPath = TestBP->GetPathName();

	// Add a node we can search for
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		AddParams->SetObjectField(TEXT("params"), NodeParams);
		const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
		if (!TestTrue(TEXT("add_node should succeed (setup)"), AddResult.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
	}

	// --- Compact mode (default) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes compact (default) should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			if (TestTrue(TEXT("Data has results array"), Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
				&& ResultsArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				(*ResultsArray)[0]->TryGetObject(Entry);
				if (TestNotNull(TEXT("First result is object"), Entry))
				{
					TestFalse(TEXT("Compact: no 'node_class' field"), (*Entry)->HasField(TEXT("node_class")));
					TestTrue(TEXT("Compact: 'class' field present"), (*Entry)->HasField(TEXT("class")));
					TestTrue(TEXT("Compact: 'node_id' field present"), (*Entry)->HasField(TEXT("node_id")));
					TestTrue(TEXT("Compact: 'display_name' field present"), (*Entry)->HasField(TEXT("display_name")));
					TestTrue(TEXT("Compact: 'graph_name' field present"), (*Entry)->HasField(TEXT("graph_name")));
				}
			}
		}
	}

	// --- Verbose mode (compact=false) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		Params->SetBoolField(TEXT("compact"), false);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes compact=false should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			if (TestTrue(TEXT("Data has results array (verbose)"), Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
				&& ResultsArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				(*ResultsArray)[0]->TryGetObject(Entry);
				if (TestNotNull(TEXT("First result is object (verbose)"), Entry))
				{
					TestTrue(TEXT("Verbose: 'node_class' field present"), (*Entry)->HasField(TEXT("node_class")));
				}
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
