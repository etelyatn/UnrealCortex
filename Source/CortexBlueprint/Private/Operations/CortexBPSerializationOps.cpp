#include "Operations/CortexBPSerializationOps.h"

#include "CortexBlueprintModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UBlueprint* FCortexBPSerializationOps::LoadBlueprintSafe(const FString& AssetPath, FString& OutError)
{
	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath);
		return nullptr;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath);
		return nullptr;
	}
	return Blueprint;
}

void FCortexBPSerializationOps::Serialize(const FCortexSerializationRequest& Request, FOnSerializationComplete Callback)
{
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintSafe(Request.BlueprintPath, LoadError);
	if (!Blueprint)
	{
		Callback.Execute(false, FString::Printf(TEXT("{\"error\":\"%s\"}"), *LoadError));
		return;
	}

	FString Json;
	if (Request.bConversionMode)
	{
		switch (Request.Scope)
		{
		case ECortexConversionScope::EntireBlueprint:
			Json = SerializeEntireBlueprintCompact(Blueprint);
			break;
		case ECortexConversionScope::SelectedNodes:
			Json = SerializeSelectedNodesCompact(Blueprint, Request.SelectedNodeIds);
			break;
		case ECortexConversionScope::CurrentGraph:
			Json = Request.TargetGraphNames.Num() > 0
				? SerializeGraphCompact(Blueprint, Request.TargetGraphNames[0])
				: FString();
			break;
		case ECortexConversionScope::EventOrFunction:
			if (Request.TargetGraphNames.Num() == 0)
			{
				Json = TEXT("{\"error\":\"No target events or functions specified\"}");
			}
			else if (Request.TargetGraphNames.Num() == 1)
			{
				Json = SerializeEventOrFunctionCompact(Blueprint, Request.TargetGraphNames[0]);
			}
			else
			{
				Json = SerializeMultipleEventOrFunctionCompact(Blueprint, Request.TargetGraphNames);
			}
			break;
		}
	}
	else
	{
		switch (Request.Scope)
		{
		case ECortexConversionScope::EntireBlueprint:
			Json = SerializeEntireBlueprint(Blueprint);
			break;
		case ECortexConversionScope::SelectedNodes:
			Json = SerializeSelectedNodes(Blueprint, Request.SelectedNodeIds);
			break;
		case ECortexConversionScope::CurrentGraph:
			Json = Request.TargetGraphNames.Num() > 0
				? SerializeGraph(Blueprint, Request.TargetGraphNames[0])
				: FString();
			break;
		case ECortexConversionScope::EventOrFunction:
			if (Request.TargetGraphNames.Num() == 0)
			{
				Json = TEXT("{\"error\":\"No target events or functions specified\"}");
			}
			else if (Request.TargetGraphNames.Num() == 1)
			{
				Json = SerializeEventOrFunction(Blueprint, Request.TargetGraphNames[0]);
			}
			else
			{
				Json = SerializeMultipleEventOrFunction(Blueprint, Request.TargetGraphNames);
			}
			break;
		}
	}

	Callback.Execute(true, Json);
}

TSharedRef<FJsonObject> FCortexBPSerializationOps::NodeToJson(UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Obj->SetNumberField(TEXT("x"), Node->NodePosX);
	Obj->SetNumberField(TEXT("y"), Node->NodePosY);

	// Comment
	if (!Node->NodeComment.IsEmpty())
	{
		Obj->SetStringField(TEXT("comment"), Node->NodeComment);
	}

	// Pins
	TArray<TSharedPtr<FJsonValue>> PinsJson;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->bHidden) continue;

		TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

		// Sub-category and type object for accurate C++ type resolution
		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			PinObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategory.ToString());
		}
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObj->SetStringField(TEXT("type_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
			PinObj->SetStringField(TEXT("type_object_name"), Pin->PinType.PinSubCategoryObject->GetName());
		}
		if (Pin->PinType.ContainerType != EPinContainerType::None)
		{
			PinObj->SetStringField(TEXT("container"),
				Pin->PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
				Pin->PinType.ContainerType == EPinContainerType::Set ? TEXT("set") : TEXT("map"));
		}
		if (Pin->PinType.bIsReference)
		{
			PinObj->SetBoolField(TEXT("is_reference"), true);
		}
		if (Pin->PinType.bIsConst)
		{
			PinObj->SetBoolField(TEXT("is_const"), true);
		}

		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (!Pin->DefaultTextValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_text_value"), Pin->DefaultTextValue.ToString());
		}

		// Connected pins
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LinksJson;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				TSharedRef<FJsonObject> LinkObj = MakeShared<FJsonObject>();
				LinkObj->SetStringField(TEXT("node_id"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				LinkObj->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
				LinksJson.Add(MakeShared<FJsonValueObject>(LinkObj));
			}
			PinObj->SetField(TEXT("connected_to"), MakeShared<FJsonValueArray>(LinksJson));
		}

		PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Obj->SetField(TEXT("pins"), MakeShared<FJsonValueArray>(PinsJson));

	return Obj;
}

