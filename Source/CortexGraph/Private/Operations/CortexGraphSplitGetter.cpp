#include "Operations/CortexGraphSplitGetter.h"

#include "CortexAssetMutationGuard.h"
#include "CortexGraphModule.h"
#include "CortexGraphNodeVariation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexGraphNodeOps.h"
#include "ScopedTransaction.h"

namespace
{
	/** A node that must be recorded in the transaction before its pins change; nodes of older add_node paths may lack RF_Transactional. */
	void RecordSplitGetterNode(UEdGraphNode* Node)
	{
		if (Node != nullptr)
		{
			Node->SetFlags(RF_Transactional);
			Node->Modify();
		}
	}

	struct FSplitGetterConsumer
	{
		UEdGraphNode* Node = nullptr;
		/** The consumer's input pins fed by the getter's value pin, in link order. */
		TArray<UEdGraphPin*> InputPins;
	};

	UEdGraphNode* FindSplitGetterNode(UEdGraph* Graph, const FString& NodeId)
	{
		FGuid NodeGuid;
		const bool bIsGuid = FGuid::Parse(NodeId, NodeGuid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && (Node->GetName() == NodeId || (bIsGuid && Node->NodeGuid == NodeGuid)))
			{
				return Node;
			}
		}
		return nullptr;
	}
}

