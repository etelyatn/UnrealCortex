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