TSharedRef<FJsonObject> FCortexBPSerializationOps::GraphToJson(UEdGraph* Graph)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Graph->GetFName().ToString());
	Obj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
	}
	Obj->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));

	return Obj;
}

TArray<TSharedPtr<FJsonValue>> FCortexBPSerializationOps::VariablesToJson(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> VarsJson;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedRef<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());

		if (Var.VarType.PinSubCategoryObject.IsValid())
		{
			VarObj->SetStringField(TEXT("sub_type"), Var.VarType.PinSubCategoryObject->GetName());
		}
		if (Var.VarType.ContainerType == EPinContainerType::Array)
		{
			VarObj->SetBoolField(TEXT("is_array"), true);
		}

		VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarObj->SetBoolField(TEXT("is_exposed"),
			(Var.PropertyFlags & CPF_BlueprintVisible) ? true : false);

		if (!Var.Category.IsEmpty())
		{
			VarObj->SetStringField(TEXT("category"), Var.Category.ToString());
		}

		// UPROPERTY specifiers
		TArray<TSharedPtr<FJsonValue>> Specifiers;
		if (Var.PropertyFlags & CPF_Edit) Specifiers.Add(MakeShared<FJsonValueString>(TEXT("EditAnywhere")));
		if (Var.PropertyFlags & CPF_BlueprintVisible) Specifiers.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadWrite")));
		if (Var.PropertyFlags & CPF_BlueprintReadOnly) Specifiers.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadOnly")));
		if (Var.PropertyFlags & CPF_Net) Specifiers.Add(MakeShared<FJsonValueString>(TEXT("Replicated")));
		if (Var.PropertyFlags & CPF_SaveGame) Specifiers.Add(MakeShared<FJsonValueString>(TEXT("SaveGame")));
		if (!Specifiers.IsEmpty())
		{
			VarObj->SetField(TEXT("specifiers"), MakeShared<FJsonValueArray>(Specifiers));
		}

		VarsJson.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	return VarsJson;
}

TArray<TSharedPtr<FJsonValue>> FCortexBPSerializationOps::ComponentsToJson(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> CompsJson;
	if (!Blueprint->SimpleConstructionScript) return CompsJson;

	TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
	for (USCS_Node* ScsNode : AllNodes)
	{
		TSharedRef<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), ScsNode->GetVariableName().ToString());
		CompObj->SetStringField(TEXT("class"),
			ScsNode->ComponentClass ? ScsNode->ComponentClass->GetName() : TEXT("Unknown"));

		// Parent component
		if (USCS_Node* Parent = Blueprint->SimpleConstructionScript->FindParentNode(ScsNode))
		{
			CompObj->SetStringField(TEXT("parent"), Parent->GetVariableName().ToString());
		}

		CompsJson.Add(MakeShared<FJsonValueObject>(CompObj));
	}
	return CompsJson;
}

