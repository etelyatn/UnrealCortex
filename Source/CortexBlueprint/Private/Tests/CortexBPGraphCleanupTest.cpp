#include "Misc/AutomationTest.h"
#include "Operations/CortexBPGraphCleanupOps.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteOrphanedNodesTest,
	"Cortex.Blueprint.GraphCleanup.DeleteOrphanedNodes.RemovesOrphans",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteOrphanedNodesTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_OrphanTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph)
		{
			EventGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("EventGraph found"), EventGraph);
	if (!EventGraph)
	{
		BP->MarkAsGarbage();
		return false;
	}

	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	EventGraph->AddNode(PrintNode, false, false);
	PrintNode->AllocateDefaultPins();
	PrintNode->NodePosX = 400;
	PrintNode->NodePosY = 0;

	const int32 NodeCountBefore = EventGraph->Nodes.Num();
	TestTrue(TEXT("Graph has nodes before cleanup"), NodeCountBefore > 0);

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	const FCortexCommandResult Result = FCortexBPGraphCleanupOps::DeleteOrphanedNodes(Params);
	TestTrue(TEXT("delete_orphaned_nodes succeeded"), Result.bSuccess);

	int32 DeletedCount = 0;
	if (Result.Data.IsValid())
	{
		double Value = 0;
		Result.Data->TryGetNumberField(TEXT("deleted_count"), Value);
		DeletedCount = static_cast<int32>(Value);
	}
	TestTrue(TEXT("At least one orphaned node deleted"), DeletedCount > 0);

	bool bEventNodeSurvived = false;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (Node && Node->IsA<UK2Node_Event>())
		{
			bEventNodeSurvived = true;
			break;
		}
	}
	TestTrue(TEXT("Event entry nodes are preserved"), bEventNodeSurvived);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteOrphanedNodesConnectedTest,
	"Cortex.Blueprint.GraphCleanup.DeleteOrphanedNodes.PreservesConnected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteOrphanedNodesConnectedTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_OrphanConnectedTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph)
		{
			EventGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("EventGraph found"), EventGraph);
	if (!EventGraph)
	{
		BP->MarkAsGarbage();
		return false;
	}

	const int32 NodeCountBefore = EventGraph->Nodes.Num();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	const FCortexCommandResult Result = FCortexBPGraphCleanupOps::DeleteOrphanedNodes(Params);
	TestTrue(TEXT("delete_orphaned_nodes succeeded"), Result.bSuccess);

	int32 DeletedCount = 0;
	if (Result.Data.IsValid())
	{
		double Value = 0;
		Result.Data->TryGetNumberField(TEXT("deleted_count"), Value);
		DeletedCount = static_cast<int32>(Value);
	}
	TestEqual(TEXT("No nodes deleted from clean graph"), DeletedCount, 0);
	TestEqual(TEXT("Node count unchanged"), EventGraph->Nodes.Num(), NodeCountBefore);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteOrphanedNodesPreservesDataDepsTest,
	"Cortex.Blueprint.GraphCleanup.DeleteOrphanedNodes.PreservesDataDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteOrphanedNodesPreservesDataDepsTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_OrphanDataDepsTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph)
		{
			EventGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("EventGraph found"), EventGraph);
	if (!EventGraph)
	{
		BP->MarkAsGarbage();
		return false;
	}

	UK2Node_Event* BeginPlayEvent = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			BeginPlayEvent = EventNode;
			break;
		}
	}
	TestNotNull(TEXT("Event node found"), BeginPlayEvent);
	if (!BeginPlayEvent)
	{
		BP->MarkAsGarbage();
		return false;
	}

	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	EventGraph->AddNode(PrintNode, false, false);
	PrintNode->AllocateDefaultPins();

	UK2Node_CallFunction* ConnectedPureNode = NewObject<UK2Node_CallFunction>(EventGraph);
	ConnectedPureNode->SetFromFunction(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
	EventGraph->AddNode(ConnectedPureNode, false, false);
	ConnectedPureNode->AllocateDefaultPins();

	UK2Node_CallFunction* OrphanPureNode = NewObject<UK2Node_CallFunction>(EventGraph);
	OrphanPureNode->SetFromFunction(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_IntToString")));
	EventGraph->AddNode(OrphanPureNode, false, false);
	OrphanPureNode->AllocateDefaultPins();

	UEdGraphPin* EventThenPin = BeginPlayEvent->FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* PrintExecPin = PrintNode->FindPin(UEdGraphSchema_K2::PN_Execute);
	UEdGraphPin* PureOutPin = nullptr;
	for (UEdGraphPin* Pin : ConnectedPureNode->Pins)
	{
		if (Pin
			&& Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			PureOutPin = Pin;
			break;
		}
	}
	UEdGraphPin* PrintStringPin = PrintNode->FindPin(TEXT("InString"));
	TestNotNull(TEXT("Event then pin found"), EventThenPin);
	TestNotNull(TEXT("Print exec pin found"), PrintExecPin);
	TestNotNull(TEXT("Pure output pin found"), PureOutPin);
	TestNotNull(TEXT("Print InString pin found"), PrintStringPin);
	if (!EventThenPin || !PrintExecPin || !PureOutPin || !PrintStringPin)
	{
		BP->MarkAsGarbage();
		return false;
	}

	EventThenPin->MakeLinkTo(PrintExecPin);
	PureOutPin->MakeLinkTo(PrintStringPin);

	UEdGraphNode* ConnectedPureNodePtr = ConnectedPureNode;
	UEdGraphNode* OrphanPureNodePtr = OrphanPureNode;

	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	const FCortexCommandResult Result = FCortexBPGraphCleanupOps::DeleteOrphanedNodes(Params);
	TestTrue(TEXT("delete_orphaned_nodes succeeded"), Result.bSuccess);

	bool bConnectedNodeExists = false;
	bool bOrphanNodeExists = false;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (Node == ConnectedPureNodePtr)
		{
			bConnectedNodeExists = true;
		}
		if (Node == OrphanPureNodePtr)
		{
			bOrphanNodeExists = true;
		}
	}

	TestTrue(TEXT("Pure node connected to live exec chain should be preserved"), bConnectedNodeExists);
	TestFalse(TEXT("Unlinked pure node should be deleted"), bOrphanNodeExists);

	BP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteOrphanedNodesRejectsFunctionGraphTest,
	"Cortex.Blueprint.GraphCleanup.DeleteOrphanedNodes.RejectsFunctionGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteOrphanedNodesRejectsFunctionGraphTest::RunTest(const FString& Parameters)
{
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_OrphanRejectFunctionGraphTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("BP created"), BP);
	if (!BP)
	{
		return false;
	}

	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		FName(TEXT("TestCleanupFunctionGraph")),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FunctionGraph, true, static_cast<UClass*>(nullptr));
	FKismetEditorUtilities::CompileBlueprint(BP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("TestCleanupFunctionGraph"));
	const FCortexCommandResult Result = FCortexBPGraphCleanupOps::DeleteOrphanedNodes(Params);
	TestFalse(TEXT("delete_orphaned_nodes should reject non-event graphs"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	BP->MarkAsGarbage();
	return true;
}
