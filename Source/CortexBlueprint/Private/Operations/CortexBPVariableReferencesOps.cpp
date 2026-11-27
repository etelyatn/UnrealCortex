#include "Operations/CortexBPVariableReferencesOps.h"

#include "CortexBlueprintModule.h"
#include "CortexGraphTree.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGraphExecReach.h"
#include "Operations/CortexBPGraphMetricsOps.h"
#include "Operations/CortexBPGraphReachability.h"
#include "UObject/UnrealType.h"

namespace
{
	/** The variable a find_variable_references request names: its declaring owner decides which nodes refer to it. */
	struct FVarRefTarget
	{
		FName Name;
		/** Authoritative class that declares the variable (null only for an uncompiled Blueprint variable). */
		const UClass* OwnerClass = nullptr;
		FString DeclaredIn;
		bool bNative = false;
	};

	using FVarRefEntryReach = FCortexBPGraphMetricsOps::FLiveEntryReach;
	bool ResolveVarRefTarget(const UBlueprint* Blueprint, const FString& Variable, FVarRefTarget& OutTarget)
	{
		OutTarget.Name = FName(*Variable);
		const bool bBlueprintVariable = Blueprint->NewVariables.ContainsByPredicate(
			[&OutTarget](const FBPVariableDescription& Candidate) { return Candidate.VarName == OutTarget.Name; });
		if (bBlueprintVariable)
		{
			const UClass* Own = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
			OutTarget.OwnerClass = Own != nullptr ? Own->GetAuthoritativeClass() : nullptr;
			OutTarget.DeclaredIn = Blueprint->GetPathName();
			OutTarget.bNative = false;
			return true;
		}

		const UClass* Scope = Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass.Get()
			: (Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get());
		const FProperty* Property = Scope != nullptr ? FindFProperty<FProperty>(Scope, OutTarget.Name) : nullptr;
		if (Property == nullptr)
		{
			return false;
		}
		const UClass* Owner = Property->GetOwnerClass();
		if (Owner == nullptr)
		{
			return false;
		}
		OutTarget.OwnerClass = Owner->GetAuthoritativeClass();
		OutTarget.bNative = Owner->HasAnyClassFlags(CLASS_Native);
		const UBlueprint* OwnerBlueprint = Cast<UBlueprint>(Owner->ClassGeneratedBy);
		OutTarget.DeclaredIn = (!OutTarget.bNative && OwnerBlueprint != nullptr) ? OwnerBlueprint->GetPathName() : Owner->GetPathName();
		return true;
	}

	/**
	 * True when the get/set node refers to the target member: the property UE resolves the reference to (redirects and member GUID
	 * renames followed, on a COPY so the node is never rewritten) has the target's name and declaring owner. A local variable never
	 * does. An unresolvable reference falls back to its stored name.
	 */
	bool VarRefNodeRefersToTarget(const UK2Node_Variable* Node, const FVarRefTarget& Target)
	{
		const FMemberReference& Reference = Node->VariableReference;
		if (Reference.IsLocalScope())
		{
			return false;
		}
		FMemberReference Resolver = Reference;
		const FProperty* Resolved = Resolver.ResolveMember<FProperty>(Node->GetBlueprintClassFromNode(), true);
		if (Resolved != nullptr)
		{
			const UClass* Owner = Resolved->GetOwnerClass();
			return Resolved->GetFName() == Target.Name && Owner != nullptr
				&& (Target.OwnerClass == nullptr || Owner->GetAuthoritativeClass() == Target.OwnerClass);
		}
		if (Reference.GetMemberName() != Target.Name)
		{
			return false;
		}
		if (Reference.IsSelfContext())
		{
			return true;
		}
		const UClass* Parent = Reference.GetMemberParentClass(Node->GetBlueprintClassFromNode());
		return Parent != nullptr && Target.OwnerClass != nullptr && Parent->GetAuthoritativeClass()->IsChildOf(Target.OwnerClass);
	}

	/** Distinct consumer nodes of every output pin, reroute knots passed through; the first consuming pin names each node. */
	void CollectVarRefConsumers(const UEdGraphNode* Node, TArray<TSharedPtr<FJsonValue>>& OutConsumers)
	{
		TSet<const UEdGraphNode*> Seen;
		TSet<const UEdGraphNode*> VisitedKnots;
		TArray<const UEdGraphPin*> Work;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Output)
			{
				Work.Add(Pin);
			}
		}
		// Process in pin order: Work is consumed from the front through an index so the order stays deterministic.
		for (int32 Index = 0; Index < Work.Num(); ++Index)
		{
			for (const UEdGraphPin* Linked : Work[Index]->LinkedTo)
			{
				const UEdGraphNode* Consumer = Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr;
				if (Consumer == nullptr)
				{
					continue;
				}
				if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Consumer))
				{
					if (!VisitedKnots.Contains(Knot))
					{
						VisitedKnots.Add(Knot);
						Work.Add(Knot->GetOutputPin());
					}
					continue;
				}
				if (Seen.Contains(Consumer))
				{
					continue;
				}
				Seen.Add(Consumer);
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("node_guid"), Consumer->NodeGuid.ToString());
				Row->SetStringField(TEXT("node_class"), Consumer->GetClass()->GetName());
				Row->SetStringField(TEXT("title"), Consumer->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Row->SetStringField(TEXT("pin"), Linked->PinName.ToString());
				OutConsumers.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
	}
}

