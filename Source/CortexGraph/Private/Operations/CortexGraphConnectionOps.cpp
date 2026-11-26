#include "Operations/CortexGraphConnectionOps.h"
#include "Operations/CortexGraphNodeOps.h"
#include "CortexBatchScope.h"
#include "CortexEditorUtils.h"
#include "CortexGraphModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Composite.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace
{
FString GetGraphConnectionWritableValidationPath(const FString& AssetPath)
{
	static const FString LevelBPPrefix = TEXT("__level_bp__:");
	const FString BlueprintPath = AssetPath.StartsWith(LevelBPPrefix)
		? AssetPath.Mid(LevelBPPrefix.Len())
		: AssetPath;
	const FString NormalizedPath = FCortexEditorUtils::NormalizeMountedContentPath(BlueprintPath);
	return FPackageName::ObjectPathToPackageName(NormalizedPath);
}

bool ValidateWritableGraphConnectionBlueprintAssetPath(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString ValidationError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(GetGraphConnectionWritableValidationPath(AssetPath), ValidationError))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
		return false;
	}

	return true;
}

bool ResolveMutableConnectionGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	UEdGraph*& OutGraph,
	FCortexCommandResult& OutError)
{
	FCortexGraphEntry Entry;
	if (!FCortexGraphNodeOps::FindGraphEntry(Blueprint, GraphName, Entry))
	{
		const FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::GraphNotFound,
			FString::Printf(TEXT("Graph not found: %s"), *TargetName)
		);
		return false;
	}

	if (!FCortexGraphNodeOps::IsMutableGraphKind(Entry.Kind))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(
				TEXT("Graph kind is not mutable through graph_cmd: %s"),
				*FCortexGraphNodeOps::GraphKindToString(Entry.Kind))
		);
		return false;
	}

	OutGraph = Entry.Graph;
	return true;
}

UEdGraph* ResolveRollbackGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	const FString& SubgraphPath)
{
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	FCortexCommandResult ResolveError;
	UEdGraph* Graph = FCortexGraphNodeOps::FindGraph(Blueprint, GraphName, ResolveError);
	if (Graph != nullptr && !SubgraphPath.IsEmpty())
	{
		Graph = FCortexGraphNodeOps::ResolveSubgraph(Graph, SubgraphPath, ResolveError);
	}
	return Graph;
}

UEdGraphNode* ResolveRollbackNode(UEdGraph* Graph, const FGuid& NodeGuid, const FString& NodeId)
{
	if (Graph == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node != nullptr && (Node->NodeGuid == NodeGuid || Node->GetName() == NodeId))
		{
			return Node;
		}
	}
	return nullptr;
}
}

