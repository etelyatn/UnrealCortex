#include "Operations/CortexBPAnalysisOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
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
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"

namespace
{
FString DetermineBlueprintTypeForAnalysis(const UBlueprint* BP)
{
	if (!BP || !BP->ParentClass)
	{
		return TEXT("Unknown");
	}

	if (BP->BlueprintType == BPTYPE_Interface)
	{
		return TEXT("Interface");
	}

	if (BP->BlueprintType == BPTYPE_FunctionLibrary)
	{
		return TEXT("FunctionLibrary");
	}

	static UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	if (UserWidgetClass && BP->ParentClass->IsChildOf(UserWidgetClass))
	{
		return TEXT("Widget");
	}

	if (BP->ParentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return TEXT("Component");
	}

	if (BP->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		return TEXT("Actor");
	}

	return TEXT("Unknown");
}

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

		MaxGraphDepth = FMath::Max(MaxGraphDepth, Graph->Nodes.Num());
		TotalNodes += Graph->Nodes.Num();

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
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
	Metrics->SetBoolField(TEXT("has_latent"), LatentCount > 0);
	Metrics->SetBoolField(TEXT("has_dispatchers"), bHasDispatchers);
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
	Data->SetStringField(TEXT("type"), DetermineBlueprintTypeForAnalysis(BP));
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
		VarObj->SetStringField(TEXT("category"), Variable.Category.ToString());
		VarObj->SetNumberField(TEXT("usage_count"), CountVariableUsage(BP, Variable.VarName));
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
				TSharedPtr<FJsonObject> CustomEventObj = MakeShared<FJsonObject>();
				CustomEventObj->SetStringField(TEXT("name"), CustomEvent->GetName());
				TArray<TSharedPtr<FJsonValue>> ParamArray;
				for (const TSharedPtr<FUserPinInfo>& Pin : CustomEvent->UserDefinedPins)
				{
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					ParamObj->SetStringField(TEXT("type"), CortexBPTypeUtils::FriendlyTypeName(Pin->PinType));
					ParamArray.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
				CustomEventObj->SetArrayField(TEXT("params"), ParamArray);
				CustomEventsArr.Add(MakeShared<FJsonValueObject>(CustomEventObj));
			}

			if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->IsLatentFunction())
				{
					TSharedPtr<FJsonObject> LatentObj = MakeShared<FJsonObject>();
					LatentObj->SetStringField(TEXT("node_type"), Node->GetClass()->GetName());
					LatentObj->SetStringField(TEXT("graph"), Graph->GetName());
					LatentObj->SetBoolField(TEXT("is_sequential"), false);
					LatentObj->SetNumberField(TEXT("sequence_index"), 0);
					LatentObj->SetNumberField(TEXT("sequence_length"), 1);
					LatentNodesArray.Add(MakeShared<FJsonValueObject>(LatentObj));
				}
			}
		}

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph->GetName());
		GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphObj->SetBoolField(TEXT("has_tick"), bGraphHasTick);
		GraphObj->SetArrayField(TEXT("events"), EventsArr);
		GraphObj->SetArrayField(TEXT("custom_events"), CustomEventsArr);
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
		TimelineObj->SetArrayField(TEXT("vector_tracks"), TArray<TSharedPtr<FJsonValue>>{});
		TimelineObj->SetArrayField(TEXT("color_tracks"), TArray<TSharedPtr<FJsonValue>>{});
		TimelineObj->SetArrayField(TEXT("event_tracks"), TArray<TSharedPtr<FJsonValue>>{});

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
