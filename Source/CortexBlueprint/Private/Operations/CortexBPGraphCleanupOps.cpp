#include "Operations/CortexBPGraphCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

namespace
{
	void GatherReachableExecNodesFromEvents(UEdGraph* Graph, TSet<UEdGraphNode*>& OutReachable, bool& bOutHasEventNode)
	{
		bOutHasEventNode = false;
		if (!Graph)
		{
			return;
		}

		TArray<UEdGraphNode*> Queue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_Event>())
			{
				bOutHasEventNode = true;
				OutReachable.Add(Node);
				Queue.Add(Node);
			}
		}

		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			UEdGraphNode* CurrentNode = Queue[QueueIndex++];
			if (!CurrentNode)
			{
				continue;
			}

			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}

				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}

					UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					if (LinkedNode && !OutReachable.Contains(LinkedNode))
					{
						OutReachable.Add(LinkedNode);
						Queue.Add(LinkedNode);
					}
				}
			}
		}
	}

	void ExpandToLinkedDataDependencies(UEdGraph* Graph, TSet<UEdGraphNode*>& InOutReachable)
	{
		if (!Graph || InOutReachable.Num() == 0)
		{
			return;
		}

		TArray<UEdGraphNode*> Queue = InOutReachable.Array();
		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			UEdGraphNode* CurrentNode = Queue[QueueIndex++];
			if (!CurrentNode)
			{
				continue;
			}

			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}

					UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					if (LinkedNode && !InOutReachable.Contains(LinkedNode))
					{
						InOutReachable.Add(LinkedNode);
						Queue.Add(LinkedNode);
					}
				}
			}
		}
	}
}

FCortexCommandResult FCortexBPGraphCleanupOps::DeleteOrphanedNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, graph_name"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}

	if (!TargetGraph)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Event graph not found: %s"), *GraphName));
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;

	TSet<UEdGraphNode*> ReachableNodes;
	bool bHasEventNode = false;
	GatherReachableExecNodesFromEvents(TargetGraph, ReachableNodes, bHasEventNode);
	ExpandToLinkedDataDependencies(TargetGraph, ReachableNodes);

	TArray<UEdGraphNode*> NodesToDelete;
	for (UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (!Node || Node->IsA<UK2Node_Event>())
		{
			continue;
		}

		if (bHasEventNode && !ReachableNodes.Contains(Node))
		{
			NodesToDelete.Add(Node);
		}
	}

	TArray<TSharedPtr<FJsonValue>> DeletedNodes;
	if (NodesToDelete.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Delete %d orphaned nodes from %s"), NodesToDelete.Num(), *GraphName)));

		Blueprint->Modify();
		TargetGraph->Modify();

		for (UEdGraphNode* Node : NodesToDelete)
		{
			if (!Node)
			{
				continue;
			}

			DeletedNodes.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s (%s)"), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Node->GetClass()->GetName())));
			TargetGraph->RemoveNode(Node);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("graph_name"), GraphName);
	ResponseData->SetNumberField(TEXT("deleted_count"), NodesToDelete.Num());
	ResponseData->SetArrayField(TEXT("deleted_nodes"), DeletedNodes);
	ResponseData->SetNumberField(TEXT("remaining_nodes"), TargetGraph->Nodes.Num());

	UE_LOG(LogCortexBlueprint, Log, TEXT("DeleteOrphanedNodes: %s/%s deleted=%d remaining=%d"),
		*Blueprint->GetName(),
		*GraphName,
		NodesToDelete.Num(),
		TargetGraph->Nodes.Num());

	return FCortexCommandRouter::Success(ResponseData);
}
