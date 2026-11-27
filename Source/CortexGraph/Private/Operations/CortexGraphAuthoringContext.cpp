#include "Operations/CortexGraphAuthoringContext.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_Composite.h"

namespace
{
	struct FGraphChoiceEntry
	{
		UEdGraph* Graph = nullptr;
		ECortexGraphKind Kind = ECortexGraphKind::Function;
		FName OwningInterface = NAME_None;
		FString SubgraphPath;
		bool bIsMutable = true;
	};

	void CollectSubgraphsRecursive(
		UEdGraph* Graph,
		ECortexGraphKind Kind,
		FName OwningInterface,
		const FString& CurrentPath,
		TArray<FGraphChoiceEntry>& OutChoices,
		int32 Depth)
	{
		if (!Graph || Depth >= 4)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node)) continue;
			UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
			if (!Composite || !Composite->BoundGraph) continue;

			UEdGraph* Sub = Composite->BoundGraph;
			if (!IsValid(Sub)) continue;

			FString SubPath = CurrentPath.IsEmpty()
				? Sub->GetName()
				: FString::Printf(TEXT("%s.%s"), *CurrentPath, *Sub->GetName());

			FGraphChoiceEntry Choice;
			Choice.Graph = Sub;
			Choice.Kind = Kind;
			Choice.OwningInterface = OwningInterface;
			Choice.SubgraphPath = SubPath;
			Choice.bIsMutable = FCortexGraphNodeOps::IsMutableGraphKind(Kind);
			OutChoices.Add(Choice);

			CollectSubgraphsRecursive(Sub, Kind, OwningInterface, SubPath, OutChoices, Depth + 1);
		}
	}
}

