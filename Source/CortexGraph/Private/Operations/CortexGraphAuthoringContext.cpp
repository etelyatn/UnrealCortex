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
		if (!Graph || Depth > 4)
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

	// Reject ambiguous flat name at top-level
	if (Params->HasField(TEXT("graph_name")))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Flat graph_name is ambiguous; use target.graph_ref with canonical graph_guid")
		);
	}

	// Check target object if provided
	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	const bool bHasTarget = Params->TryGetObjectField(TEXT("target"), TargetObjPtr);
	if (bHasTarget && TargetObjPtr && TargetObjPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;
		if (TargetObj->HasField(TEXT("graph_name")) && !TargetObj->HasField(TEXT("graph_ref")))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Ambiguous flat name: target requires graph_ref with canonical graph_guid or implementation")
			);
		}
		if (!TargetObj->HasField(TEXT("graph_ref")) && !TargetObj->HasField(TEXT("implementation")))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Target must specify either graph_ref or implementation")
			);
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
		const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
		if (TargetObj->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) && GraphRefPtr && GraphRefPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& GraphRef = *GraphRefPtr;
			FString TargetGuidStr;
			if (!GraphRef->TryGetStringField(TEXT("graph_guid"), TargetGuidStr) || TargetGuidStr.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("target.graph_ref requires graph_guid")
				);
			}

			FGuid TargetGuid;
			if (!FGuid::Parse(TargetGuidStr, TargetGuid))
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

			const FGraphChoiceEntry* FoundChoice = nullptr;
			for (const FGraphChoiceEntry& Choice : Choices)
			{
				if (Choice.Graph->GraphGuid == TargetGuid && Choice.SubgraphPath == SubgraphPath)
				{
					FoundChoice = &Choice;
					break;
				}
			}

			if (!FoundChoice)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::GraphNotFound,
					FString::Printf(TEXT("Graph with GUID %s not found on Blueprint"), *TargetGuidStr)
				);
			}

			FString SpecifiedKind;
			if (GraphRef->TryGetStringField(TEXT("graph_kind"), SpecifiedKind) && !SpecifiedKind.IsEmpty())
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
	Data->SetArrayField(TEXT("supported_families"), Families);

	TArray<TSharedPtr<FJsonValue>> Modes;
	Modes.Add(MakeShared<FJsonValueString>(TEXT("graph_ref")));
	Modes.Add(MakeShared<FJsonValueString>(TEXT("implementation")));
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
