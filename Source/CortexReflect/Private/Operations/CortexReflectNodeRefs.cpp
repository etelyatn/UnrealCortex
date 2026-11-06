#include "Operations/CortexReflectNodeRefs.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"

namespace CortexReflectNodeRefs
{
namespace
{
constexpr int32 MaxSubgraphDepth = 16;

void VisitGraph(
	const UEdGraph* TopGraph,
	const UEdGraph* Graph,
	const FString& SubgraphPath,
	const int32 Depth,
	const TFunctionRef<void(const UEdGraph*, const FString&, const UEdGraphNode*)>& Visitor)
{
	if (!Graph || Depth > MaxSubgraphDepth)
	{
		return;
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		Visitor(TopGraph, SubgraphPath, Node);
	}

	// Child graphs: composite (collapsed) graphs and the graphs the animation editor keeps in SubGraphs (state machines, states,
	// transitions, blend spaces). A composite's bound graph is normally also in SubGraphs, so it is visited once.
	TArray<const UEdGraph*> Children;
	for (const UEdGraph* Child : Graph->SubGraphs)
	{
		Children.AddUnique(Child);
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			Children.AddUnique(Composite->BoundGraph);
		}
	}
	for (const UEdGraph* Child : Children)
	{
		if (Child && Child != Graph)
		{
			const FString Name = Child->GetName();
			VisitGraph(TopGraph, Child, SubgraphPath.IsEmpty() ? Name : FString::Printf(TEXT("%s.%s"), *SubgraphPath, *Name), Depth + 1, Visitor);
		}
	}
}

UClass* GetClassPinDefault(const UK2Node_ConstructObjectFromClass* Node)
{
	const UEdGraphPin* ClassPin = Node->GetClassPin();
	return ClassPin ? Cast<UClass>(ClassPin->DefaultObject) : nullptr;
}
}

UClass* GetCastTargetClass(const UEdGraphNode* Node)
{
	const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
	return CastNode ? CastNode->TargetType.Get() : nullptr;
}

UClass* GetSpawnedClass(const UEdGraphNode* Node)
{
	const UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node);
	return SpawnNode ? GetClassPinDefault(SpawnNode) : nullptr;
}

UClass* GetCreatedWidgetClass(const UEdGraphNode* Node)
{
	static const FName CreateWidgetNodeName(TEXT("K2Node_CreateWidget"));
	const UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(Node);
	if (!ConstructNode)
	{
		return nullptr;
	}
	for (const UClass* NodeClass = ConstructNode->GetClass(); NodeClass; NodeClass = NodeClass->GetSuperClass())
	{
		if (NodeClass->GetFName() == CreateWidgetNodeName)
		{
			return GetClassPinDefault(ConstructNode);
		}
	}
	return nullptr;
}

FString GetNodeClassLabel(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}
	const UClass* NodeClass = Node->GetClass();
	return FString(NodeClass->GetPrefixCPP()) + NodeClass->GetName();
}

const UClass* GetCallMemberOwner(const UK2Node_CallFunction* Node, const UClass* Context)
{
	if (!Node)
	{
		return nullptr;
	}
	const UClass* Owner = Node->FunctionReference.GetMemberParentClass(const_cast<UClass*>(Context));
	if (!Owner)
	{
		const UFunction* Function = Node->GetTargetFunction();
		Owner = Function ? Function->GetOwnerClass() : nullptr;
	}
	return Owner ? Owner->GetAuthoritativeClass() : nullptr;
}

void ForEachNode(
	const UBlueprint* Blueprint,
	TFunctionRef<void(const UEdGraph* TopGraph, const FString& SubgraphPath, const UEdGraphNode* Node)> Visitor)
{
	if (!Blueprint)
	{
		return;
	}
	TArray<const UEdGraph*> TopGraphs;
	for (const TArray<TObjectPtr<UEdGraph>>* List : {&Blueprint->UbergraphPages, &Blueprint->FunctionGraphs, &Blueprint->MacroGraphs, &Blueprint->DelegateSignatureGraphs})
	{
		for (const UEdGraph* Graph : *List)
		{
			TopGraphs.Add(Graph);
		}
	}
	for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
	{
		for (const UEdGraph* Graph : Description.Graphs)
		{
			TopGraphs.Add(Graph);
		}
	}
	for (const UEdGraph* Graph : TopGraphs)
	{
		VisitGraph(Graph, Graph, FString(), 0, Visitor);
	}
}
}