FCortexCommandResult FCortexGraphSplitGetter::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}
	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: node_id"));
	}
	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
	}

	FCortexCommandResult LoadError;
	if (!FCortexGraphNodeOps::ValidateWritableAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// The requested path was checked by the handler, but loading follows asset redirectors: the loaded Blueprint can be a
	// quarantined asset reached through an unblocked alias.
	FString BlockReason;
	if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	UEdGraph* Graph = nullptr;
	if (!FCortexGraphNodeOps::ResolveMutableGraph(Blueprint, GraphName, Graph, LoadError))
	{
		return LoadError;
	}
	FString SubgraphPath;
	Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
	if (!SubgraphPath.IsEmpty())
	{
		Graph = FCortexGraphNodeOps::ResolveSubgraph(Graph, SubgraphPath, LoadError);
		if (Graph == nullptr)
		{
			return LoadError;
		}
	}

	UEdGraphNode* Node = FindSplitGetterNode(Graph, NodeId);
	if (Node == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node);
	if (Getter == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Node %s is a %s, not a Variable Get node"), *NodeId, *Node->GetClass()->GetName()));
	}
	const FString Variation = FCortexGraphNodeVariation::GetVariableGetVariation(Getter);
	if (Variation != FCortexGraphNodeVariation::Pure())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Variable Get node %s is %s: it executes, so it cannot be duplicated per consumer; only a pure getter can be split"),
				*NodeId, *Variation));
	}
	UEdGraphPin* ValuePin = Getter->GetValuePin();
	if (ValuePin == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Variable Get node %s has no value output pin"), *NodeId));
	}
	if (!ValuePin->SubPins.IsEmpty())
	{
		// A split struct output carries its links on the child pins; the parent's LinkedTo is empty, so counting it would
		// report no consumers. Splitting per consumer needs the pin shape reproduced, so the pin is recombined first.
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Variable Get node %s has a split output pin '%s'; recombine it before splitting the getter"),
				*NodeId, *ValuePin->PinName.ToString()));
	}

	// The distinct consumer nodes of the value pin in link order, with the pins each one takes.
	TArray<FSplitGetterConsumer> Consumers;
	for (UEdGraphPin* Linked : ValuePin->LinkedTo)
	{
		UEdGraphNode* Owner = Linked != nullptr ? Linked->GetOwningNode() : nullptr;
		if (Owner == nullptr)
		{
			continue;
		}
		FSplitGetterConsumer* Existing = Consumers.FindByPredicate([Owner](const FSplitGetterConsumer& Consumer) { return Consumer.Node == Owner; });
		if (Existing == nullptr)
		{
			FSplitGetterConsumer NewConsumer;
			NewConsumer.Node = Owner;
			Existing = &Consumers.Add_GetRef(MoveTemp(NewConsumer));
		}
		Existing->InputPins.Add(Linked);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("graph_name"), Graph->GetName());
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetStringField(TEXT("original_node_id"), Getter->GetName());
	Data->SetStringField(TEXT("original_node_guid"), Getter->NodeGuid.ToString());
	Data->SetNumberField(TEXT("consumer_count"), Consumers.Num());

	TArray<TSharedPtr<FJsonValue>> CreatedJson;
	if (Consumers.Num() < 2)
	{
		Data->SetBoolField(TEXT("changed"), false);
		Data->SetArrayField(TEXT("created"), CreatedJson);
		return FCortexCommandRouter::Success(Data);
	}

	// Every clone must be able to reproduce the value output: a getter whose variable is gone can keep a linked orphaned
	// output that a fresh node cannot recreate. Checked on a transient node before anything is added or moved.
	{
		UK2Node_VariableGet* Probe = NewObject<UK2Node_VariableGet>(Graph, UK2Node_VariableGet::StaticClass(), NAME_None, RF_Transient);
		Probe->VariableReference = Getter->VariableReference;
		Probe->AllocateDefaultPins();
		const UEdGraphPin* ProbePin = Probe->GetValuePin();
		const bool bReproduced = ProbePin != nullptr
			&& ProbePin->PinType.PinCategory == ValuePin->PinType.PinCategory
			&& ProbePin->PinType.PinSubCategory == ValuePin->PinType.PinSubCategory
			&& ProbePin->PinType.PinSubCategoryObject == ValuePin->PinType.PinSubCategoryObject
			&& ProbePin->PinType.ContainerType == ValuePin->PinType.ContainerType;
		Probe->MarkAsGarbage();
		if (!bReproduced)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Variable Get node %s: its variable reference no longer produces a matching value output, so a replacement getter cannot be created; nothing was changed"),
					*NodeId));
		}
	}

	if (bDryRun)
	{
		for (int32 Index = 1; Index < Consumers.Num(); ++Index)
		{
			TSharedRef<FJsonObject> Planned = MakeShared<FJsonObject>();
			Planned->SetStringField(TEXT("consumer_node_id"), Consumers[Index].Node->GetName());
			Planned->SetStringField(TEXT("consumer_node_guid"), Consumers[Index].Node->NodeGuid.ToString());
			CreatedJson.Add(MakeShared<FJsonValueObject>(Planned));
		}
		Data->SetBoolField(TEXT("changed"), false);
		Data->SetArrayField(TEXT("created"), CreatedJson);
		return FCortexCommandRouter::Success(Data);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Split getter %s into %d getters"), *Getter->GetName(), Consumers.Num())));
	Graph->Modify();
	RecordSplitGetterNode(Getter);

	for (int32 Index = 1; Index < Consumers.Num(); ++Index)
	{
		const FSplitGetterConsumer& Consumer = Consumers[Index];
		RecordSplitGetterNode(Consumer.Node);

		// A fresh node of the same variable: only the variable reference carries over (member, owner class, self context),
		// so no pin or link state of the original is copied by object serialization. Pins are rebuilt below.
		UK2Node_VariableGet* NewGetter = NewObject<UK2Node_VariableGet>(Graph, UK2Node_VariableGet::StaticClass(), NAME_None, RF_Transactional);
		NewGetter->VariableReference = Getter->VariableReference;
		NewGetter->CreateNewGuid();
		Graph->AddNode(NewGetter, false, false);
		NewGetter->AllocateDefaultPins();

		// Same self/target wiring: every input pin takes the original's default and links.
		for (const UEdGraphPin* OriginalPin : Getter->Pins)
		{
			if (OriginalPin == nullptr || OriginalPin->Direction != EGPD_Input)
			{
				continue;
			}
			UEdGraphPin* NewPin = NewGetter->FindPin(OriginalPin->PinName, EGPD_Input);
			if (NewPin == nullptr)
			{
				continue;
			}
			NewPin->DefaultValue = OriginalPin->DefaultValue;
			NewPin->DefaultObject = OriginalPin->DefaultObject;
			NewPin->DefaultTextValue = OriginalPin->DefaultTextValue;
			for (UEdGraphPin* Source : OriginalPin->LinkedTo)
			{
				if (Source == nullptr)
				{
					continue;
				}
				RecordSplitGetterNode(Source->GetOwningNode());
				NewPin->MakeLinkTo(Source);
			}
		}

		UEdGraphPin* NewValuePin = NewGetter->GetValuePin();
		if (NewValuePin != nullptr)
		{
			for (UEdGraphPin* ConsumerPin : Consumer.InputPins)
			{
				ValuePin->BreakLinkTo(ConsumerPin);
				NewValuePin->MakeLinkTo(ConsumerPin);
			}
		}

		int32 NewWidth = 0;
		FIntPoint NewSize(0, 0);
		if (FCortexGraphNodeOps::EstimateNodeBodySize(NewGetter, NewSize))
		{
			NewWidth = NewSize.X;
		}
		constexpr int32 SplitGetterGap = 80;
		NewGetter->NodePosX = Consumer.Node->NodePosX - SplitGetterGap - NewWidth;
		NewGetter->NodePosY = Consumer.Node->NodePosY;

		for (UEdGraphPin* ConsumerPin : Consumer.InputPins)
		{
			Consumer.Node->PinConnectionListChanged(ConsumerPin);
		}

		TSharedRef<FJsonObject> Created = MakeShared<FJsonObject>();
		Created->SetStringField(TEXT("node_id"), NewGetter->GetName());
		Created->SetStringField(TEXT("node_guid"), NewGetter->NodeGuid.ToString());
		Created->SetStringField(TEXT("consumer_node_id"), Consumer.Node->GetName());
		Created->SetStringField(TEXT("consumer_node_guid"), Consumer.Node->NodeGuid.ToString());
		CreatedJson.Add(MakeShared<FJsonValueObject>(Created));
	}

	Getter->PinConnectionListChanged(ValuePin);
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	UE_LOG(LogCortexGraph, Log, TEXT("Split getter %s in graph %s into %d getters"), *Getter->GetName(), *Graph->GetName(), Consumers.Num());

	Data->SetBoolField(TEXT("changed"), true);
	Data->SetArrayField(TEXT("created"), CreatedJson);
	return FCortexCommandRouter::Success(Data);
}
