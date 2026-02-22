#include "Operations/CortexGraphNodeOps.h"
#include "CortexGraphModule.h"
#include "CortexGraphLayoutOps.h"
#include "CortexBatchScope.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
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
	else if (NodeClassName == TEXT("UK2Node_VariableSet"))
	{
		NodeClass = UK2Node_VariableSet::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_VariableGet"))
	{
		NodeClass = UK2Node_VariableGet::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_Event"))
	{
		NodeClass = UK2Node_Event::StaticClass();
	}
	else if (NodeClassName == TEXT("UK2Node_ExecutionSequence"))
	{
		NodeClass = UK2Node_ExecutionSequence::StaticClass();
	}
	else
	{
		// Try finding other classes dynamically (e.g., UK2Node_Event, UK2Node_MacroInstance)
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
		FString::Printf(TEXT("Cortex: Add node %s"), *NodeClassName)
	));

	Graph->Modify();

	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
	NewNode->CreateNewGuid();
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	Graph->AddNode(NewNode, true, false);

	// Handle type-specific setup
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

		UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(NewNode);
		if (VarNode)
		{
			FString VariableName;
			if ((*NodeParams)->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				FString VariableClass;
				if ((*NodeParams)->TryGetStringField(TEXT("variable_class"), VariableClass))
				{
					// External class property (e.g., PlayerController.bShowMouseCursor)
					UClass* VarClass = FindFirstObject<UClass>(*VariableClass);
					if (VarClass == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Variable owner class not found: %s"), *VariableClass)
						);
					}

					FProperty* Prop = VarClass->FindPropertyByName(FName(*VariableName));
					if (Prop == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Property not found: %s on class %s"), *VariableName, *VariableClass)
						);
					}

					VarNode->SetFromProperty(Prop, false, VarClass);
				}
				else
				{
					// Self-context property (on the Blueprint's own class)
					UClass* SelfClass = Blueprint->SkeletonGeneratedClass
						? Blueprint->SkeletonGeneratedClass
						: Blueprint->GeneratedClass;
					if (SelfClass)
					{
						FProperty* SelfProp = SelfClass->FindPropertyByName(FName(*VariableName));
						if (SelfProp == nullptr)
						{
							Graph->RemoveNode(NewNode);
							return FCortexCommandRouter::Error(
								CortexErrorCodes::InvalidField,
								FString::Printf(TEXT("Self property not found: %s"), *VariableName)
							);
						}
					}
					VarNode->VariableReference.SetSelfMember(FName(*VariableName));
				}
			}
		}

		UK2Node_Event* EventNode = Cast<UK2Node_Event>(NewNode);
		if (EventNode)
		{
			FString FunctionName;
			if ((*NodeParams)->TryGetStringField(TEXT("function_name"), FunctionName))
			{
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
							FString::Printf(TEXT("Event owner class not found: %s"), *ClassName)
						);
					}

					UFunction* Func = FuncClass->FindFunctionByName(FName(*FuncName));
					if (Func == nullptr)
					{
						Graph->RemoveNode(NewNode);
						return FCortexCommandRouter::Error(
							CortexErrorCodes::InvalidField,
							FString::Printf(TEXT("Event function not found: %s on class %s"), *FuncName, *ClassName)
						);
					}

					EventNode->EventReference.SetExternalMember(FName(*FuncName), FuncClass);
					EventNode->bOverrideFunction = true;
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
	// For class/object pins, try to resolve and set DefaultObject
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
		Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		// Try to load the class from the value string
		UClass* ClassObject = LoadClass<UObject>(nullptr, *Value);
		if (ClassObject)
		{
			Pin->DefaultObject = ClassObject;
			Pin->DefaultValue = TEXT("");  // Clear string value when using object reference
		}
		else
		{
			// Fallback to string value if class can't be loaded
			Pin->DefaultValue = Value;
		}
	}
	else
	{
		Pin->DefaultValue = Value;
	}

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

