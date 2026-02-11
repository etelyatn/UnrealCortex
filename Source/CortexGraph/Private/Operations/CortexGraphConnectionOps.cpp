#include "Operations/CortexGraphConnectionOps.h"
#include "Operations/CortexGraphNodeOps.h"
#include "CortexGraphModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"

FCortexCommandResult FCortexGraphConnectionOps::Connect(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SourceNodeId;
	FString SourcePinName;
	FString TargetNodeId;
	FString TargetPinName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("source_node"), SourceNodeId)
		&& Params->TryGetStringField(TEXT("source_pin"), SourcePinName)
		&& Params->TryGetStringField(TEXT("target_node"), TargetNodeId)
		&& Params->TryGetStringField(TEXT("target_pin"), TargetPinName);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, source_node, source_pin, target_node, target_pin")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	// Load blueprint using helper
	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// Find graph using helper
	UEdGraph* Graph = FCortexGraphNodeOps::FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
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
	}

	// Create transaction only after validation passes
	FScopedTransaction Transaction(FText::FromString(TEXT("Cortex:Connect Pins")));
	Graph->Modify();

	// Make the connection (schema handles breaking existing connections if needed)
	if (Schema != nullptr)
	{
		Schema->TryCreateConnection(SourcePin, TargetPin);
	}
	else
	{
		SourcePin->MakeLinkTo(TargetPin);
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
	FString NodeId;
	FString PinName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId)
		&& Params->TryGetStringField(TEXT("pin_name"), PinName);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, pin_name")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	// Load blueprint using helper
	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	// Find graph using helper
	UEdGraph* Graph = FCortexGraphNodeOps::FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
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
