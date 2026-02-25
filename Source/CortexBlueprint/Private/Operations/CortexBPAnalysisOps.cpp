#include "Operations/CortexBPAnalysisOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Blueprint/BlueprintSupport.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayTagContainer.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Net/UnrealNetwork.h"

namespace
{
int32 CountVariableUsage(UBlueprint* BP, const FName& VarName)
{
	int32 Count = 0;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
			{
				if (Getter->GetVarName() == VarName)
				{
					++Count;
				}
			}
			else if (const UK2Node_VariableSet* Setter = Cast<UK2Node_VariableSet>(Node))
			{
				if (Setter->GetVarName() == VarName)
				{
					++Count;
				}
			}
		}
	}
	return Count;
}

bool HasLatentNodesInGraph(UEdGraph* Graph)
{
	if (!Graph)
	{
		return false;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (CallNode->IsLatentFunction())
			{
				return true;
			}
		}
	}

	return false;
}

bool IsFunctionPure(UEdGraph* Graph)
{
	if (!Graph)
	{
		return false;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			for (UEdGraphPin* Pin : Entry->Pins)
			{
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					return false;
				}
			}
			return true;
		}
	}

	return false;
}

/** Extract the default value of the "Duration" pin from a latent call node (Delay, RetriggerableDelay, etc.) */
FString ExtractDurationPinValue(const UK2Node_CallFunction* CallNode)
{
	if (!CallNode)
	{
		return FString();
	}

	// Delay / RetriggerableDelay use "Duration" pin
	const UEdGraphPin* DurationPin = CallNode->FindPin(TEXT("Duration"));
	if (DurationPin && !DurationPin->DefaultValue.IsEmpty())
	{
		return DurationPin->DefaultValue;
	}

	// MoveComponentTo uses "OverTime" pin
	const UEdGraphPin* OverTimePin = CallNode->FindPin(TEXT("OverTime"));
	if (OverTimePin && !OverTimePin->DefaultValue.IsEmpty())
	{
		return OverTimePin->DefaultValue;
	}

	return FString();
}

/** Check if a node is a latent call function node */
bool IsLatentCallNode(const UEdGraphNode* Node)
{
	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
	return CallNode && CallNode->IsLatentFunction();
}

/**
 * Walk output exec pins from a latent node to find subsequent latent nodes in the same execution path.
 * Returns the count of latent nodes in the chain (including the starting node).
 */
int32 CountDownstreamLatentNodes(UEdGraphNode* StartNode, TSet<UEdGraphNode*>& Visited)
{
	if (!StartNode || Visited.Contains(StartNode))
	{
		return 0;
	}

	Visited.Add(StartNode);
	int32 MaxDownstream = 0;

	for (UEdGraphPin* Pin : StartNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}

			UEdGraphNode* NextNode = LinkedPin->GetOwningNode();

			if (IsLatentCallNode(NextNode))
			{
				// Found another latent node — count it plus its downstream
				TSet<UEdGraphNode*> ChildVisited = Visited;
				int32 Downstream = 1 + CountDownstreamLatentNodes(NextNode, ChildVisited);
				MaxDownstream = FMath::Max(MaxDownstream, Downstream);
			}
			else
			{
				// Non-latent node — keep walking through it
				int32 Downstream = CountDownstreamLatentNodes(NextNode, Visited);
				MaxDownstream = FMath::Max(MaxDownstream, Downstream);
			}
		}
	}

	return MaxDownstream;
}

FString ResolveUPropertySpecifier(uint64 Flags)
{
	const bool bEdit = (Flags & CPF_Edit) != 0;
	const bool bNoInstance = (Flags & CPF_DisableEditOnInstance) != 0;
	const bool bNoTemplate = (Flags & CPF_DisableEditOnTemplate) != 0;
	const bool bBPVisible = (Flags & CPF_BlueprintVisible) != 0;

	if (bEdit)
	{
		if (bNoInstance) { return TEXT("EditDefaultsOnly"); }
		if (bNoTemplate) { return TEXT("EditInstanceOnly"); }
		return TEXT("EditAnywhere");
	}
	if (bBPVisible)
	{
		if (bNoInstance) { return TEXT("VisibleDefaultsOnly"); }
		if (bNoTemplate) { return TEXT("VisibleInstanceOnly"); }
		return TEXT("VisibleAnywhere");
	}
	return TEXT("None");
}