FCortexCommandResult FCortexGraphConnectionOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SourceNodeId;
	FString SourcePinName;
	FString TargetNodeId;
	FString TargetPinName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath);

	if (bHasParams)
	{
		// Accept aliases: from_node/to_node, source_node_id/target_node_id
		if (!Params->TryGetStringField(TEXT("source_node"), SourceNodeId))
			if (!Params->TryGetStringField(TEXT("from_node"), SourceNodeId))
				Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId);
		if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
			Params->TryGetStringField(TEXT("from_pin"), SourcePinName);
		if (!Params->TryGetStringField(TEXT("target_node"), TargetNodeId))
			if (!Params->TryGetStringField(TEXT("to_node"), TargetNodeId))
				Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId);
		if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
			Params->TryGetStringField(TEXT("to_pin"), TargetPinName);

		bHasParams = !SourceNodeId.IsEmpty() && !SourcePinName.IsEmpty()
			&& !TargetNodeId.IsEmpty() && !TargetPinName.IsEmpty();
	}

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, source_node (or from_node), source_pin (or from_pin), target_node (or to_node), target_pin (or to_pin)")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	// Load blueprint using helper
	FCortexCommandResult LoadError;
	if (!ValidateWritableGraphConnectionBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = nullptr;
	if (!ResolveMutableConnectionGraph(Blueprint, GraphName, Graph, LoadError))
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
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

	// Find source node and pin
	UEdGraphNode* SourceNode = FCortexGraphNodeOps::FindNode(Graph, SourceNodeId, LoadError);
	if (SourceNode == nullptr)
	{
		return LoadError;
	}

	UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
	if (SourcePin == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
			FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId));
	}

	// Find target node and pin
	UEdGraphNode* TargetNode = FCortexGraphNodeOps::FindNode(Graph, TargetNodeId, LoadError);
	if (TargetNode == nullptr)
	{
		return LoadError;
	}

	UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
	if (TargetPin == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
			FString::Printf(TEXT("Target pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId));
	}

	// Check if already connected
	if (SourcePin->LinkedTo.Contains(TargetPin))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ConnectionExists,
			TEXT("Pins are already connected"));
	}

	// Validate connection via schema BEFORE creating transaction
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema != nullptr)
	{
		FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::PinTypeMismatch,
				FString::Printf(TEXT("Cannot connect: %s"), *Response.Message.ToString()));
		}
		// Inside a rollback-enabled batch the journal can only undo direct links. A connection
		// that would displace an existing link or spawn conversion/promotion nodes cannot be
		// restored or verified atomically, so reject it up front instead of reporting a clean
		// rollback while the displaced topology remains. Non-rollback batches keep the schema
		// default (break-others / conversion are normal editor behavior).
		if (FCortexCommandRouter::IsInBatch() && FCortexBatchScope::IsRollbackEnabled()
			&& Response.Response != CONNECT_RESPONSE_MAKE)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(
					TEXT("Connection requires replacing an existing link or creating a conversion node (%s); not supported inside rollback-enabled batches"),
					*Response.Message.ToString()));
		}
	}

	// Create transaction only after validation passes
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex:Connect Pins")));
	Graph->Modify();

	// Make the connection (schema handles breaking existing connections if needed)
	if (Schema != nullptr)
	{
		if (!Schema->TryCreateConnection(SourcePin, TargetPin))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::PinTypeMismatch,
				TEXT("Schema rejected the connection attempt"));
		}
	}
	else
	{
		SourcePin->MakeLinkTo(TargetPin);
	}

	// Record the mutation for failure-atomic batch rollback: a failing batch breaks every
	// connection it created (reverse journal order) and verifies the link is gone.
	// All early-return error paths above already ran, so the link is established here.
	if (FCortexCommandRouter::IsInBatch())
	{
		const TWeakObjectPtr<UBlueprint> WeakBlueprint = Blueprint;
		const FString RootGraphName = GraphName;
		const FString JournalSubgraphPath = SubgraphPath;
		const FGuid SourceNodeGuid = SourceNode->NodeGuid;
		const FGuid TargetNodeGuid = TargetNode->NodeGuid;
		FCortexBatchScope::RegisterRollbackEntry(
			TEXT("connect"),
			TEXT(""),
			FString::Printf(TEXT("connect %s -> %s"), *SourcePin->PinName.ToString(), *TargetPin->PinName.ToString()),
			[WeakBlueprint, RootGraphName, JournalSubgraphPath, SourceNodeGuid, TargetNodeGuid,
				SourceNodeId, TargetNodeId, SourcePinName, TargetPinName]() -> bool
			{
				UEdGraph* CurrentGraph = ResolveRollbackGraph(
					WeakBlueprint.Get(), RootGraphName, JournalSubgraphPath);
				UEdGraphNode* CurrentSourceNode = ResolveRollbackNode(CurrentGraph, SourceNodeGuid, SourceNodeId);
				UEdGraphNode* CurrentTargetNode = ResolveRollbackNode(CurrentGraph, TargetNodeGuid, TargetNodeId);
				UEdGraphPin* CurrentSourcePin = CurrentSourceNode
					? CurrentSourceNode->FindPin(FName(*SourcePinName)) : nullptr;
				UEdGraphPin* CurrentTargetPin = CurrentTargetNode
					? CurrentTargetNode->FindPin(FName(*TargetPinName)) : nullptr;
				if (CurrentSourcePin == nullptr || CurrentTargetPin == nullptr)
				{
					return false;
				}
				if (CurrentSourcePin->LinkedTo.Contains(CurrentTargetPin))
				{
					CurrentSourcePin->BreakLinkTo(CurrentTargetPin);
				}
				return true;
			},
			[WeakBlueprint, RootGraphName, JournalSubgraphPath, SourceNodeGuid, TargetNodeGuid,
				SourceNodeId, TargetNodeId, SourcePinName, TargetPinName]() -> bool
			{
				UEdGraph* CurrentGraph = ResolveRollbackGraph(
					WeakBlueprint.Get(), RootGraphName, JournalSubgraphPath);
				UEdGraphNode* CurrentSourceNode = ResolveRollbackNode(CurrentGraph, SourceNodeGuid, SourceNodeId);
				UEdGraphNode* CurrentTargetNode = ResolveRollbackNode(CurrentGraph, TargetNodeGuid, TargetNodeId);
				UEdGraphPin* CurrentSourcePin = CurrentSourceNode
					? CurrentSourceNode->FindPin(FName(*SourcePinName)) : nullptr;
				UEdGraphPin* CurrentTargetPin = CurrentTargetNode
					? CurrentTargetNode->FindPin(FName(*TargetPinName)) : nullptr;
				return CurrentSourcePin != nullptr && CurrentTargetPin != nullptr
					&& !CurrentSourcePin->LinkedTo.Contains(CurrentTargetPin);
			},
			SourcePin ? Cast<UPackage>(SourcePin->GetOwningNode()->GetOutermost()) : nullptr);
	}

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("connected"), true);
	Data->SetStringField(TEXT("source"), FString::Printf(TEXT("%s.%s"), *SourceNodeId, *SourcePinName));
	Data->SetStringField(TEXT("target"), FString::Printf(TEXT("%s.%s"), *TargetNodeId, *TargetPinName));

	UE_LOG(LogCortexGraph, Log, TEXT("Connected %s.%s -> %s.%s"),
		*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphConnectionOps::Disconnect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	// Two shapes: node_id + pin_name breaks every link on that pin; graph.connect's
	// source_node + source_pin + target_node + target_pin breaks exactly that one link.
	const bool bPinShape = Params->HasField(TEXT("node_id")) || Params->HasField(TEXT("pin_name"));
	const bool bLinkShape = Params->HasField(TEXT("source_node")) || Params->HasField(TEXT("source_pin"))
		|| Params->HasField(TEXT("target_node")) || Params->HasField(TEXT("target_pin"));
	if (bPinShape && bLinkShape)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("Use either node_id + pin_name (break every link on the pin) or source_node + source_pin + target_node + target_pin (break one link), not both"));
	}

	FString NodeId;
	FString PinName;
	FString TargetNodeId;
	FString TargetPinName;
	bool bHasParams = false;
	if (bLinkShape)
	{
		bHasParams = Params->TryGetStringField(TEXT("source_node"), NodeId) && !NodeId.IsEmpty()
			&& Params->TryGetStringField(TEXT("source_pin"), PinName) && !PinName.IsEmpty()
			&& Params->TryGetStringField(TEXT("target_node"), TargetNodeId) && !TargetNodeId.IsEmpty()
			&& Params->TryGetStringField(TEXT("target_pin"), TargetPinName) && !TargetPinName.IsEmpty();
	}
	else
	{
		bHasParams = Params->TryGetStringField(TEXT("node_id"), NodeId)
			&& Params->TryGetStringField(TEXT("pin_name"), PinName);
	}

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			bLinkShape
				? TEXT("Missing required params for one link: source_node, source_pin, target_node, target_pin")
				: TEXT("Missing required params: node_id and pin_name, or source_node, source_pin, target_node and target_pin")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	// Load blueprint using helper
	FCortexCommandResult LoadError;
	if (!ValidateWritableGraphConnectionBlueprintAssetPath(AssetPath, LoadError))
	{
		return LoadError;
	}

	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// Find graph using helper
	UEdGraph* Graph = nullptr;
	if (!ResolveMutableConnectionGraph(Blueprint, GraphName, Graph, LoadError))
	{
		return LoadError;
	}

	// Resolve subgraph path if provided
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

	// Find node and pin
	UEdGraphNode* Node = FCortexGraphNodeOps::FindNode(Graph, NodeId, LoadError);
	if (Node == nullptr)
	{
		return LoadError;
	}

	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (Pin == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
			FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId));
	}

	if (bLinkShape)
	{
		UEdGraphNode* TargetNode = FCortexGraphNodeOps::FindNode(Graph, TargetNodeId, LoadError);
		if (TargetNode == nullptr)
		{
			return LoadError;
		}
		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
		if (TargetPin == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
				FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId));
		}
		if (!Pin->LinkedTo.Contains(TargetPin))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Pins are not connected: %s.%s -> %s.%s"), *NodeId, *PinName, *TargetNodeId, *TargetPinName));
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex:Disconnect Link")));
		Graph->Modify();
		// Cortex-created nodes lack RF_Transactional; record both ends so one undo restores the link.
		for (UEdGraphNode* LinkEnd : { Node, TargetNode })
		{
			LinkEnd->SetFlags(RF_Transactional);
			LinkEnd->Modify();
		}
		if (const UEdGraphSchema* Schema = Graph->GetSchema())
		{
			Schema->BreakSinglePinLink(Pin, TargetPin);
		}
		else
		{
			Pin->BreakLinkTo(TargetPin);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("disconnected"), true);
		Data->SetNumberField(TEXT("broken_connections"), 1);
		Data->SetStringField(TEXT("source"), FString::Printf(TEXT("%s.%s"), *NodeId, *PinName));
		Data->SetStringField(TEXT("target"), FString::Printf(TEXT("%s.%s"), *TargetNodeId, *TargetPinName));

		UE_LOG(LogCortexGraph, Log, TEXT("Disconnected link %s.%s -> %s.%s"),
			*NodeId, *PinName, *TargetNodeId, *TargetPinName);
		return FCortexCommandRouter::Success(Data);
	}

	// Get connection count BEFORE breaking (for reporting)
	int32 BrokenCount = Pin->LinkedTo.Num();

	// Create transaction only when we're actually modifying
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex:Disconnect Pin")));
	Graph->Modify();

	Pin->BreakAllPinLinks();

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("disconnected"), true);
	Data->SetNumberField(TEXT("broken_connections"), BrokenCount);

	UE_LOG(LogCortexGraph, Log, TEXT("Disconnected pin %s.%s (%d connections broken)"),
		*NodeId, *PinName, BrokenCount);

	return FCortexCommandRouter::Success(Data);
}