FCortexCommandResult FCortexGraphNodeOps::AutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UBlueprint* Blueprint = LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint) return LoadError;

	FString ModeStr;
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	ECortexLayoutMode Mode = (ModeStr == TEXT("incremental"))
		? ECortexLayoutMode::Incremental : ECortexLayoutMode::Full;

	FString GraphFilter;
	Params->TryGetStringField(TEXT("graph_name"), GraphFilter);

	FCortexLayoutConfig Config;
	Config.Direction = ECortexLayoutDirection::LeftToRight;
	Config.Mode = Mode;

	double HSpacingVal = 0, VSpacingVal = 0;
	if (Params->TryGetNumberField(TEXT("horizontal_spacing"), HSpacingVal) && HSpacingVal > 0)
	{
		Config.HorizontalSpacing = static_cast<int32>(HSpacingVal);
	}
	if (Params->TryGetNumberField(TEXT("vertical_spacing"), VSpacingVal) && VSpacingVal > 0)
	{
		Config.VerticalSpacing = static_cast<int32>(VSpacingVal);
	}

	// Collect graphs to process
	TArray<UEdGraph*> Graphs;
	if (!GraphFilter.IsEmpty())
	{
		UEdGraph* Graph = FindGraph(Blueprint, GraphFilter, LoadError);
		if (!Graph) return LoadError;
		Graphs.Add(Graph);
	}
	else
	{
		for (UEdGraph* G : Blueprint->UbergraphPages) { if (G) Graphs.Add(G); }
		for (UEdGraph* G : Blueprint->FunctionGraphs) { if (G) Graphs.Add(G); }
		for (UEdGraph* G : Blueprint->MacroGraphs) { if (G) Graphs.Add(G); }
	}

	int32 TotalNodesProcessed = 0;

	TUniquePtr<FScopedTransaction> Transaction;
	if (!FCortexCommandRouter::IsInBatch())
	{
		Transaction = MakeUnique<FScopedTransaction>(
			FText::FromString(TEXT("Cortex: Auto-Layout Blueprint Graphs")));
	}

	for (UEdGraph* Graph : Graphs)
	{
		TArray<FCortexLayoutNode> LayoutNodes;
		TMap<FString, UEdGraphNode*> IdToNode;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			// Skip comment nodes (class name contains "Comment")
			if (Node->GetClass()->GetName() == TEXT("EdGraphNode_Comment"))
			{
				continue;
			}

			FCortexLayoutNode LN;
			LN.Id = Node->GetName();
			IdToNode.Add(LN.Id, Node);

			bool bHasExecInput = false;
			bool bHasExecOutput = false;
			int32 InputPinCount = 0;
			int32 OutputPinCount = 0;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				if (Pin->Direction == EGPD_Input)
				{
					InputPinCount++;
					if (bIsExec) bHasExecInput = true;
				}
				else
				{
					OutputPinCount++;
					if (bIsExec) bHasExecOutput = true;
				}
			}

			LN.bIsEntryPoint = (!bHasExecInput && bHasExecOutput);
			LN.bIsExecNode = (bHasExecInput || bHasExecOutput);
			int32 PinRows = FMath::Max(InputPinCount, OutputPinCount);
			LN.Width = 200;
			LN.Height = FMath::Max(100, PinRows * 28 + 40);

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				bool bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
					FString TargetId = LinkedPin->GetOwningNode()->GetName();
					if (bIsExec)
					{
						LN.ExecOutputs.AddUnique(TargetId);
					}
					else
					{
						LN.DataOutputs.AddUnique(TargetId);
					}
				}
			}
			LayoutNodes.Add(LN);
		}

		// Collect existing positions for incremental mode
		TMap<FString, FIntPoint> ExistingPositions;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !Node->GetClass()->GetName().Contains(TEXT("Comment")))
			{
				ExistingPositions.Add(Node->GetName(), FIntPoint(Node->NodePosX, Node->NodePosY));
			}
		}

		FCortexLayoutResult LayoutResult = FCortexGraphLayoutOps::CalculateLayout(
			LayoutNodes, Config, ExistingPositions);

		for (const auto& Pair : LayoutResult.Positions)
		{
			UEdGraphNode** NodePtr = IdToNode.Find(Pair.Key);
			if (NodePtr && *NodePtr)
			{
				(*NodePtr)->Modify();
				(*NodePtr)->NodePosX = Pair.Value.X;
				(*NodePtr)->NodePosY = Pair.Value.Y;
			}
		}

		if (FCortexCommandRouter::IsInBatch())
		{
			FString GraphKey = FString::Printf(TEXT("graph.notify.%s"), *Graph->GetPathName());
			FCortexBatchScope::AddCleanupAction(GraphKey,
				[WeakGraph = TWeakObjectPtr<UEdGraph>(Graph)]()
				{
					if (UEdGraph* G = WeakGraph.Get()) G->NotifyGraphChanged();
				});
		}
		else
		{
			Graph->NotifyGraphChanged();
		}

		TotalNodesProcessed += LayoutResult.Positions.Num();
	}

	if (FCortexCommandRouter::IsInBatch())
	{
		FString BPKey = FString::Printf(TEXT("bp.modified.%s"), *Blueprint->GetPathName());
		FCortexBatchScope::AddCleanupAction(BPKey,
			[WeakBP = TWeakObjectPtr<UBlueprint>(Blueprint)]()
			{
				if (UBlueprint* BP = WeakBP.Get())
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
				}
			});
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	Blueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), TotalNodesProcessed);
	Data->SetNumberField(TEXT("graphs_processed"), Graphs.Num());

	UE_LOG(LogCortexGraph, Log, TEXT("Auto-layout completed: %d nodes across %d graphs in %s"),
		TotalNodesProcessed, Graphs.Num(), *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
