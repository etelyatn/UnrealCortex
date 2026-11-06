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

void FCortexGraphTree::Collect(const UBlueprint* Blueprint, TArray<FCortexGraphTreeEntry>& OutEntries)
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

namespace
{
	FString TreeResolveParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
	{
		FString Value;
		if (Params.IsValid())
		{
			Params->TryGetStringField(Field, Value);
		}
		return Value;
	}

	TSharedRef<FJsonObject> TreeResolveGraphReference(const FCortexGraphTreeEntry& Entry)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("graph_guid"), Entry.Graph->GraphGuid.ToString());
		Json->SetStringField(TEXT("graph_path"), Entry.Path);
		Json->SetStringField(TEXT("kind"), Entry.Kind);
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> TreeResolveReferences(const TArray<const FCortexGraphTreeEntry*>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> References;
		for (const FCortexGraphTreeEntry* Entry : Entries)
		{
			References.Add(MakeShared<FJsonValueObject>(TreeResolveGraphReference(*Entry)));
		}
		return References;
	}
}

const FCortexGraphTreeEntry* FCortexGraphTree::ResolveInTree(const TArray<FCortexGraphTreeEntry>& Tree, const TSharedPtr<FJsonObject>& Params,
	FCortexCommandResult& OutError)
{
	const FString GuidText = TreeResolveParam(Params, TEXT("graph_guid"));
	const FString PathText = TreeResolveParam(Params, TEXT("graph_path"));
	const FString GraphName = TreeResolveParam(Params, TEXT("graph_name"));
	const FString SubgraphPath = TreeResolveParam(Params, TEXT("subgraph_path"));


	const FCortexGraphTreeEntry* Found = nullptr;
	if (!GuidText.IsEmpty() || !PathText.IsEmpty())
	{
		FGuid Guid;
		if (!GuidText.IsEmpty() && !FGuid::Parse(GuidText, Guid))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid graph_guid format: '%s'"), *GuidText));
			return nullptr;
		}

		if (!PathText.IsEmpty())
		{
			for (const FCortexGraphTreeEntry& Entry : Tree)
			{
				if (Entry.Path == PathText)
				{
					Found = &Entry;
					break;
				}
			}
			if (Found == nullptr)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::GraphNotFound,
					FString::Printf(TEXT("Graph path not found: %s"), *PathText));
				return nullptr;
			}
			if (!GuidText.IsEmpty() && Found->Graph->GraphGuid != Guid)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("graph_guid %s does not name the graph at graph_path '%s'; that graph's guid is %s"),
						*GuidText, *PathText, *Found->Graph->GraphGuid.ToString()));
				return nullptr;
			}
		}
		else
		{
			TArray<const FCortexGraphTreeEntry*> Matches;
			for (const FCortexGraphTreeEntry& Entry : Tree)
			{
				if (Entry.Graph->GraphGuid == Guid)
				{
					Matches.Add(&Entry);
				}
			}
			if (Matches.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::GraphNotFound,
					FString::Printf(TEXT("Graph with GUID %s not found on Blueprint"), *GuidText));
				return nullptr;
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> Paths;
				for (const FCortexGraphTreeEntry* Match : Matches)
				{
					Paths.Add(Match->Path);
				}
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetArrayField(TEXT("colliding_graphs"), TreeResolveReferences(Matches));
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("graph_guid %s is shared by %d graphs; address one with graph_path: %s"),
						*GuidText, Matches.Num(), *FString::Join(Paths, TEXT(", "))),
					Details);
				return nullptr;
			}
			Found = Matches[0];
		}

		if (!SubgraphPath.IsEmpty() && Found->SubgraphPath != SubgraphPath)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("subgraph_path '%s' does not name the graph identified by graph_guid/graph_path; that graph's subgraph_path is '%s'"),
					*SubgraphPath, *Found->SubgraphPath));
			return nullptr;
		}
		if (!GraphName.IsEmpty())
		{
			// graph_name is the graph's own name, or (with its composite subgraph_path) the root graph the path is relative to.
			const bool bOwnName = Found->Graph->GetName() == GraphName;
			const bool bRootRelative = !SubgraphPath.IsEmpty() && Found->RootGraphName == GraphName;
			if (!bOwnName && !bRootRelative)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("graph_name '%s' does not name the graph identified by graph_guid/graph_path; that graph is '%s'"),
						*GraphName, *Found->Graph->GetName()));
				return nullptr;
			}
		}
	}
	else
	{
		const FString TargetName = GraphName.IsEmpty() ? FString(TEXT("EventGraph")) : GraphName;
		const FCortexGraphTreeEntry* Root = nullptr;
		for (const FCortexGraphTreeEntry& Entry : Tree)
		{
			if (Entry.bRoot && Entry.Graph->GetName() == TargetName)
			{
				Root = &Entry;
				break;
			}
		}
		if (Root == nullptr)
		{
			TSharedPtr<FJsonObject> Details;
			TArray<const FCortexGraphTreeEntry*> Candidates;
			for (const FCortexGraphTreeEntry& Entry : Tree)
			{
				if (!Entry.bRoot && Entry.Graph->GetName() == TargetName)
				{
					Candidates.Add(&Entry);
				}
			}
			if (!Candidates.IsEmpty())
			{
				Details = MakeShared<FJsonObject>();
				Details->SetArrayField(TEXT("nested_candidates"), TreeResolveReferences(Candidates));
			}
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::GraphNotFound,
				Candidates.IsEmpty()
					? FString::Printf(TEXT("Graph not found: %s"), *TargetName)
					: FString::Printf(TEXT("Graph not found: %s; %d nested graphs carry that name, address one by graph_guid or graph_path"),
						*TargetName, Candidates.Num()),
				Details);
			return nullptr;
		}

		UEdGraph* Graph = Root->Graph;
		if (!SubgraphPath.IsEmpty())
		{
			Graph = FCortexGraphNodeOps::ResolveSubgraph(Graph, SubgraphPath, OutError);
			if (Graph == nullptr)
			{
				return nullptr;
			}
		}
		for (const FCortexGraphTreeEntry& Entry : Tree)
		{
			if (Entry.Graph == Graph)
			{
				Found = &Entry;
				break;
			}
		}
	}

	if (Found == nullptr)
	{
		// A composite body resolved through subgraph_path beyond the catalogue depth: no entry exists to report.
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::GraphNotFound,
			FString::Printf(TEXT("Graph '%s' (subgraph_path '%s') is outside the Blueprint's graph tree"), *GraphName, *SubgraphPath));
		return nullptr;
	}

	return Found;
}