FString ResolveReferenceType(const FEdGraphPinType& PinType)
{
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
		PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
	{
		return TEXT("Soft");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
	{
		return TEXT("Interface");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object && PinType.bIsWeakPointer)
	{
		return TEXT("Weak");
	}
	return TEXT("Hard");
}

FString CalculateMigrationConfidence(int32 TotalNodes, int32 LatentCount)
{
	if (TotalNodes < 20 && LatentCount == 0)
	{
		return TEXT("high");
	}
	if (TotalNodes < 50 && LatentCount <= 2)
	{
		return TEXT("high");
	}
	if (TotalNodes < 100)
	{
		return TEXT("medium");
	}
	return TEXT("low");
}

int32 ComputeLongestExecDepthFromNode(
	UEdGraphNode* Node,
	TMap<UEdGraphNode*, int32>& Memo,
	TSet<UEdGraphNode*>& ActiveStack)
{
	if (!Node)
	{
		return 0;
	}

	if (const int32* Cached = Memo.Find(Node))
	{
		return *Cached;
	}

	// Break cycles in looping graphs.
	if (ActiveStack.Contains(Node))
	{
		return 0;
	}

	ActiveStack.Add(Node);

	int32 MaxChildDepth = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}
			MaxChildDepth = FMath::Max(
				MaxChildDepth,
				ComputeLongestExecDepthFromNode(LinkedPin->GetOwningNode(), Memo, ActiveStack));
		}
	}

	ActiveStack.Remove(Node);

	const int32 Depth = 1 + MaxChildDepth;
	Memo.Add(Node, Depth);
	return Depth;
}

int32 ComputeGraphExecDepth(UEdGraph* Graph)
{
	if (!Graph)
	{
		return 0;
	}

	int32 MaxDepth = 0;
	TMap<UEdGraphNode*, int32> Memo;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		bool bHasOutgoingExec = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				bHasOutgoingExec = true;
				break;
			}
		}

		if (!bHasOutgoingExec)
		{
			continue;
		}

		TSet<UEdGraphNode*> ActiveStack;
		MaxDepth = FMath::Max(MaxDepth, ComputeLongestExecDepthFromNode(Node, Memo, ActiveStack));
	}

	return MaxDepth;
}

TSharedPtr<FJsonObject> BuildComplexityMetrics(UBlueprint* BP)
{
	int32 TotalNodes = 0;
	int32 TotalConnections = 0;
	int32 MaxGraphDepth = 0;
	int32 LatentCount = 0;
	bool bHasTick = false;
	bool bHasTimelines = BP && BP->Timelines.Num() > 0;
	bool bHasDispatchers = false;
	bool bHasInterfaces = BP && BP->ImplementedInterfaces.Num() > 0;

	TSet<FString> UnsupportedNodeClasses;
	TSet<FString> KnownNodePrefixes = {
		TEXT("K2Node_"),
		TEXT("EdGraphNode_Comment"),
		TEXT("MaterialGraphNode_"),
	};

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		MaxGraphDepth = FMath::Max(MaxGraphDepth, ComputeGraphExecDepth(Graph));
		TotalNodes += Graph->Nodes.Num();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					TotalConnections += Pin->LinkedTo.Num();
				}
			}

			if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->IsLatentFunction())
				{
					++LatentCount;
				}
			}

			if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				const FName EventName = EventNode->EventReference.GetMemberName();
				if (EventName == TEXT("ReceiveTick") || EventName == TEXT("Tick"))
				{
					bHasTick = true;
				}
			}

			const FString NodeClassName = Node->GetClass()->GetName();
			bool bKnownPrefix = false;
			for (const FString& Prefix : KnownNodePrefixes)
			{
				if (NodeClassName.StartsWith(Prefix))
				{
					bKnownPrefix = true;
					break;
				}
			}
			if (!bKnownPrefix)
			{
				UnsupportedNodeClasses.Add(NodeClassName);
			}
		}
	}

	// Delegate fields usually live on generated/skeleton class.
	const UClass* DelegateClass = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;
	if (DelegateClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(DelegateClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			bHasDispatchers = true;
			break;
		}
	}

	TArray<TSharedPtr<FJsonValue>> UnsupportedArray;
	for (const FString& NodeClass : UnsupportedNodeClasses)
	{
		UnsupportedArray.Add(MakeShared<FJsonValueString>(NodeClass));
	}

	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	Metrics->SetNumberField(TEXT("total_nodes"), TotalNodes);
	Metrics->SetNumberField(TEXT("total_connections"), TotalConnections);
	Metrics->SetNumberField(TEXT("max_graph_depth"), MaxGraphDepth);
	Metrics->SetBoolField(TEXT("has_tick"), bHasTick);
	Metrics->SetBoolField(TEXT("has_timelines"), bHasTimelines);
	Metrics->SetBoolField(TEXT("has_latent_nodes"), LatentCount > 0);
	Metrics->SetBoolField(TEXT("has_event_dispatchers"), bHasDispatchers);
	Metrics->SetBoolField(TEXT("has_interfaces"), bHasInterfaces);
	Metrics->SetStringField(TEXT("migration_confidence"), CalculateMigrationConfidence(TotalNodes, LatentCount));
	Metrics->SetArrayField(TEXT("unsupported_node_types"), UnsupportedArray);
	return Metrics;
}
} // namespace

