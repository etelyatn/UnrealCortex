#include "CortexGraphTree.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_Composite.h"
#include "Operations/CortexGraphNodeOps.h"

namespace
{
	struct FCortexGraphTreeChild
	{
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Owner = nullptr;
	};

	bool ClassChainHasName(const UClass* Class, const TCHAR* Name)
	{
		const FName Target(Name);
		for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
		{
			if (Current->GetFName() == Target)
			{
				return true;
			}
		}
		return false;
	}

	/** Reserves a path no earlier graph uses: the segment alone, then `~2`, `~3`... in collection order. '~' is not a valid object-name character, so a suffix never collides with a graph name. */
	FString ReservePath(TSet<FString>& UsedPaths, const FString& ParentPath, const FString& GraphName)
	{
		const FString Base = ParentPath.IsEmpty() ? GraphName : ParentPath + TEXT("/") + GraphName;
		FString Candidate = Base;
		for (int32 Ordinal = 2; UsedPaths.Contains(Candidate); ++Ordinal)
		{
			Candidate = FString::Printf(TEXT("%s~%d"), *Base, Ordinal);
		}
		UsedPaths.Add(Candidate);
		return Candidate;
	}

	/** The nested graphs of Graph that were not yet collected: owner-node graphs in node order, then graphs only SubGraphs knows. */
	void GatherChildren(const UEdGraph* Graph, const TSet<const UEdGraph*>& Visited, TArray<FCortexGraphTreeChild>& OutChildren)
	{
		TSet<const UEdGraph*> Seen;
		auto AddChild = [&](UEdGraph* Child, UEdGraphNode* Owner)
		{
			if (Child == nullptr || !IsValid(Child) || Visited.Contains(Child) || Seen.Contains(Child))
			{
				return;
			}
			Seen.Add(Child);
			OutChildren.Add({ Child, Owner });
		};

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (UEdGraph* Child : Node->GetSubGraphs())
			{
				AddChild(Child, Node);
			}
		}
		for (UEdGraph* Child : Graph->SubGraphs)
		{
			AddChild(Child, nullptr);
		}
	}

	void CollectChildren(int32 ParentIndex, TArray<FCortexGraphTreeEntry>& Entries, TSet<const UEdGraph*>& Visited, TSet<FString>& UsedPaths)
	{
		const UEdGraph* ParentGraph = Entries[ParentIndex].Graph;
		TArray<FCortexGraphTreeChild> Children;
		GatherChildren(ParentGraph, Visited, Children);
		if (Children.IsEmpty())
		{
			return;
		}

		if (Entries[ParentIndex].Depth >= FCortexGraphTree::MaxDepth)
		{
			Entries[ParentIndex].bChildrenTruncated = true;
			return;
		}

		for (const FCortexGraphTreeChild& Child : Children)
		{
			// A child already collected below an earlier sibling's subtree is a shared or cyclic reference, not a second graph.
			if (Visited.Contains(Child.Graph))
			{
				continue;
			}
			Visited.Add(Child.Graph);

			const FCortexGraphTreeEntry Parent = Entries[ParentIndex];
			FCortexGraphTreeEntry Entry;
			Entry.Graph = Child.Graph;
			Entry.Kind = FCortexGraphTree::KindFromGraph(Child.Graph, Child.Owner);
			Entry.ParentGraphGuid = Parent.Graph->GraphGuid;
			Entry.ParentGraphName = Parent.Graph->GetName();
			if (Child.Owner != nullptr)
			{
				Entry.OwnerNodeGuid = Child.Owner->NodeGuid;
				Entry.OwnerNodeName = Child.Owner->GetName();
			}
			Entry.Path = ReservePath(UsedPaths, Parent.Path, Child.Graph->GetName());
			if (Entry.Kind == TEXT("composite") && (Parent.bRoot || !Parent.SubgraphPath.IsEmpty()))
			{
				Entry.SubgraphPath = Parent.SubgraphPath.IsEmpty()
					? Child.Graph->GetName()
					: FString::Printf(TEXT("%s.%s"), *Parent.SubgraphPath, *Child.Graph->GetName());
			}
			Entry.RootGraphName = Parent.RootGraphName;
			Entry.Depth = Parent.Depth + 1;
			Entry.ParentIndex = ParentIndex;
			Entry.bRoot = false;
			Entry.RootKind = Parent.RootKind;
			Entry.OwningInterface = Parent.OwningInterface;

			const int32 EntryIndex = Entries.Add(Entry);
			CollectChildren(EntryIndex, Entries, Visited, UsedPaths);
		}
	}
}

void FCortexGraphTree::Collect(UBlueprint* Blueprint, TArray<FCortexGraphTreeEntry>& OutEntries)
{
	OutEntries.Reset();

	TArray<FCortexGraphEntry> Roots;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Roots);

	TSet<const UEdGraph*> Visited;
	for (const FCortexGraphEntry& Root : Roots)
	{
		Visited.Add(Root.Graph);
	}

	TSet<FString> UsedPaths;
	for (const FCortexGraphEntry& Root : Roots)
	{
		FCortexGraphTreeEntry Entry;
		Entry.Graph = Root.Graph;
		Entry.Kind = FCortexGraphNodeOps::GraphKindToString(Root.Kind);
		Entry.Path = ReservePath(UsedPaths, FString(), Root.Graph->GetName());
		Entry.RootGraphName = Root.Graph->GetName();
		Entry.bRoot = true;
		Entry.RootKind = Root.Kind;
		Entry.OwningInterface = Root.OwningInterface;

		const int32 EntryIndex = OutEntries.Add(Entry);
		CollectChildren(EntryIndex, OutEntries, Visited, UsedPaths);
	}

	TMap<FGuid, int32> GuidCounts;
	for (const FCortexGraphTreeEntry& Entry : OutEntries)
	{
		++GuidCounts.FindOrAdd(Entry.Graph->GraphGuid);
	}
	for (FCortexGraphTreeEntry& Entry : OutEntries)
	{
		Entry.bGuidUnique = GuidCounts[Entry.Graph->GraphGuid] == 1;
	}
}

