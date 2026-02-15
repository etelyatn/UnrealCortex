#include "Operations/CortexGraphNodeOps.h"
#include "CortexGraphModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"

UBlueprint* FCortexGraphNodeOps::LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError)
{
	// Check if package exists before LoadObject to avoid SkipPackage warnings
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (Blueprint == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)
		);
	}
	return Blueprint;
}

UEdGraph* FCortexGraphNodeOps::FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError)
{
	FString TargetName = GraphName.IsEmpty() ? TEXT("EventGraph") : GraphName;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == TargetName)
		{
			return Graph;
		}
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TargetName)
		{
			return Graph;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::GraphNotFound,
		FString::Printf(TEXT("Graph not found: %s"), *TargetName)
	);
	return nullptr;
}

UEdGraphNode* FCortexGraphNodeOps::FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->GetName() == NodeId)
		{
			return Node;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::NodeNotFound,
		FString::Printf(TEXT("Node not found: %s"), *NodeId)
	);
	return nullptr;
}

UEdGraphPin* FCortexGraphNodeOps::FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			return Pin;
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::PinNotFound,
		FString::Printf(TEXT("Pin not found: %s on node %s"), *PinName, *Node->GetName())
	);
	return nullptr;
}

FCortexCommandResult FCortexGraphNodeOps::ListGraphs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArray;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Graph->GetName());
		Entry->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Graph->GetName());
		Entry->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
		Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), Node->GetName());
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Entry->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
		Pos->SetNumberField(TEXT("x"), Node->NodePosX);
		Pos->SetNumberField(TEXT("y"), Node->NodePosY);
		Entry->SetObjectField(TEXT("position"), Pos);
		Entry->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
		NodesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::GetNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: node_id")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	UEdGraphNode* Node = FindNode(Graph, NodeId, LoadError);
	if (Node == nullptr)
	{
		return LoadError;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), Node->GetName());
	Data->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Data->SetStringField(TEXT("display_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

	TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
	Pos->SetNumberField(TEXT("x"), Node->NodePosX);
	Pos->SetNumberField(TEXT("y"), Node->NodePosY);
	Data->SetObjectField(TEXT("position"), Pos);

	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, true)));
	}
	Data->SetArrayField(TEXT("pins"), PinsArray);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	FString NodeClassName;
	if (!Params->TryGetStringField(TEXT("node_class"), NodeClassName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: node_class")
		);
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0;
	int32 PosY = 0;
	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PosObj) && PosObj)
	{
		(*PosObj)->TryGetNumberField(TEXT("x"), PosX);
		(*PosObj)->TryGetNumberField(TEXT("y"), PosY);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Resolve node class
	// For well-known classes, use StaticClass (faster, no dynamic loading)
	// Other classes use dynamic loading from /Script/BlueprintGraph or /Script/Engine
	UClass* NodeClass = nullptr;
	if (NodeClassName == TEXT("UK2Node_CallFunction"))
	{
		NodeClass = UK2Node_CallFunction::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_IfThenElse"))
	{
		NodeClass = UK2Node_IfThenElse::StaticClass();
	}
	else
	{
		// Try finding other classes dynamically (e.g., UK2Node_VariableGet, UK2Node_Event)
		NodeClass = StaticLoadClass(UEdGraphNode::StaticClass(), nullptr,
			*FString::Printf(TEXT("/Script/BlueprintGraph.%s"), *NodeClassName));
		if (NodeClass == nullptr)
		{
			NodeClass = StaticLoadClass(UEdGraphNode::StaticClass(), nullptr,
				*FString::Printf(TEXT("/Script/Engine.%s"), *NodeClassName));
		}
	}

	if (NodeClass == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node class not found: %s"), *NodeClassName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Node '%s'"), *NodeClassName)
	));

	Graph->Modify();

	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
	NewNode->CreateNewGuid();
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	Graph->AddNode(NewNode, true, false);

	// Handle CallFunction-specific setup
	// NodeParams = node-specific parameters (nested object), distinct from outer Params
	const TSharedPtr<FJsonObject>* NodeParams = nullptr;
	if (Params->TryGetObjectField(TEXT("params"), NodeParams) && NodeParams)
	{
		UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode);
		if (CallNode)
		{
			FString FunctionName;
			if ((*NodeParams)->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				// Parse "ClassName.FunctionName" format
				FString ClassName;
				FString FuncName;
				if (FunctionName.Split(TEXT("."), &ClassName, &FuncName))
				{
					UClass* FuncClass = FindFirstObject<UClass>(*ClassName);
					if (FuncClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Function owner class not found: %s"), *ClassName)
						);
					}

					UFunction* Func = FuncClass->FindFunctionByName(FName(*FuncName));
					if (Func == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Function not found: %s on class %s"), *FuncName, *ClassName)
						);
					}

					CallNode->SetFromFunction(Func);
				}
			}
		}
	}

	NewNode->AllocateDefaultPins();
	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NewNode->GetName());
	Data->SetStringField(TEXT("class"), NewNode->GetClass()->GetName());
	Data->SetStringField(TEXT("display_name"), NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		PinsArray.Add(MakeShared<FJsonValueObject>(SerializePin(Pin, false)));
	}
	Data->SetArrayField(TEXT("pins"), PinsArray);

	return FCortexCommandRouter::Success(Data);
}

