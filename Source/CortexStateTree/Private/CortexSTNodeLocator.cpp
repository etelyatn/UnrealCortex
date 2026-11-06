#include "CortexSTNodeLocator.h"

#include "CortexSTCompat.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeNodeBase.h"
#include "StateTreeState.h"

namespace
{
FCortexSTNodeLocation MakeLocatorEntry(
	const ECortexSTNodeSlot Slot,
	FStateTreeEditorNode& Node,
	UStateTreeState* OwnerState,
	UObject* InstanceOuter,
	const int32 NodeIndex,
	const int32 TransitionIndex = INDEX_NONE)
{
	FCortexSTNodeLocation Location;
	Location.Slot = Slot;
	Location.Node = &Node;
	Location.OwnerState = OwnerState;
	Location.InstanceOuter = InstanceOuter;
	Location.NodeIndex = NodeIndex;
	Location.TransitionIndex = TransitionIndex;
	return Location;
}

void AppendLocatorArray(
	TArray<FStateTreeEditorNode>& Nodes,
	const ECortexSTNodeSlot Slot,
	UStateTreeState* OwnerState,
	UObject* InstanceOuter,
	TArray<FCortexSTNodeLocation>& OutNodes,
	const int32 TransitionIndex = INDEX_NONE)
{
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		OutNodes.Add(MakeLocatorEntry(Slot, Nodes[Index], OwnerState, InstanceOuter, Index, TransitionIndex));
	}
}
}

namespace CortexST
{
const TCHAR* LexNodeSlot(const ECortexSTNodeSlot Slot)
{
	switch (Slot)
	{
	case ECortexSTNodeSlot::GlobalTask: return TEXT("global_task");
	case ECortexSTNodeSlot::Evaluator: return TEXT("evaluator");
	case ECortexSTNodeSlot::Task: return TEXT("task");
	case ECortexSTNodeSlot::SingleTask: return TEXT("single_task");
	case ECortexSTNodeSlot::EnterCondition: return TEXT("enter_condition");
	case ECortexSTNodeSlot::Consideration: return TEXT("consideration");
	case ECortexSTNodeSlot::TransitionCondition: return TEXT("transition_condition");
	default: return TEXT("unknown");
	}
}

void CollectEditorNodes(const FCortexSTAssetContext& Context, TArray<FCortexSTNodeLocation>& OutNodes)
{
	if (Context.EditorData == nullptr)
	{
		return;
	}

	AppendLocatorArray(Context.EditorData->Evaluators, ECortexSTNodeSlot::Evaluator, nullptr, Context.EditorData, OutNodes);
	AppendLocatorArray(Context.EditorData->GlobalTasks, ECortexSTNodeSlot::GlobalTask, nullptr, Context.EditorData, OutNodes);

	TArray<FCortexSTStateRef> States;
	CollectAllStates(Context, States, false);
	for (const FCortexSTStateRef& StateRef : States)
	{
		UStateTreeState* State = StateRef.State;
		if (State == nullptr)
		{
			continue;
		}

		AppendLocatorArray(State->EnterConditions, ECortexSTNodeSlot::EnterCondition, State, State, OutNodes);
		if (TArray<FStateTreeEditorNode>* Considerations = CortexSTCompat::GetConsiderations(*State))
		{
			AppendLocatorArray(*Considerations, ECortexSTNodeSlot::Consideration, State, State, OutNodes);
		}
		AppendLocatorArray(State->Tasks, ECortexSTNodeSlot::Task, State, State, OutNodes);
		if (State->SingleTask.ID.IsValid())
		{
			OutNodes.Add(MakeLocatorEntry(ECortexSTNodeSlot::SingleTask, State->SingleTask, State, State, INDEX_NONE));
		}
		for (int32 TransitionIndex = 0; TransitionIndex < State->Transitions.Num(); ++TransitionIndex)
		{
			AppendLocatorArray(State->Transitions[TransitionIndex].Conditions, ECortexSTNodeSlot::TransitionCondition, State, State, OutNodes, TransitionIndex);
		}
	}
}

bool FindEditorNode(const FCortexSTAssetContext& Context, const FGuid& NodeId, FCortexSTNodeLocation& Out)
{
	if (!NodeId.IsValid())
	{
		return false;
	}

	TArray<FCortexSTNodeLocation> Nodes;
	CollectEditorNodes(Context, Nodes);
	for (const FCortexSTNodeLocation& Location : Nodes)
	{
		if (Location.Node != nullptr && Location.Node->ID == NodeId)
		{
			Out = Location;
			return true;
		}
	}
	return false;
}

const UClass* GetNodeInstanceClass(const FStateTreeEditorNode& Node)
{
	const FStateTreeNodeBase* NodeBase = Node.Node.GetPtr<FStateTreeNodeBase>();
	return NodeBase != nullptr ? Cast<UClass>(NodeBase->GetInstanceDataType()) : nullptr;
}

void FindEditorNodesByBlueprintClass(
	const FCortexSTAssetContext& Context,
	const UClass* BlueprintClass,
	const FString& BlueprintClassPath,
	TArray<FCortexSTNodeLocation>& OutNodes)
{
	TArray<FCortexSTNodeLocation> Nodes;
	CollectEditorNodes(Context, Nodes);
	for (const FCortexSTNodeLocation& Location : Nodes)
	{
		if (Location.Node == nullptr)
		{
			continue;
		}
		const UClass* NodeClass = GetNodeInstanceClass(*Location.Node);
		if (NodeClass == nullptr)
		{
			continue;
		}
		if ((BlueprintClass != nullptr && NodeClass == BlueprintClass)
			|| (!BlueprintClassPath.IsEmpty() && NodeClass->GetPathName() == BlueprintClassPath))
		{
			OutNodes.Add(Location);
		}
	}
}
}