FString FCortexGraphTree::KindFromGraph(const UEdGraph* Graph, const UEdGraphNode* Owner)
{
	// The owner node decides first: a conduit graph is a UAnimationTransitionGraph (AnimStateConduitNode.cpp:73-77),
	// so the graph class alone would label it a transition rule.
	if (Owner != nullptr)
	{
		if (Owner->IsA<UK2Node_Composite>())
		{
			return TEXT("composite");
		}
		const UClass* OwnerClass = Owner->GetClass();
		if (ClassChainHasName(OwnerClass, TEXT("AnimStateConduitNode")))
		{
			return TEXT("anim_conduit");
		}
		if (ClassChainHasName(OwnerClass, TEXT("AnimStateNode")))
		{
			return TEXT("anim_state");
		}
		if (ClassChainHasName(OwnerClass, TEXT("AnimGraphNode_StateMachineBase")))
		{
			return TEXT("anim_state_machine");
		}
		if (ClassChainHasName(OwnerClass, TEXT("AnimStateTransitionNode")))
		{
			return Graph != nullptr && ClassChainHasName(Graph->GetClass(), TEXT("AnimationCustomTransitionGraph"))
				? FString(TEXT("anim_custom_transition"))
				: FString(TEXT("anim_transition_rule"));
		}
	}

	if (Graph != nullptr)
	{
		const UClass* GraphClass = Graph->GetClass();
		if (ClassChainHasName(GraphClass, TEXT("AnimationBlendSpaceSampleGraph")))
		{
			return TEXT("anim_blend_space_sample");
		}
		if (ClassChainHasName(GraphClass, TEXT("BlendSpaceGraph")))
		{
			return TEXT("anim_blend_space");
		}
		if (ClassChainHasName(GraphClass, TEXT("AnimationCustomTransitionGraph")))
		{
			return TEXT("anim_custom_transition");
		}
		if (ClassChainHasName(GraphClass, TEXT("AnimationTransitionGraph")))
		{
			return TEXT("anim_transition_rule");
		}
		if (ClassChainHasName(GraphClass, TEXT("AnimationStateGraph")))
		{
			return TEXT("anim_state");
		}
		if (ClassChainHasName(GraphClass, TEXT("AnimationStateMachineGraph")))
		{
			return TEXT("anim_state_machine");
		}
		if (ClassChainHasName(GraphClass, TEXT("AnimationGraph")))
		{
			return TEXT("anim_graph");
		}
	}

	return TEXT("nested");
}

TSharedRef<FJsonObject> FCortexGraphTree::ToJson(const FCortexGraphTreeEntry& Entry)
{
	const UEdGraph* Graph = Entry.Graph;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Graph->GetName());
	Json->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
	Json->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
	Json->SetStringField(TEXT("kind"), Entry.Kind);
	Json->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
	Json->SetBoolField(TEXT("graph_guid_unique"), Entry.bGuidUnique);
	Json->SetStringField(TEXT("path"), Entry.Path);

	if (Entry.bRoot)
	{
		if (Entry.RootKind == ECortexGraphKind::InterfaceImpl && Entry.OwningInterface != NAME_None)
		{
			Json->SetStringField(TEXT("owning_interface"), Entry.OwningInterface.ToString());
		}
		if (Graph->GetClass()->GetFName() == FName(TEXT("AnimationGraph")))
		{
			Json->SetStringField(TEXT("graph_class_kind"), TEXT("anim_graph"));
		}
		return Json;
	}

	if (Entry.ParentIndex != INDEX_NONE)
	{
		Json->SetStringField(TEXT("parent_graph"), Entry.ParentGraphName);
		Json->SetStringField(TEXT("parent_graph_guid"), Entry.ParentGraphGuid.ToString());
	}
	if (Entry.OwnerNodeGuid.IsValid())
	{
		Json->SetStringField(TEXT("owner_node_guid"), Entry.OwnerNodeGuid.ToString());
	}
	if (!Entry.OwnerNodeName.IsEmpty())
	{
		Json->SetStringField(TEXT("owner_node"), Entry.OwnerNodeName);
	}
	if (!Entry.SubgraphPath.IsEmpty())
	{
		Json->SetStringField(TEXT("subgraph_path"), Entry.SubgraphPath);
	}
	return Json;
}

void FCortexGraphTree::AppendTruncation(const TArray<FCortexGraphTreeEntry>& Entries, const TSharedPtr<FJsonObject>& Data)
{
	TArray<TSharedPtr<FJsonValue>> CutParents;
	for (const FCortexGraphTreeEntry& Entry : Entries)
	{
		if (Entry.bChildrenTruncated)
		{
			CutParents.Add(MakeShared<FJsonValueString>(Entry.Graph->GraphGuid.ToString()));
		}
	}
	if (CutParents.IsEmpty())
	{
		return;
	}
	Data->SetBoolField(TEXT("truncated"), true);
	Data->SetArrayField(TEXT("truncated_parent_graph_guids"), CutParents);
}