FCortexCommandResult FCortexBPAnalysisOps::AnalyzeForMigration(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path"));
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (BP == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), BP->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("type"), FCortexBPAssetOps::DetermineBlueprintType(BP));
	Data->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT(""));
	Data->SetBoolField(TEXT("is_compiled"), BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings);

	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FBPVariableDescription& Variable : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Variable.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Variable.VarType));
		VarObj->SetStringField(TEXT("default_value"), Variable.DefaultValue);
		VarObj->SetBoolField(TEXT("is_exposed"), (Variable.PropertyFlags & CPF_BlueprintVisible) != 0);
		VarObj->SetBoolField(TEXT("is_replicated"), (Variable.PropertyFlags & CPF_Net) != 0);
		VarObj->SetStringField(TEXT("category"), Variable.Category.ToString());

		// Container type from pin type
		FString ContainerTypeStr = TEXT("None");
		switch (Variable.VarType.ContainerType)
		{
		case EPinContainerType::Array:
			ContainerTypeStr = TEXT("Array");
			break;
		case EPinContainerType::Set:
			ContainerTypeStr = TEXT("Set");
			break;
		case EPinContainerType::Map:
			ContainerTypeStr = TEXT("Map");
			break;
		default:
			break;
		}
		VarObj->SetStringField(TEXT("container_type"), ContainerTypeStr);

		VarObj->SetNumberField(TEXT("usage_count"), CountVariableUsage(BP, Variable.VarName));

		// V3: UPROPERTY specifier resolution
		const uint64 Flags = Variable.PropertyFlags;
		VarObj->SetStringField(TEXT("uproperty_specifier"), ResolveUPropertySpecifier(Flags));
		VarObj->SetStringField(TEXT("blueprint_access"),
			(Flags & CPF_BlueprintReadOnly) != 0 ? TEXT("ReadOnly") : TEXT("ReadWrite"));
		VarObj->SetBoolField(TEXT("is_save_game"), (Flags & CPF_SaveGame) != 0);
		VarObj->SetBoolField(TEXT("is_transient"), (Flags & CPF_Transient) != 0);

		// V3: Reference type
		VarObj->SetStringField(TEXT("reference_type"), ResolveReferenceType(Variable.VarType));

		// V3: Replication details
		TSharedPtr<FJsonObject> ReplicationObj = MakeShared<FJsonObject>();
		const bool bReplicated = (Flags & CPF_Net) != 0;
		ReplicationObj->SetBoolField(TEXT("is_replicated"), bReplicated);
		if (bReplicated)
		{
			const UEnum* CondEnum = StaticEnum<ELifetimeCondition>();
			const FString CondStr = CondEnum
				? CondEnum->GetNameStringByValue(static_cast<int64>(Variable.ReplicationCondition.GetValue()))
				: TEXT("COND_None");
			ReplicationObj->SetStringField(TEXT("condition"), CondStr);
			ReplicationObj->SetStringField(TEXT("notify_func"), Variable.RepNotifyFunc.ToString());
		}
		else
		{
			ReplicationObj->SetStringField(TEXT("condition"), TEXT("COND_None"));
			ReplicationObj->SetStringField(TEXT("notify_func"), TEXT(""));
		}
		VarObj->SetObjectField(TEXT("replication"), ReplicationObj);

		// V3: Gameplay Tag detection
		bool bIsGameplayTag = false;
		FString GameplayTagType;
		if (Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			const UScriptStruct* Struct = Cast<UScriptStruct>(Variable.VarType.PinSubCategoryObject.Get());
			if (Struct == TBaseStructure<FGameplayTag>::Get())
			{
				bIsGameplayTag = true;
				GameplayTagType = TEXT("FGameplayTag");
			}
			else if (Struct == TBaseStructure<FGameplayTagContainer>::Get())
			{
				bIsGameplayTag = true;
				GameplayTagType = TEXT("FGameplayTagContainer");
			}
		}
		VarObj->SetBoolField(TEXT("is_gameplay_tag"), bIsGameplayTag);
		if (bIsGameplayTag)
		{
			VarObj->SetStringField(TEXT("gameplay_tag_type"), GameplayTagType);
		}

		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	Data->SetArrayField(TEXT("variables"), VariablesArray);

	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Graph->GetName());
		FuncObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		FuncObj->SetBoolField(TEXT("has_latent_nodes"), HasLatentNodesInGraph(Graph));
		FuncObj->SetBoolField(TEXT("is_pure"), IsFunctionPure(Graph));

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
					PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PinObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					InputsArr.Add(MakeShared<FJsonValueObject>(PinObj));
				}
			}
			else if (const UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				for (const TSharedPtr<FUserPinInfo>& Pin : ResultNode->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
					PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PinObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					OutputsArr.Add(MakeShared<FJsonValueObject>(PinObj));
				}
			}
		}
		FuncObj->SetArrayField(TEXT("inputs"), InputsArr);
		FuncObj->SetArrayField(TEXT("outputs"), OutputsArr);
		FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}
	Data->SetArrayField(TEXT("functions"), FunctionsArray);

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (BP->SimpleConstructionScript)
	{
		USCS_Node* RootNode = BP->SimpleConstructionScript->GetDefaultSceneRootNode();
		for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!SCSNode || !SCSNode->ComponentTemplate)
			{
				continue;
			}

			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("class"), SCSNode->ComponentTemplate->GetClass()->GetName());
			CompObj->SetBoolField(TEXT("is_root"), SCSNode == RootNode);
			CompObj->SetBoolField(TEXT("is_scene_component"), SCSNode->ComponentTemplate->IsA<USceneComponent>());
			CompObj->SetStringField(TEXT("parent_component"), SCSNode->ParentComponentOrVariableName.ToString());
			ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}
	Data->SetArrayField(TEXT("components"), ComponentsArray);

	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	TArray<TSharedPtr<FJsonValue>> LatentNodesArray;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		bool bGraphHasTick = false;
		TArray<TSharedPtr<FJsonValue>> EventsArr;
		TArray<TSharedPtr<FJsonValue>> CustomEventsArr;
		TSharedPtr<FJsonObject> CustomEventParamsObj = MakeShared<FJsonObject>();
		TArray<UEdGraphNode*> GraphLatentNodes;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				const FString EventName = EventNode->EventReference.GetMemberName().ToString();
				if (!EventName.IsEmpty())
				{
					EventsArr.Add(MakeShared<FJsonValueString>(EventName));
				}
				if (EventName == TEXT("ReceiveTick") || EventName == TEXT("Tick"))
				{
					bGraphHasTick = true;
				}
			}

			if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				const FString CustomEventName = CustomEvent->CustomFunctionName.ToString();
				TArray<TSharedPtr<FJsonValue>> ParamArray;
				for (const TSharedPtr<FUserPinInfo>& Pin : CustomEvent->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					ParamObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
				CustomEventsArr.Add(MakeShared<FJsonValueString>(CustomEventName));
				CustomEventParamsObj->SetArrayField(CustomEventName, ParamArray);
			}

			if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->IsLatentFunction())
				{
					GraphLatentNodes.Add(Node);
				}
			}
		}

		// Second pass: compute sequential chain info for latent nodes in this graph
		for (UEdGraphNode* LatentNode : GraphLatentNodes)
		{
			const UK2Node_CallFunction* CallNode = CastChecked<UK2Node_CallFunction>(LatentNode);

			TSharedPtr<FJsonObject> LatentObj = MakeShared<FJsonObject>();
			LatentObj->SetStringField(TEXT("node_type"), LatentNode->GetClass()->GetName());
			LatentObj->SetStringField(TEXT("graph"), Graph->GetName());

			// Extract duration pin value
			const FString DurationValue = ExtractDurationPinValue(CallNode);
			if (!DurationValue.IsEmpty())
			{
				LatentObj->SetStringField(TEXT("duration_pin_value"), DurationValue);
			}

			// Compute sequential chain: how many latent nodes follow this one?
			TSet<UEdGraphNode*> Visited;
			const int32 DownstreamCount = CountDownstreamLatentNodes(LatentNode, Visited);
			const int32 ChainLength = 1 + DownstreamCount;
			const bool bIsSequential = ChainLength > 1;

			LatentObj->SetBoolField(TEXT("is_sequential"), bIsSequential);
			LatentObj->SetNumberField(TEXT("sequence_length"), ChainLength);

			// Sequence index: find how many latent nodes are upstream of this one in the chain
			int32 SequenceIndex = 0;
			for (UEdGraphNode* OtherLatent : GraphLatentNodes)
			{
				if (OtherLatent == LatentNode)
				{
					continue;
				}
				// Check if OtherLatent has this node downstream
				TSet<UEdGraphNode*> CheckVisited;
				CountDownstreamLatentNodes(OtherLatent, CheckVisited);
				if (CheckVisited.Contains(LatentNode))
				{
					++SequenceIndex;
				}
			}
			LatentObj->SetNumberField(TEXT("sequence_index"), SequenceIndex);

			LatentNodesArray.Add(MakeShared<FJsonValueObject>(LatentObj));
		}

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph->GetName());
		GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphObj->SetBoolField(TEXT("has_tick"), bGraphHasTick);
		GraphObj->SetArrayField(TEXT("events"), EventsArr);
		GraphObj->SetArrayField(TEXT("custom_events"), CustomEventsArr);
		GraphObj->SetObjectField(TEXT("custom_event_params"), CustomEventParamsObj);
		GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	Data->SetArrayField(TEXT("latent_nodes"), LatentNodesArray);

	TArray<TSharedPtr<FJsonValue>> TimelinesArray;
	for (UTimelineTemplate* Timeline : BP->Timelines)
	{
		if (!Timeline)
		{
			continue;
		}

		TSharedPtr<FJsonObject> TimelineObj = MakeShared<FJsonObject>();
		TimelineObj->SetStringField(TEXT("name"), Timeline->GetVariableName().ToString());
		TimelineObj->SetNumberField(TEXT("length"), Timeline->TimelineLength);
		TimelineObj->SetBoolField(TEXT("auto_play"), Timeline->bAutoPlay);
		TimelineObj->SetBoolField(TEXT("loop"), Timeline->bLoop);
		// UTimelineTemplate does not store per-template play rate in UE 5.6.
		// Runtime UTimelineComponent defaults to 1.0 unless adjusted by graph logic.
		TimelineObj->SetNumberField(TEXT("play_rate"), 1.0);
		TimelineObj->SetBoolField(TEXT("ignore_time_dilation"), Timeline->bIgnoreTimeDilation);

		TArray<TSharedPtr<FJsonValue>> FloatTracksArray;
		for (const FTTFloatTrack& Track : Timeline->FloatTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
			TArray<TSharedPtr<FJsonValue>> KeysArray;
			if (Track.CurveFloat)
			{
				for (const FRichCurveKey& Key : Track.CurveFloat->FloatCurve.GetConstRefOfKeys())
				{
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), Key.Time);
					KeyObj->SetNumberField(TEXT("value"), Key.Value);
					KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			FloatTracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
		}
		TimelineObj->SetArrayField(TEXT("float_tracks"), FloatTracksArray);

		TArray<TSharedPtr<FJsonValue>> VectorTracksArray;
		for (const FTTVectorTrack& Track : Timeline->VectorTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
			TArray<TSharedPtr<FJsonValue>> KeysArray;
			if (Track.CurveVector)
			{
				const FRichCurve& XCurve = Track.CurveVector->FloatCurves[0];
				const FRichCurve& YCurve = Track.CurveVector->FloatCurves[1];
				const FRichCurve& ZCurve = Track.CurveVector->FloatCurves[2];
				for (const FRichCurveKey& Key : XCurve.GetConstRefOfKeys())
				{
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), Key.Time);
					KeyObj->SetNumberField(TEXT("x"), XCurve.Eval(Key.Time));
					KeyObj->SetNumberField(TEXT("y"), YCurve.Eval(Key.Time));
					KeyObj->SetNumberField(TEXT("z"), ZCurve.Eval(Key.Time));
					KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			VectorTracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
		}
		TimelineObj->SetArrayField(TEXT("vector_tracks"), VectorTracksArray);

		TArray<TSharedPtr<FJsonValue>> ColorTracksArray;
		for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
			TArray<TSharedPtr<FJsonValue>> KeysArray;
			if (Track.CurveLinearColor)
			{
				const FRichCurve& RCurve = Track.CurveLinearColor->FloatCurves[0];
				for (const FRichCurveKey& Key : RCurve.GetConstRefOfKeys())
				{
					const FLinearColor Value = Track.CurveLinearColor->GetLinearColorValue(Key.Time);
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), Key.Time);
					KeyObj->SetNumberField(TEXT("r"), Value.R);
					KeyObj->SetNumberField(TEXT("g"), Value.G);
					KeyObj->SetNumberField(TEXT("b"), Value.B);
					KeyObj->SetNumberField(TEXT("a"), Value.A);
					KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			ColorTracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
		}
		TimelineObj->SetArrayField(TEXT("color_tracks"), ColorTracksArray);

		TArray<TSharedPtr<FJsonValue>> EventTracksArray;
		for (const FTTEventTrack& Track : Timeline->EventTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("name"), Track.GetTrackName().ToString());
			TArray<TSharedPtr<FJsonValue>> KeysArray;
			if (Track.CurveKeys)
			{
				for (const FRichCurveKey& Key : Track.CurveKeys->FloatCurve.GetConstRefOfKeys())
				{
					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), Key.Time);
					KeyObj->SetStringField(TEXT("event_name"), Track.GetFunctionName().ToString());
					KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
				}
			}
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			EventTracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
		}
		TimelineObj->SetArrayField(TEXT("event_tracks"), EventTracksArray);

		TimelinesArray.Add(MakeShared<FJsonValueObject>(TimelineObj));
	}
	Data->SetArrayField(TEXT("timelines"), TimelinesArray);

	TArray<TSharedPtr<FJsonValue>> DispatchersArray;
	const UClass* DelegateClass = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass : BP->GeneratedClass;
	if (DelegateClass)
	{
		for (TFieldIterator<FMulticastDelegateProperty> It(DelegateClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FMulticastDelegateProperty* DelegateProp = *It;
			if (!DelegateProp)
			{
				continue;
			}

			TSharedPtr<FJsonObject> DispatcherObj = MakeShared<FJsonObject>();
			DispatcherObj->SetStringField(TEXT("name"), DelegateProp->GetName());
			TArray<TSharedPtr<FJsonValue>> ParamsArray;
			if (UFunction* SignatureFunc = DelegateProp->SignatureFunction)
			{
				for (TFieldIterator<FProperty> ParamIt(SignatureFunc); ParamIt; ++ParamIt)
				{
					FProperty* Param = *ParamIt;
					if (Param
						&& Param->HasAnyPropertyFlags(CPF_Parm)
						&& !Param->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
						ParamObj->SetStringField(TEXT("name"), Param->GetName());
						ParamObj->SetStringField(TEXT("type"), Param->GetCPPType());
						ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
					}
				}
			}
			DispatcherObj->SetArrayField(TEXT("params"), ParamsArray);
			DispatchersArray.Add(MakeShared<FJsonValueObject>(DispatcherObj));
		}
	}
	Data->SetArrayField(TEXT("event_dispatchers"), DispatchersArray);

	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface)
		{
			continue;
		}

		TSharedPtr<FJsonObject> InterfaceObj = MakeShared<FJsonObject>();
		InterfaceObj->SetStringField(TEXT("name"), InterfaceDesc.Interface->GetName());
		TArray<TSharedPtr<FJsonValue>> InterfaceFunctions;
		for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
		{
			if (InterfaceGraph)
			{
				InterfaceFunctions.Add(MakeShared<FJsonValueString>(InterfaceGraph->GetName()));
			}
		}
		InterfaceObj->SetArrayField(TEXT("functions"), InterfaceFunctions);
		InterfacesArray.Add(MakeShared<FJsonValueObject>(InterfaceObj));
	}
	Data->SetArrayField(TEXT("interfaces_implemented"), InterfacesArray);

	Data->SetObjectField(TEXT("complexity_metrics"), BuildComplexityMetrics(BP));

	return FCortexCommandRouter::Success(Data);
}