FString FCortexBPSerializationOps::SerializeEntireBlueprint(UBlueprint* Blueprint)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
	Root->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());

	// Variables
	Root->SetField(TEXT("variables"), MakeShared<FJsonValueArray>(VariablesToJson(Blueprint)));

	// Components
	Root->SetField(TEXT("components"), MakeShared<FJsonValueArray>(ComponentsToJson(Blueprint)));

	// Graphs (ubergraph pages + function graphs)
	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJson(Graph)));
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJson(Graph)));
	}
	Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

	// Serialize to string
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FCortexBPSerializationOps::SerializeSelectedNodes(UBlueprint* Blueprint, const TArray<FString>& NodeIds)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("scope"), TEXT("selected_nodes"));

	// Find matching nodes across all graphs
	TSet<FString> NodeIdSet(NodeIds);
	TArray<TSharedPtr<FJsonValue>> NodesJson;

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (NodeIdSet.Contains(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)))
			{
				NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
			}
		}
	}

	Root->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));
	Root->SetNumberField(TEXT("node_count"), NodesJson.Num());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FCortexBPSerializationOps::SerializeGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph->GetFName().ToString() == GraphName)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("parent_class"),
				Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
			Root->SetStringField(TEXT("scope"), TEXT("graph"));

			// Include variables for context
			Root->SetField(TEXT("variables"), MakeShared<FJsonValueArray>(VariablesToJson(Blueprint)));
			Root->SetField(TEXT("components"), MakeShared<FJsonValueArray>(ComponentsToJson(Blueprint)));

			// The target graph
			TArray<TSharedPtr<FJsonValue>> GraphsJson;
			GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJson(Graph)));
			Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	return TEXT("{\"error\":\"Graph not found\"}");
}

FString FCortexBPSerializationOps::SerializeEventOrFunction(UBlueprint* Blueprint, const FString& TargetName)
{
	// Check function graphs first (each function is its own graph)
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetFName().ToString() == TargetName)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("scope"), TEXT("function"));
			Root->SetStringField(TEXT("target"), TargetName);

			TArray<TSharedPtr<FJsonValue>> GraphsJson;
			GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJson(Graph)));
			Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	// Event traversal — find matching event node in ubergraph and follow exec pins
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (!EventNode) continue;

			FString EventTitle = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (EventTitle != TargetName) continue;

			// Two-pass traversal:
			// Pass 1: Follow exec pins forward to find exec-reachable nodes
			// Pass 2: Follow data input pins backward to collect pure/helper nodes
			TSet<UEdGraphNode*> ReachableNodes;
			TArray<UEdGraphNode*> WorkQueue;
			WorkQueue.Add(EventNode);
			ReachableNodes.Add(EventNode);

			// Pass 1: exec-flow traversal
			while (WorkQueue.Num() > 0)
			{
				UEdGraphNode* Current = WorkQueue.Pop();
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin->Direction != EGPD_Output) continue;
					// Only follow exec pins in pass 1
					if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						if (!ReachableNodes.Contains(LinkedNode))
						{
							ReachableNodes.Add(LinkedNode);
							WorkQueue.Add(LinkedNode);
						}
					}
				}
			}

			// Pass 2: follow data input pins backward from exec-reachable nodes
			// to collect pure/helper nodes (e.g., GetActorLocation feeding into SetActorLocation)
			TArray<UEdGraphNode*> DataQueue;
			for (UEdGraphNode* ReachableNode : ReachableNodes)
			{
				DataQueue.Add(ReachableNode);
			}
			while (DataQueue.Num() > 0)
			{
				UEdGraphNode* Current = DataQueue.Pop();
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						if (!ReachableNodes.Contains(LinkedNode))
						{
							ReachableNodes.Add(LinkedNode);
							DataQueue.Add(LinkedNode);
						}
					}
				}
			}

			// Serialize reachable nodes
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("scope"), TEXT("event"));
			Root->SetStringField(TEXT("target"), TargetName);

			TArray<TSharedPtr<FJsonValue>> NodesJson;
			for (UEdGraphNode* ReachableNode : ReachableNodes)
			{
				NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJson(ReachableNode)));
			}
			Root->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));
			Root->SetNumberField(TEXT("node_count"), NodesJson.Num());

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	return TEXT("{\"error\":\"Event or function not found\"}");
}

FString FCortexBPSerializationOps::SerializeMultipleEventOrFunction(UBlueprint* Blueprint, const TArray<FString>& TargetNames)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("scope"), TEXT("multiple_functions"));

	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (const FString& Name : TargetNames)
	{
		FString SingleJson = SerializeEventOrFunction(Blueprint, Name);

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SingleJson);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			FunctionsArray.Add(MakeShared<FJsonValueObject>(Parsed.ToSharedRef()));
		}
	}
	Root->SetField(TEXT("functions"), MakeShared<FJsonValueArray>(FunctionsArray));
	Root->SetNumberField(TEXT("function_count"), FunctionsArray.Num());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