FCortexCommandResult FCortexGraphAuthoringContext::Read(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Params object is required")
		);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	// Check target object if provided. A flat target.graph_name (no graph_ref) is a name lookup resolved after the load.
	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	const bool bHasTarget = Params->TryGetObjectField(TEXT("target"), TargetObjPtr);
	if (bHasTarget && TargetObjPtr && TargetObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
		if (!TargetObj->HasField(TEXT("graph_ref")) && !TargetObj->HasField(TEXT("implementation")) && !TargetObj->HasField(TEXT("graph_name")))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Target must specify graph_ref, graph_name or implementation")
			);
		}
	}

	// A supplied name must be a non-empty string unless a graph_ref (or implementation) takes precedence over it: a null,
	// empty or mistyped name is never treated as omitted, which would return the context without the requested target.
	{
		const bool bNameSuperseded = bHasTarget && TargetObjPtr && TargetObjPtr->IsValid()
			&& ((*TargetObjPtr)->HasField(TEXT("graph_ref")) || (*TargetObjPtr)->HasField(TEXT("implementation")));
		if (!bNameSuperseded)
		{
			auto CheckGraphNameShape = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Where, FCortexCommandResult& OutError) -> bool
			{
				if (!Object->HasField(TEXT("graph_name")))
				{
					return true;
				}
				// TryGetStringField would coerce a number or boolean into text, so the JSON type is checked directly.
				const TSharedPtr<FJsonValue> Field = Object->TryGetField(TEXT("graph_name"));
				if (Field.IsValid() && Field->Type == EJson::String && !Field->AsString().IsEmpty())
				{
					return true;
				}
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%sgraph_name must be a non-empty string"), Where));
				return false;
			};
			FCortexCommandResult NameShapeError;
			if (!CheckGraphNameShape(Params, TEXT(""), NameShapeError)
				|| (bHasTarget && TargetObjPtr && TargetObjPtr->IsValid() && !CheckGraphNameShape(*TargetObjPtr, TEXT("target."), NameShapeError)))
			{
				return NameShapeError;
			}
		}
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return LoadError;
	}

	// Validate class context readiness
	if (Blueprint->ParentClass == nullptr || Blueprint->GeneratedClass == nullptr || Blueprint->Status == BS_BeingCreated)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Blueprint class context is unready: missing ParentClass or GeneratedClass")
		);
	}

	// Enumerate user graphs
	TArray<FCortexGraphEntry> Entries;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);

	TArray<FGraphChoiceEntry> Choices;
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (!Entry.Graph) continue;
		FGraphChoiceEntry Choice;
		Choice.Graph = Entry.Graph;
		Choice.Kind = Entry.Kind;
		Choice.OwningInterface = Entry.OwningInterface;
		Choice.SubgraphPath = TEXT("");
		Choice.bIsMutable = FCortexGraphNodeOps::IsMutableGraphKind(Entry.Kind);
		Choices.Add(Choice);

		CollectSubgraphsRecursive(Entry.Graph, Entry.Kind, Entry.OwningInterface, TEXT(""), Choices, 0);
	}

	TArray<TSharedPtr<FJsonValue>> GraphChoicesArray;
	for (const FGraphChoiceEntry& Choice : Choices)
	{
		TSharedRef<FJsonObject> ChoiceObj = MakeShared<FJsonObject>();
		ChoiceObj->SetStringField(TEXT("graph_guid"), Choice.Graph->GraphGuid.ToString());
		ChoiceObj->SetStringField(TEXT("graph_name"), Choice.Graph->GetName());
		ChoiceObj->SetStringField(TEXT("graph_kind"), FCortexGraphNodeOps::GraphKindToString(Choice.Kind));
		ChoiceObj->SetBoolField(TEXT("is_mutable"), Choice.bIsMutable);
		ChoiceObj->SetNumberField(TEXT("node_count"), Choice.Graph->Nodes.Num());
		if (Choice.OwningInterface != NAME_None)
		{
			ChoiceObj->SetStringField(TEXT("owning_interface"), Choice.OwningInterface.ToString());
		}
		if (!Choice.SubgraphPath.IsEmpty())
		{
			ChoiceObj->SetStringField(TEXT("subgraph_path"), Choice.SubgraphPath);
		}
		GraphChoicesArray.Add(MakeShared<FJsonValueObject>(ChoiceObj));
	}

	TSharedPtr<FJsonObject> ResolvedTargetObj;

	if (bHasTarget && TargetObjPtr && TargetObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
		if (TargetObj->HasField(TEXT("implementation")))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::UnsupportedOperation,
				TEXT("Implementation target resolution is not yet supported")
			);
		}

		const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
		if (TargetObj->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) && GraphRefPtr && GraphRefPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& GraphRef = *GraphRefPtr;
			FString TargetGuidStr;
			FString GraphPathStr;
			GraphRef->TryGetStringField(TEXT("graph_guid"), TargetGuidStr);
			GraphRef->TryGetStringField(TEXT("graph_path"), GraphPathStr);
			if (TargetGuidStr.IsEmpty() && GraphPathStr.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("target.graph_ref requires graph_guid or graph_path")
				);
			}

			FGuid TargetGuid;
			if (!TargetGuidStr.IsEmpty() && !FGuid::Parse(TargetGuidStr, TargetGuid))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Invalid graph_guid format: '%s'"), *TargetGuidStr)
				);
			}

			FString SubgraphPath;
			GraphRef->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);

			if (!SubgraphPath.IsEmpty())
			{
				TArray<FString> Segments;
				SubgraphPath.ParseIntoArray(Segments, TEXT("."), false);
				for (const FString& Seg : Segments)
				{
					if (Seg.TrimStartAndEnd().IsEmpty())
					{
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							TEXT("Invalid subgraph_path: contains empty segment")
						);
					}
				}
				if (Segments.Num() > 4)
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::SubgraphDepthExceeded,
						TEXT("Subgraph path exceeds max depth of 4")
					);
				}
			}

			// `graph_guid` names the target graph (a root graph or a bound composite child). The
			// identity is the lookup; a supplied `subgraph_path` must equal that target's own
			// root-relative path, so an omitted path resolves by identity alone and a pair that
			// disagrees is refused instead of resolved to a different graph. A `graph_path` (the path
			// list_graphs publishes) selects among graphs that share a GraphGuid and is resolved by the
			// shared tree rules below.
			const FGraphChoiceEntry* FoundChoice = nullptr;
			if (GraphPathStr.IsEmpty())
			{
				for (const FGraphChoiceEntry& Choice : Choices)
				{
					if (Choice.Graph->GraphGuid == TargetGuid)
					{
						FoundChoice = &Choice;
						break;
					}
				}
			}

			if (FoundChoice && !SubgraphPath.IsEmpty() && FoundChoice->SubgraphPath != SubgraphPath)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("subgraph_path '%s' does not name the graph identified by graph_guid %s; that graph's path is '%s'"),
						*SubgraphPath, *TargetGuidStr, *FoundChoice->SubgraphPath)
				);
			}

			// A graph nested below a root graph that is not an authoring choice (Animation Blueprint state, transition and
			// conduit graphs, or any graph a node publishes through GetSubGraphs) resolves read-only: its kind is reported
			// and is_mutable is false. It is never a graph_choices entry and the patch route still refuses it.
			TArray<FCortexGraphTreeEntry> Tree;
			const FCortexGraphTreeEntry* FoundNested = nullptr;
			if (!FoundChoice)
			{
				FCortexGraphTree::Collect(Blueprint, Tree);
				TSharedPtr<FJsonObject> IdentityParams = MakeShared<FJsonObject>();
				if (!TargetGuidStr.IsEmpty())
				{
					IdentityParams->SetStringField(TEXT("graph_guid"), TargetGuidStr);
				}
				if (!GraphPathStr.IsEmpty())
				{
					IdentityParams->SetStringField(TEXT("graph_path"), GraphPathStr);
				}
				if (!SubgraphPath.IsEmpty())
				{
					IdentityParams->SetStringField(TEXT("subgraph_path"), SubgraphPath);
				}

				// The shared resolver: unknown identity GRAPH_NOT_FOUND, a shared GUID INVALID_FIELD listing the paths,
				// graph_guid/graph_path/subgraph_path must agree.
				FCortexCommandResult ResolveError;
				const FCortexGraphTreeEntry* Resolved = FCortexGraphTree::ResolveInTree(Tree, IdentityParams, ResolveError);
				if (Resolved == nullptr)
				{
					return ResolveError;
				}

				// A path can also name a root or composite authoring choice.
				for (const FGraphChoiceEntry& Choice : Choices)
				{
					if (Choice.Graph == Resolved->Graph)
					{
						FoundChoice = &Choice;
						break;
					}
				}
				if (FoundChoice == nullptr)
				{
					FoundNested = Resolved;
				}
			}

			FString SpecifiedKind;
			GraphRef->TryGetStringField(TEXT("graph_kind"), SpecifiedKind);
			if (FoundNested != nullptr)
			{
				if (!SpecifiedKind.IsEmpty() && SpecifiedKind != FoundNested->Kind)
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Graph kind mismatch: specified '%s' but graph is '%s'"), *SpecifiedKind, *FoundNested->Kind)
					);
				}

				ResolvedTargetObj = MakeShared<FJsonObject>();
				ResolvedTargetObj->SetStringField(TEXT("graph_guid"), FoundNested->Graph->GraphGuid.ToString());
				ResolvedTargetObj->SetStringField(TEXT("graph_name"), FoundNested->Graph->GetName());
				ResolvedTargetObj->SetStringField(TEXT("graph_kind"), FoundNested->Kind);
				ResolvedTargetObj->SetBoolField(TEXT("is_mutable"), false);
				ResolvedTargetObj->SetStringField(TEXT("graph_path"), FoundNested->Path);
				ResolvedTargetObj->SetStringField(TEXT("parent_graph_guid"), FoundNested->ParentGraphGuid.ToString());
				if (!FoundNested->SubgraphPath.IsEmpty())
				{
					ResolvedTargetObj->SetStringField(TEXT("subgraph_path"), FoundNested->SubgraphPath);
				}
			}
			else
			{
				if (!SpecifiedKind.IsEmpty())
				{
					FString ActualKind = FCortexGraphNodeOps::GraphKindToString(FoundChoice->Kind);
					if (SpecifiedKind != ActualKind)
					{
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Graph kind mismatch: specified '%s' but graph is '%s'"), *SpecifiedKind, *ActualKind)
						);
					}
				}

				if (FoundChoice->Kind == ECortexGraphKind::Delegate || !FoundChoice->bIsMutable)
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidOperation,
						TEXT("Delegate signature graphs are read-only and cannot be targeted for authoring")
					);
				}

				ResolvedTargetObj = MakeShared<FJsonObject>();
				ResolvedTargetObj->SetStringField(TEXT("graph_guid"), FoundChoice->Graph->GraphGuid.ToString());
				ResolvedTargetObj->SetStringField(TEXT("graph_name"), FoundChoice->Graph->GetName());
				ResolvedTargetObj->SetStringField(TEXT("graph_kind"), FCortexGraphNodeOps::GraphKindToString(FoundChoice->Kind));
				ResolvedTargetObj->SetBoolField(TEXT("is_mutable"), FoundChoice->bIsMutable);
				if (!FoundChoice->SubgraphPath.IsEmpty())
				{
					ResolvedTargetObj->SetStringField(TEXT("subgraph_path"), FoundChoice->SubgraphPath);
				}
			}
		}
	}

	// A graph name (top-level graph_name, or target.graph_name without graph_ref) resolves like the other graph reads.
	// graph_ref keeps precedence; an implementation target was already refused above.
	{
		const bool bTargetHasRefOrImplementation = bHasTarget && TargetObjPtr && TargetObjPtr->IsValid()
			&& ((*TargetObjPtr)->HasField(TEXT("graph_ref")) || (*TargetObjPtr)->HasField(TEXT("implementation")));
		FString RequestedGraphName;
		Params->TryGetStringField(TEXT("graph_name"), RequestedGraphName);
		if (RequestedGraphName.IsEmpty() && bHasTarget && TargetObjPtr && TargetObjPtr->IsValid())
		{
			(*TargetObjPtr)->TryGetStringField(TEXT("graph_name"), RequestedGraphName);
		}

		if (!bTargetHasRefOrImplementation && !RequestedGraphName.IsEmpty())
		{
			TArray<FCortexGraphTreeEntry> NameTree;
			FCortexGraphTree::Collect(Blueprint, NameTree);

			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (const FCortexGraphTreeEntry& TreeEntry : NameTree)
			{
				if (TreeEntry.bRoot && TreeEntry.Graph->GetName() == RequestedGraphName)
				{
					TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
					Candidate->SetStringField(TEXT("graph_guid"), TreeEntry.Graph->GraphGuid.ToString());
					Candidate->SetStringField(TEXT("graph_name"), TreeEntry.Graph->GetName());
					Candidate->SetStringField(TEXT("graph_kind"), TreeEntry.Kind);
					Candidate->SetStringField(TEXT("subgraph_path"), TreeEntry.SubgraphPath);
					Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
				}
			}
			if (Candidates.Num() > 1)
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				const int32 CandidateCount = Candidates.Num();
				Details->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("graph_name '%s' matches %d graphs; address one with target.graph_ref and its graph_guid from details.candidates"),
						*RequestedGraphName, CandidateCount),
					Details);
			}

			TSharedPtr<FJsonObject> NameParams = MakeShared<FJsonObject>();
			NameParams->SetStringField(TEXT("graph_name"), RequestedGraphName);
			FCortexCommandResult NameError;
			const FCortexGraphTreeEntry* NameResolved = FCortexGraphTree::ResolveInTree(NameTree, NameParams, NameError);
			if (NameResolved == nullptr)
			{
				return NameError;
			}

			const FGraphChoiceEntry* NameChoice = nullptr;
			for (const FGraphChoiceEntry& Choice : Choices)
			{
				if (Choice.Graph == NameResolved->Graph)
				{
					NameChoice = &Choice;
					break;
				}
			}
			if (NameChoice == nullptr)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::GraphNotFound,
					FString::Printf(TEXT("Graph not found: %s"), *RequestedGraphName));
			}
			if (NameChoice->Kind == ECortexGraphKind::Delegate || !NameChoice->bIsMutable)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidOperation,
					TEXT("Delegate signature graphs are read-only and cannot be targeted for authoring")
				);
			}

			ResolvedTargetObj = MakeShared<FJsonObject>();
			ResolvedTargetObj->SetStringField(TEXT("graph_guid"), NameChoice->Graph->GraphGuid.ToString());
			ResolvedTargetObj->SetStringField(TEXT("graph_name"), NameChoice->Graph->GetName());
			ResolvedTargetObj->SetStringField(TEXT("graph_kind"), FCortexGraphNodeOps::GraphKindToString(NameChoice->Kind));
			ResolvedTargetObj->SetBoolField(TEXT("is_mutable"), NameChoice->bIsMutable);
			if (!NameChoice->SubgraphPath.IsEmpty())
			{
				ResolvedTargetObj->SetStringField(TEXT("subgraph_path"), NameChoice->SubgraphPath);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Data->SetStringField(TEXT("asset_class"), Blueprint->GetClass()->GetName());
	Data->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	Data->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	Data->SetArrayField(TEXT("graph_choices"), GraphChoicesArray);

	if (ResolvedTargetObj.IsValid())
	{
		Data->SetObjectField(TEXT("target"), ResolvedTargetObj);
	}

	Data->SetObjectField(TEXT("fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));

	TArray<TSharedPtr<FJsonValue>> Families;
	Families.Add(MakeShared<FJsonValueString>(TEXT("CallFunction")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("VariableGet")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("VariableSet")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("Self")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("DynamicCast")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("ConstructObject")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("Event")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("PropertyAccess")));
	Families.Add(MakeShared<FJsonValueString>(TEXT("Message")));
	Data->SetArrayField(TEXT("supported_families"), Families);

	TArray<TSharedPtr<FJsonValue>> Modes;
	Modes.Add(MakeShared<FJsonValueString>(TEXT("graph_ref")));
	Data->SetArrayField(TEXT("supported_authoring_modes"), Modes);

	TSharedRef<FJsonObject> LimitsObj = MakeShared<FJsonObject>();
	LimitsObj->SetNumberField(TEXT("max_nodes"), 64);
	LimitsObj->SetNumberField(TEXT("max_edges"), 256);
	LimitsObj->SetNumberField(TEXT("max_client_id_length"), 32);
	LimitsObj->SetNumberField(TEXT("max_request_size_bytes"), 65536);
	LimitsObj->SetNumberField(TEXT("max_scanned_nodes"), 2048);
	LimitsObj->SetNumberField(TEXT("default_selector_page_size"), 32);
	LimitsObj->SetNumberField(TEXT("max_selector_page_size"), 128);
	LimitsObj->SetNumberField(TEXT("max_error_candidates"), 16);
	Data->SetObjectField(TEXT("limits"), LimitsObj);

	return FCortexCommandRouter::Success(Data);
}