FCortexCommandResult FCortexBPVariableReferencesOps::FindVariableReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Variable;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	if (!Params->TryGetStringField(TEXT("variable"), Variable) || Variable.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: variable"));
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FVarRefTarget Target;
	if (!ResolveVarRefTarget(Blueprint, Variable, Target))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is neither a variable of %s nor a property it inherits"), *Variable, *Blueprint->GetName()));
	}

	TArray<FCortexGraphTreeEntry> Tree;
	FCortexGraphTree::Collect(Blueprint, Tree);
	TArray<FCortexGraphTreeEntry> CutEntries;
	TArray<TSharedPtr<FJsonValue>> References;
	int32 GetCount = 0;
	int32 SetCount = 0;
	// Entry reach walks the whole Blueprint, so it is built at most once per request, on the first matching node.
	TOptional<TArray<FVarRefEntryReach>> EntryReach;
	for (const FCortexGraphTreeEntry& TreeEntry : Tree)
	{
		if (TreeEntry.RootKind != ECortexGraphKind::Ubergraph && TreeEntry.RootKind != ECortexGraphKind::Function
			&& TreeEntry.RootKind != ECortexGraphKind::Macro && TreeEntry.RootKind != ECortexGraphKind::InterfaceImpl)
		{
			continue;
		}
		if (TreeEntry.bChildrenTruncated)
		{
			CutEntries.Add(TreeEntry);
		}
		const UEdGraph* Graph = TreeEntry.Graph;
		if (Graph == nullptr)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node);
			const UK2Node_VariableSet* Setter = Cast<UK2Node_VariableSet>(Node);
			if ((Getter == nullptr && Setter == nullptr) || !VarRefNodeRefersToTarget(Getter != nullptr ? static_cast<const UK2Node_Variable*>(Getter) : Setter, Target))
			{
				continue;
			}
			if (Getter != nullptr)
			{
				++GetCount;
			}
			else
			{
				++SetCount;
			}
			if (!EntryReach.IsSet())
			{
				// Entries come from every graph of the tree: a getter inside a collapsed body is reached by the outer entry
				// that expands the composite, and carries that entry's identity.
				EntryReach.Emplace();
				for (const FCortexGraphTreeEntry& EntryGraph : Tree)
				{
					if (EntryGraph.Graph != nullptr)
					{
						FCortexBPGraphMetricsOps::CollectLiveEntryReaches(EntryGraph.Graph, EntryReach.GetValue());
					}
				}
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), Getter != nullptr ? TEXT("get") : TEXT("set"));
			Row->SetStringField(TEXT("node_id"), Node->GetName());
			Row->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			Row->SetStringField(TEXT("graph_name"), Graph->GetName());
			Row->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
			if (!TreeEntry.bRoot)
			{
				Row->SetStringField(TEXT("graph_path"), TreeEntry.Path);
			}
			if (!TreeEntry.SubgraphPath.IsEmpty())
			{
				Row->SetStringField(TEXT("subgraph_path"), TreeEntry.SubgraphPath);
			}

			TArray<TSharedPtr<FJsonValue>> Entries;
			for (const FVarRefEntryReach& Reach : EntryReach.GetValue())
			{
				if (Reach.Reached.Contains(Node))
				{
					TSharedPtr<FJsonObject> EntryRow = MakeShared<FJsonObject>();
					EntryRow->SetStringField(TEXT("name"), Reach.Name);
					EntryRow->SetStringField(TEXT("node_guid"), Reach.NodeGuid.ToString());
					Entries.Add(MakeShared<FJsonValueObject>(EntryRow));
				}
			}
			Row->SetArrayField(TEXT("entries"), Entries);

			TArray<TSharedPtr<FJsonValue>> Consumers;
			CollectVarRefConsumers(Node, Consumers);
			Row->SetArrayField(TEXT("consumers"), Consumers);
			References.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("variable"), Variable);
	Data->SetStringField(TEXT("declared_in"), Target.DeclaredIn);
	Data->SetBoolField(TEXT("is_native"), Target.bNative);
	Data->SetNumberField(TEXT("get_count"), GetCount);
	Data->SetNumberField(TEXT("set_count"), SetCount);
	Data->SetArrayField(TEXT("references"), References);
	FCortexGraphTree::AppendTruncation(CutEntries, Data);
	return FCortexCommandRouter::Success(Data);
}