// ── Compact serialization (bConversionMode=true) ─────────────────────────────
// Strips x/y positions, comments, full type paths; uses sequential int node IDs.

TSharedRef<FJsonObject> FCortexBPSerializationOps::NodeToJsonCompact(
	UEdGraphNode* Node,
	const TMap<UEdGraphNode*, int32>& IndexMap)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();

	// Sequential integer ID instead of 36-char UUID
	if (const int32* Idx = IndexMap.Find(Node))
	{
		Obj->SetNumberField(TEXT("id"), *Idx);
	}

	Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	// x, y, comment intentionally omitted

	TArray<TSharedPtr<FJsonValue>> PinsJson;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->bHidden) continue;

		TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			PinObj->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategory.ToString());
		}

		// Short class name only — full path (/Script/Engine.Actor) not needed for C++ translation
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObj->SetStringField(TEXT("type_object"), Pin->PinType.PinSubCategoryObject->GetName());
		}

		if (Pin->PinType.ContainerType != EPinContainerType::None)
		{
			PinObj->SetStringField(TEXT("container"),
				Pin->PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
				Pin->PinType.ContainerType == EPinContainerType::Set ? TEXT("set") : TEXT("map"));
		}
		if (Pin->PinType.bIsReference)
		{
			PinObj->SetBoolField(TEXT("is_reference"), true);
		}
		if (Pin->PinType.bIsConst)
		{
			PinObj->SetBoolField(TEXT("is_const"), true);
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}

		// Connections: use integer indices instead of UUIDs
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LinksJson;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (const int32* LinkedIdx = IndexMap.Find(LinkedNode))
				{
					TSharedRef<FJsonObject> LinkObj = MakeShared<FJsonObject>();
					LinkObj->SetNumberField(TEXT("node_id"), *LinkedIdx);
					LinkObj->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
					LinksJson.Add(MakeShared<FJsonValueObject>(LinkObj));
				}
			}
			if (LinksJson.Num() > 0)
			{
				PinObj->SetField(TEXT("connected_to"), MakeShared<FJsonValueArray>(LinksJson));
			}
		}

		PinsJson.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	Obj->SetField(TEXT("pins"), MakeShared<FJsonValueArray>(PinsJson));

	return Obj;
}

TSharedRef<FJsonObject> FCortexBPSerializationOps::GraphToJsonCompact(UEdGraph* Graph)
{
	// Build index map: stable ordering for reproducible integer IDs
	TMap<UEdGraphNode*, int32> IndexMap;
	IndexMap.Reserve(Graph->Nodes.Num());
	for (int32 i = 0; i < Graph->Nodes.Num(); ++i)
	{
		IndexMap.Add(Graph->Nodes[i], i);
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Graph->GetFName().ToString());

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJsonCompact(Node, IndexMap)));
	}
	Obj->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));

	return Obj;
}

FString FCortexBPSerializationOps::SerializeEntireBlueprintCompact(UBlueprint* Blueprint)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));

	Root->SetField(TEXT("variables"), MakeShared<FJsonValueArray>(VariablesToJson(Blueprint)));
	Root->SetField(TEXT("components"), MakeShared<FJsonValueArray>(ComponentsToJson(Blueprint)));

	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJsonCompact(Graph)));
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJsonCompact(Graph)));
	}
	Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FCortexBPSerializationOps::SerializeSelectedNodesCompact(UBlueprint* Blueprint, const TArray<FString>& NodeIds)
{
	TSet<FString> NodeIdSet(NodeIds);

	TArray<UEdGraphNode*> MatchedNodes;
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (NodeIdSet.Contains(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)))
			{
				MatchedNodes.Add(Node);
			}
		}
	}

	TMap<UEdGraphNode*, int32> IndexMap;
	IndexMap.Reserve(MatchedNodes.Num());
	for (int32 i = 0; i < MatchedNodes.Num(); ++i)
	{
		IndexMap.Add(MatchedNodes[i], i);
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("scope"), TEXT("selected_nodes"));

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(MatchedNodes.Num());
	for (UEdGraphNode* Node : MatchedNodes)
	{
		NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJsonCompact(Node, IndexMap)));
	}
	Root->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}