TSharedRef<FJsonObject> FCortexGraphNodeOps::SerializePin(const UEdGraphPin* Pin, bool bDetailed)
{
	TSharedRef<FJsonObject> PinEntry = MakeShared<FJsonObject>();
	PinEntry->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinEntry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	PinEntry->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	if (bDetailed)
	{
		PinEntry->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		PinEntry->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
	}
	return PinEntry;
}

FCortexCommandResult FCortexGraphNodeOps::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	// Find the node
	UEdGraphNode* FoundNode = FindNode(Graph, NodeId, LoadError);
	if (FoundNode == nullptr)
	{
		return LoadError;
	}

	// Count connected pins before removal
	int32 DisconnectedPins = 0;
	for (UEdGraphPin* Pin : FoundNode->Pins)
	{
		if (Pin != nullptr && Pin->LinkedTo.Num() > 0)
		{
			++DisconnectedPins;
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Remove node %s"), *NodeId)
	));
	Graph->Modify();

	// Break all connections then remove the node
	FoundNode->BreakAllNodeLinks();
	Graph->RemoveNode(FoundNode);

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("removed_node_id"), NodeId);
	Data->SetNumberField(TEXT("disconnected_pins"), DisconnectedPins);

	UE_LOG(LogCortexGraph, Log, TEXT("Removed node %s from graph %s (%d pins disconnected)"),
		*NodeId, *Graph->GetName(), DisconnectedPins);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeOps::SetPinValue(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;
	FString PinName;
	FString Value;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("node_id"), NodeId)
		&& Params->TryGetStringField(TEXT("pin_name"), PinName)
		&& Params->TryGetStringField(TEXT("value"), Value);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, node_id, pin_name, and value")
		);
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return LoadError;
	}

	FString GraphName;
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* Graph = FindGraph(Blueprint, GraphName, LoadError);
	if (Graph == nullptr)
	{
		return LoadError;
	}

	UEdGraphNode* Node = FindNode(Graph, NodeId, LoadError);
	if (Node == nullptr)
	{
		return LoadError;
	}

	UEdGraphPin* Pin = FindPin(Node, PinName, LoadError);
	if (Pin == nullptr)
	{
		return LoadError;
	}

	// Verify this is an input pin (output pins don't have default values)
	if (Pin->Direction != EGPD_Input)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set value on output pin: %s"), *PinName)
		);
	}

	// Verify pin is not connected (connected pins ignore default values)
	if (Pin->LinkedTo.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Cannot set value on connected pin: %s"), *PinName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set pin value %s.%s"), *NodeId, *PinName)
	));

	Graph->Modify();
	Node->Modify();

	// Set the default value
	Pin->DefaultValue = Value;

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("pin_name"), PinName);
	Data->SetStringField(TEXT("value"), Value);
	Data->SetBoolField(TEXT("success"), true);

	UE_LOG(LogCortexGraph, Log, TEXT("Set pin value %s.%s = %s"), *NodeId, *PinName, *Value);

	return FCortexCommandRouter::Success(Data);
}