bool FCortexGraphTree::ResolveReadTarget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, UEdGraph*& OutGraph,
	FCortexGraphTreeEntry* OutEntry, FCortexCommandResult& OutError)
{
	OutGraph = nullptr;
	TArray<FCortexGraphTreeEntry> Tree;
	Collect(Blueprint, Tree);
	const FCortexGraphTreeEntry* Found = ResolveInTree(Tree, Params, OutError);
	if (Found == nullptr)
	{
		return false;
	}
	OutGraph = Found->Graph;
	if (OutEntry != nullptr)
	{
		*OutEntry = *Found;
	}
	return true;
}

bool FCortexGraphTree::HasGraphSelector(const TSharedPtr<FJsonObject>& Params)
{
	return !TreeResolveParam(Params, TEXT("graph_guid")).IsEmpty()
		|| !TreeResolveParam(Params, TEXT("graph_path")).IsEmpty()
		|| !TreeResolveParam(Params, TEXT("graph_name")).IsEmpty()
		|| !TreeResolveParam(Params, TEXT("subgraph_path")).IsEmpty();
}

bool FCortexGraphTree::ResolveReadScope(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	TArray<FCortexGraphTreeEntry>& OutEntries, FCortexCommandResult& OutError)
{
	TArray<FCortexGraphTreeEntry> Tree;
	Collect(Blueprint, Tree);
	if (!HasGraphSelector(Params))
	{
		OutEntries = MoveTemp(Tree);
		return true;
	}

	const FCortexGraphTreeEntry* Target = ResolveInTree(Tree, Params, OutError);
	if (Target == nullptr)
	{
		return false;
	}

	OutEntries.Reset();
	for (const FCortexGraphTreeEntry& Entry : Tree)
	{
		for (const FCortexGraphTreeEntry* Ancestor = &Entry; Ancestor != nullptr;
			Ancestor = Ancestor->ParentIndex == INDEX_NONE ? nullptr : &Tree[Ancestor->ParentIndex])
		{
			if (Ancestor->Graph == Target->Graph)
			{
				OutEntries.Add(Entry);
				break;
			}
		}
	}
	return true;
}