FString FCortexBPSerializationOps::SerializeGraphCompact(UBlueprint* Blueprint, const FString& GraphName)
{
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph->GetFName().ToString() == GraphName)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("parent_class"),
				Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
			Root->SetStringField(TEXT("scope"), TEXT("graph"));

			Root->SetField(TEXT("variables"), MakeShared<FJsonValueArray>(VariablesToJson(Blueprint)));
			Root->SetField(TEXT("components"), MakeShared<FJsonValueArray>(ComponentsToJson(Blueprint)));

			TArray<TSharedPtr<FJsonValue>> GraphsJson;
			GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJsonCompact(Graph)));
			Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	return TEXT("{\"error\":\"Graph not found\"}");
}

FString FCortexBPSerializationOps::SerializeEventOrFunctionCompact(UBlueprint* Blueprint, const FString& TargetName)
{
	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetFName().ToString() == TargetName)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("scope"), TEXT("function"));
			Root->SetStringField(TEXT("target"), TargetName);

			TArray<TSharedPtr<FJsonValue>> GraphsJson;
			GraphsJson.Add(MakeShared<FJsonValueObject>(GraphToJsonCompact(Graph)));
			Root->SetField(TEXT("graphs"), MakeShared<FJsonValueArray>(GraphsJson));

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	// Event traversal — same two-pass algorithm as verbose variant
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
			if (!EventNode) continue;

			FString EventTitle = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (EventTitle != TargetName) continue;

			TSet<UEdGraphNode*> ReachableSet;
			TArray<UEdGraphNode*> WorkQueue;
			WorkQueue.Add(EventNode);
			ReachableSet.Add(EventNode);

			// Pass 1: exec-flow traversal
			while (WorkQueue.Num() > 0)
			{
				UEdGraphNode* Current = WorkQueue.Pop();
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin->Direction != EGPD_Output) continue;
					if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						if (!ReachableSet.Contains(LinkedNode))
						{
							ReachableSet.Add(LinkedNode);
							WorkQueue.Add(LinkedNode);
						}
					}
				}
			}

			// Pass 2: follow data inputs backward for pure/helper nodes
			TArray<UEdGraphNode*> DataQueue(ReachableSet.Array());
			while (DataQueue.Num() > 0)
			{
				UEdGraphNode* Current = DataQueue.Pop();
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						if (!ReachableSet.Contains(LinkedNode))
						{
							ReachableSet.Add(LinkedNode);
							DataQueue.Add(LinkedNode);
						}
					}
				}
			}

			// Build stable-ordered array and index map
			TArray<UEdGraphNode*> ReachableNodes = ReachableSet.Array();
			TMap<UEdGraphNode*, int32> IndexMap;
			IndexMap.Reserve(ReachableNodes.Num());
			for (int32 i = 0; i < ReachableNodes.Num(); ++i)
			{
				IndexMap.Add(ReachableNodes[i], i);
			}

			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
			Root->SetStringField(TEXT("scope"), TEXT("event"));
			Root->SetStringField(TEXT("target"), TargetName);

			TArray<TSharedPtr<FJsonValue>> NodesJson;
			NodesJson.Reserve(ReachableNodes.Num());
			for (UEdGraphNode* ReachableNode : ReachableNodes)
			{
				NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJsonCompact(ReachableNode, IndexMap)));
			}
			Root->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(NodesJson));
			Root->SetNumberField(TEXT("node_count"), NodesJson.Num());

			FString Output;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
			FJsonSerializer::Serialize(Root, Writer);
			return Output;
		}
	}

	return TEXT("{\"error\":\"Event or function not found\"}");
}

FString FCortexBPSerializationOps::SerializeMultipleEventOrFunctionCompact(UBlueprint* Blueprint, const TArray<FString>& TargetNames)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Root->SetStringField(TEXT("scope"), TEXT("multiple_functions"));

	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (const FString& Name : TargetNames)
	{
		FString SingleJson = SerializeEventOrFunctionCompact(Blueprint, Name);

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SingleJson);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			FunctionsArray.Add(MakeShared<FJsonValueObject>(Parsed.ToSharedRef()));
		}
	}
	Root->SetField(TEXT("functions"), MakeShared<FJsonValueArray>(FunctionsArray));
	Root->SetNumberField(TEXT("function_count"), FunctionsArray.Num());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
	FJsonSerializer::Serialize(Root, Writer);
	return Output;
}
