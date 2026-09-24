#include "Operations/CortexBPRemoveGraphOps.h"

#include "CortexGraphFingerprint.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Knot.h"
#include "CortexAssetMutationGuard.h"
#include "ScopedTransaction.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "IO/IoHash.h"
#include "Misc/Char.h"
#include "Containers/StringConv.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace
{
#if WITH_AUTOMATION_TESTS
FName RemoveGraphFaultPoint;
#endif

bool ShouldInjectFault(const TCHAR* Point)
{
#if WITH_AUTOMATION_TESTS
	return RemoveGraphFaultPoint == FName(Point);
#else
	return false;
#endif
}

bool ReadRequiredStrictBool(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	bool& OutValue,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonValue> JsonValue = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean || !Params->TryGetBoolField(Field, OutValue))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be an explicit boolean"), Field));
		return false;
	}
	return true;
}

bool ReadOptionalStrictBool(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	bool& OutValue,
	FCortexCommandResult& OutError)
{
	if (!Params->HasField(Field))
	{
		OutValue = false;
		return true;
	}
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(Field);
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean || !Params->TryGetBoolField(Field, OutValue))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a boolean"), Field));
		return false;
	}
	return true;
}

bool ReadRequiredStrictString(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	FString& OutValue,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonValue> JsonValue = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::String
		|| !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a non-empty string"), Field));
		return false;
	}
	return true;
}

void AppendQuoted(const FString& Value, FString& Out)
{
	Out += TCHAR('"');
	for (const TCHAR Char : Value)
	{
		switch (Char)
		{
		case TCHAR('\\'): Out += TEXT("\\\\"); break;
		case TCHAR('"'): Out += TEXT("\\\""); break;
		case TCHAR('\b'): Out += TEXT("\\b"); break;
		case TCHAR('\f'): Out += TEXT("\\f"); break;
		case TCHAR('\n'): Out += TEXT("\\n"); break;
		case TCHAR('\r'): Out += TEXT("\\r"); break;
		case TCHAR('\t'): Out += TEXT("\\t"); break;
		default:
			if (Char < 0x20)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Char));
			}
			else
			{
				Out += Char;
			}
			break;
		}
	}
	Out += TCHAR('"');
}

void AppendCanonical(const TSharedPtr<FJsonValue>& Value, FString& Out);

void AppendCanonicalObject(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
	Out += TCHAR('{');
	if (Object.IsValid())
	{
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
		Fields.Reserve(Object->Values.Num());
		for (const auto& Pair : Object->Values)
		{
			Fields.Emplace(CortexEngineCompat::JsonKeyToString(Pair.Key), Pair.Value);
		}
		Fields.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& Left, const TPair<FString, TSharedPtr<FJsonValue>>& Right)
		{
			return Left.Key < Right.Key;
		});
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			if (Index > 0) Out += TCHAR(',');
			AppendQuoted(Fields[Index].Key, Out);
			Out += TCHAR(':');
			AppendCanonical(Fields[Index].Value, Out);
		}
	}
	Out += TCHAR('}');
}

void AppendCanonical(const TSharedPtr<FJsonValue>& Value, FString& Out)
{
	if (!Value.IsValid())
	{
		Out += TEXT("null");
		return;
	}
	switch (Value->Type)
	{
	case EJson::Object:
		AppendCanonicalObject(Value->AsObject(), Out);
		break;
	case EJson::Array:
		{
			Out += TCHAR('[');
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (Index > 0) Out += TCHAR(',');
				AppendCanonical(Array[Index], Out);
			}
			Out += TCHAR(']');
		}
		break;
	case EJson::String:
		AppendQuoted(Value->AsString(), Out);
		break;
	case EJson::Number:
		Out += FString::Printf(TEXT("%.17g"), Value->AsNumber());
		break;
	case EJson::Boolean:
		Out += Value->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Null:
	default:
		Out += TEXT("null");
		break;
	}
}

FString CanonicalObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Result;
	AppendCanonicalObject(Object, Result);
	return Result;
}

void GatherCascadeExecNodes(UEdGraphNode* StartNode, TSet<UEdGraphNode*>& OutRemovalSet)
{
	if (!StartNode)
	{
		return;
	}

	OutRemovalSet.Add(StartNode);
	TArray<UEdGraphNode*> Queue;
	Queue.Add(StartNode);
	int32 QueueIndex = 0;
	while (QueueIndex < Queue.Num())
	{
		UEdGraphNode* Current = Queue[QueueIndex++];
		if (!Current)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			const bool bCurrentIsKnot = Cast<UK2Node_Knot>(Current) != nullptr;
			const bool bPinIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			if (!bPinIsExec && !bCurrentIsKnot)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin)
				{
					continue;
				}
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (!LinkedNode || OutRemovalSet.Contains(LinkedNode))
				{
					continue;
				}
				if (bCurrentIsKnot && !bPinIsExec)
				{
					const bool bDestinationExecOrKnot = Cast<UK2Node_Knot>(LinkedNode)
						|| LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
					if (!bDestinationExecOrKnot)
					{
						continue;
					}
				}
				bool bHasExternalExecInput = false;
				for (UEdGraphPin* NodePin : LinkedNode->Pins)
				{
					if (!NodePin || NodePin->Direction != EGPD_Input)
					{
						continue;
					}
					const bool bNodePinIsExec = NodePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				const bool bLinkedIsKnot = Cast<UK2Node_Knot>(LinkedNode) != nullptr;
				if (!bNodePinIsExec && !bLinkedIsKnot)
				{
					continue;
				}
				for (UEdGraphPin* IncomingPin : NodePin->LinkedTo)
				{
					if (IncomingPin)
					{
						UEdGraphNode* IncomingNode = IncomingPin->GetOwningNode();
						if (IncomingNode && !OutRemovalSet.Contains(IncomingNode))
						{
							bHasExternalExecInput = true;
							break;
						}
					}
				}
				if (bHasExternalExecInput)
				{
					break;
				}
				}
				if (!bHasExternalExecInput)
				{
					OutRemovalSet.Add(LinkedNode);
					Queue.Add(LinkedNode);
				}
			}
		}
	}
}

void AddNodeInventory(UEdGraphNode* Node, TArray<TSharedPtr<FJsonValue>>& OutInventory)
{
	if (!Node)
	{
		return;
	}
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	const FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
	if (!NodeTitle.IsEmpty())
	{
		NodeObj->SetStringField(TEXT("title"), NodeTitle.ToString());
	}
	OutInventory.Add(MakeShared<FJsonValueObject>(NodeObj));
}

FCortexCommandResult InvalidOperation(const TCHAR* Message)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, Message);
}

bool BuildPlan(
	UBlueprint* Blueprint,
	FCortexBPRemoveGraphPrepared& Prepared,
	TArray<TSharedPtr<FJsonValue>>& OutRemovedNodes,
	TSharedPtr<FJsonObject>& OutRemoved,
	FCortexCommandResult& OutError)
{
	const FString ResolvedName = Prepared.Name == TEXT("ConstructionScript")
		? TEXT("UserConstructionScript") : Prepared.Name;

	if (Blueprint->UbergraphPages.Num() > 0 && Blueprint->UbergraphPages[0]
		&& ResolvedName == Blueprint->UbergraphPages[0]->GetName())
	{
		OutError = InvalidOperation(TEXT("Cannot remove primary EventGraph — it is a protected structural graph"));
		return false;
	}
	if (ResolvedName == TEXT("UserConstructionScript"))
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName() == ResolvedName)
			{
				OutError = InvalidOperation(TEXT("Cannot remove ConstructionScript — it is a protected structural graph"));
				return false;
			}
		}
	}

	UEdGraph* FoundGraph = nullptr;
	FString FoundType;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == ResolvedName)
		{
			FoundGraph = Graph;
			FoundType = TEXT("Function");
			break;
		}
	}
	if (!FoundGraph)
	{
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph && Graph->GetName() == ResolvedName)
			{
				FoundGraph = Graph;
				FoundType = TEXT("Macro");
				break;
			}
		}
	}
	if (!FoundGraph)
	{
		for (int32 Index = 1; Index < Blueprint->UbergraphPages.Num(); ++Index)
		{
			UEdGraph* Graph = Blueprint->UbergraphPages[Index];
			if (Graph && Graph->GetName() == ResolvedName)
			{
				FoundGraph = Graph;
				FoundType = TEXT("EventGraph");
				break;
			}
		}
	}

	if (FoundGraph)
	{
		if (!FoundGraph->GraphGuid.IsValid())
		{
			OutError = InvalidOperation(TEXT("Cannot remove a graph with an invalid graph GUID"));
			return false;
		}
		Prepared.Target = MakeShared<FJsonObject>();
		Prepared.Target->SetStringField(TEXT("kind"), TEXT("graph"));
		Prepared.Target->SetStringField(TEXT("graph_guid"), FoundGraph->GraphGuid.ToString());
		Prepared.Target->SetStringField(TEXT("graph_type"), FoundType);
		Prepared.Target->SetStringField(TEXT("name"), FoundGraph->GetName());

		Prepared.Deletion = MakeShared<FJsonObject>();
		Prepared.Deletion->SetStringField(TEXT("kind"), TEXT("graph"));
		Prepared.Deletion->SetStringField(TEXT("graph_guid"), FoundGraph->GraphGuid.ToString());
		Prepared.Deletion->SetNumberField(TEXT("node_count"), FoundGraph->Nodes.Num());

		TArray<UEdGraphNode*> SortedNodes;
		for (UEdGraphNode* Node : FoundGraph->Nodes)
		{
			if (Node)
			{
				SortedNodes.Add(Node);
			}
		}
		SortedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});
		for (UEdGraphNode* Node : SortedNodes)
		{
			AddNodeInventory(Node, OutRemovedNodes);
		}
		OutRemoved = MakeShared<FJsonObject>();
		OutRemoved->SetStringField(TEXT("name"), Prepared.Name);
		OutRemoved->SetStringField(TEXT("type"), FoundType);
		OutRemoved->SetNumberField(TEXT("node_count"), FoundGraph->Nodes.Num());
		return true;
	}

	UK2Node_CustomEvent* FoundEvent = nullptr;
	UEdGraph* OwningGraph = nullptr;
	for (UEdGraph* EventGraph : Blueprint->UbergraphPages)
	{
		if (!EventGraph)
		{
			continue;
		}
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
			if (CustomEvent && CustomEvent->CustomFunctionName.ToString() == ResolvedName)
			{
				FoundEvent = CustomEvent;
				OwningGraph = EventGraph;
				break;
			}
		}
		if (FoundEvent)
		{
			break;
		}
	}
	if (FoundEvent)
	{
		if (!OwningGraph->GraphGuid.IsValid() || !FoundEvent->NodeGuid.IsValid())
		{
			OutError = InvalidOperation(TEXT("Cannot remove a custom event with an invalid graph or node GUID"));
			return false;
		}
		TSet<UEdGraphNode*> RemovalSet;
		if (Prepared.bCascadeExecChain)
		{
			GatherCascadeExecNodes(FoundEvent, RemovalSet);
		}
		else
		{
			RemovalSet.Add(FoundEvent);
		}
		TArray<UEdGraphNode*> SortedNodes = RemovalSet.Array();
		for (UEdGraphNode* Node : SortedNodes)
		{
			if (!Node || !Node->NodeGuid.IsValid())
			{
				OutError = InvalidOperation(TEXT("Cannot remove a node with an invalid node GUID"));
				return false;
			}
		}
		SortedNodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});

		Prepared.Target = MakeShared<FJsonObject>();
		Prepared.Target->SetStringField(TEXT("kind"), TEXT("custom_event"));
		Prepared.Target->SetStringField(TEXT("graph_guid"), OwningGraph->GraphGuid.ToString());
		Prepared.Target->SetStringField(TEXT("node_guid"), FoundEvent->NodeGuid.ToString());
		Prepared.Target->SetStringField(TEXT("name"), FoundEvent->CustomFunctionName.ToString());

		Prepared.Deletion = MakeShared<FJsonObject>();
		Prepared.Deletion->SetStringField(TEXT("kind"), TEXT("nodes"));
		TArray<TSharedPtr<FJsonValue>> NodeGuids;
		for (UEdGraphNode* Node : SortedNodes)
		{
			NodeGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
			AddNodeInventory(Node, OutRemovedNodes);
		}
		Prepared.Deletion->SetArrayField(TEXT("node_guids"), NodeGuids);
		OutRemoved = MakeShared<FJsonObject>();
		OutRemoved->SetStringField(TEXT("name"), Prepared.Name);
		OutRemoved->SetStringField(TEXT("type"), TEXT("CustomEvent"));
		OutRemoved->SetNumberField(TEXT("node_count"), RemovalSet.Num());
		OutRemoved->SetBoolField(TEXT("cascade_exec_chain"), Prepared.bCascadeExecChain);
		return true;
	}

	TArray<FString> AvailableNames;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph) AvailableNames.Add(Graph->GetName());
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph) AvailableNames.Add(Graph->GetName());
	}
	for (int32 Index = 1; Index < Blueprint->UbergraphPages.Num(); ++Index)
	{
		if (Blueprint->UbergraphPages[Index]) AvailableNames.Add(Blueprint->UbergraphPages[Index]->GetName());
	}
	for (UEdGraph* EventGraph : Blueprint->UbergraphPages)
	{
		if (!EventGraph) continue;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				AvailableNames.Add(CustomEvent->CustomFunctionName.ToString());
			}
		}
	}
	const FString AvailableHint = AvailableNames.Num() > 0
		? FString::Printf(TEXT(" Available: [%s]"), *FString::Join(AvailableNames, TEXT(", ")))
		: TEXT("");
	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::GraphNotFound,
		FString::Printf(TEXT("Graph or custom event '%s' not found in %s.%s"),
			*Prepared.Name, *Blueprint->GetName(), *AvailableHint));
	return false;
}

void BuildValidationHash(FCortexBPRemoveGraphPrepared& Prepared)
{
	TSharedPtr<FJsonObject> Intent = MakeShared<FJsonObject>();
	Intent->SetStringField(TEXT("asset_path"), Prepared.AssetPath);
	Intent->SetStringField(TEXT("name"), Prepared.Name);
	Intent->SetBoolField(TEXT("cascade_exec_chain"), Prepared.bCascadeExecChain);
	Intent->SetBoolField(TEXT("compile"), Prepared.bCompile);
	Intent->SetObjectField(TEXT("target"), Prepared.Target);
	Intent->SetObjectField(TEXT("deletion"), Prepared.Deletion);
	Intent->SetObjectField(TEXT("expected_fingerprint"), Prepared.FingerprintBefore);
	const FString Source = TEXT("blueprint_remove_graph_v1|") + CanonicalObject(Intent);
	FTCHARToUTF8 Utf8(*Source);
	Prepared.ValidationHash = LexToString(FIoHash::HashBuffer(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

struct FRemoveGraphJournal
{
	bool bPackageWasDirty = false;
	EBlueprintStatus StatusBefore = BS_Unknown;
	FString GraphFingerprintBefore;
	FString GeneratedStateBefore;
	TStrongObjectPtr<UBlueprint> SnapshotBlueprint;
	FGuid TargetGraphGuid;
	FString GraphType;
	FString GraphName;
	TStrongObjectPtr<UEdGraph> OriginalGraph;
	int32 GraphIndex = INDEX_NONE;

	struct FBoundaryLink
	{
		FGuid SourceNodeGuid;
		FName SourcePin;
		FGuid TargetNodeGuid;
		FName TargetPin;
	};
	struct FRemovedNode
	{
		FGuid Guid;
		int32 OriginalIndex = INDEX_NONE;
		TStrongObjectPtr<UEdGraphNode> OriginalNode;
	};
	struct FMacroInstance
	{
		FGuid HostGraphGuid;
		FGuid NodeGuid;
		FName OriginalNodeName;
		int32 OriginalIndex = INDEX_NONE;
		TStrongObjectPtr<UEdGraphNode> Snapshot;
		TStrongObjectPtr<UEdGraphNode> OriginalNode;
	};
	struct FMacroLink
	{
		FGuid HostGraphGuid;
		FGuid SourceNodeGuid;
		FName SourcePin;
		FGuid TargetNodeGuid;
		FName TargetPin;
	};
	TArray<FGuid> PreservedGraphGuids;
	TArray<FGuid> RemovedNodeGuids;
	TArray<FRemovedNode> RemovedNodes;
	TArray<FGuid> PreservedNodeGuids;
	TStrongObjectPtr<UEdGraph> SnapshotGraph;
	TArray<FBoundaryLink> BoundaryLinks;
	TArray<FMacroInstance> ExternalMacroInstances;
	TArray<FMacroLink> ExternalMacroLinks;
	TArray<FEditedDocumentInfo> LastEditedDocuments;
	TArray<FBPVariableDescription> NewVariables;
	struct FOwnedTemplate
	{
		TStrongObjectPtr<UObject> Object;
		TStrongObjectPtr<UObject> OriginalOuter;
		FName OriginalName;
	};
	TArray<FOwnedTemplate> Timelines;
	TArray<FOwnedTemplate> ComponentTemplates;
	TArray<TObjectPtr<UEdGraph>> DelegateSignatureGraphs;
	TArray<FBPInterfaceDescription> ImplementedInterfaces;
	TMap<FGuid, FEditedDocumentInfo> Bookmarks;
};

UEdGraph* FindGraphByGuid(UBlueprint* Blueprint, const FGuid& Guid, FString* OutType = nullptr, int32* OutIndex = nullptr)
{
	auto FindIn = [&](auto& Graphs, const TCHAR* Type, int32 StartIndex) -> UEdGraph*
	{
		for (int32 Index = StartIndex; Index < Graphs.Num(); ++Index)
		{
			UEdGraph* Graph = Graphs[Index];
			if (Graph && Graph->GraphGuid == Guid)
			{
				if (OutType) *OutType = Type;
				if (OutIndex) *OutIndex = Index;
				return Graph;
			}
		}
		return nullptr;
	};
	if (UEdGraph* Graph = FindIn(Blueprint->FunctionGraphs, TEXT("Function"), 0)) return Graph;
	if (UEdGraph* Graph = FindIn(Blueprint->MacroGraphs, TEXT("Macro"), 0)) return Graph;
	if (UEdGraph* Graph = FindIn(Blueprint->UbergraphPages, TEXT("EventGraph"), 0)) return Graph;
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
		if (Graph && Graph->GraphGuid == Guid) return Graph;
	return nullptr;
}
UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
{
	if (!Graph) return nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid) return Node;
	}
	return nullptr;
}

bool CaptureExternalMacroInstances(UBlueprint* Blueprint, UEdGraph* Target, FRemoveGraphJournal& Journal)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	TSet<FGuid> InstanceGuids;
	for (UEdGraph* HostGraph : Graphs)
	{
		if (!HostGraph || HostGraph == Target) continue;
		for (UEdGraphNode* Node : HostGraph->Nodes)
		{
			UK2Node_MacroInstance* Instance = Cast<UK2Node_MacroInstance>(Node);
			if (!Instance || Instance->GetMacroGraph() != Target) continue;
			FRemoveGraphJournal::FMacroInstance& Saved = Journal.ExternalMacroInstances.AddDefaulted_GetRef();
			Saved.HostGraphGuid = HostGraph->GraphGuid;
			Saved.NodeGuid = Instance->NodeGuid;
			Saved.OriginalNode = TStrongObjectPtr<UEdGraphNode>(Instance);
			Saved.OriginalNodeName = Instance->GetFName();
			Saved.OriginalIndex = HostGraph->Nodes.IndexOfByKey(Instance);
			const FName SnapshotName = MakeUniqueObjectName(
				Journal.SnapshotBlueprint.Get(), Instance->GetClass(), Instance->GetFName());
			Saved.Snapshot = TStrongObjectPtr<UEdGraphNode>(
				DuplicateObject<UEdGraphNode>(Instance, Journal.SnapshotBlueprint.Get(), SnapshotName));
			if (!Saved.Snapshot.IsValid() || Saved.OriginalIndex == INDEX_NONE) return false;
			for (UEdGraphPin* Pin : Saved.Snapshot->Pins)
				if (Pin) Pin->LinkedTo.Reset();
			InstanceGuids.Add(Instance->NodeGuid);
		}
	}
	for (UEdGraph* HostGraph : Graphs)
	{
		if (!HostGraph || HostGraph == Target) continue;
		for (UEdGraphNode* Node : HostGraph->Nodes)
		{
			if (!Node || !InstanceGuids.Contains(Node->NodeGuid)) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
					if (!Other) return false;
					if (InstanceGuids.Contains(Other->NodeGuid)
						&& Node->NodeGuid.ToString() > Other->NodeGuid.ToString()) continue;
					Journal.ExternalMacroLinks.Add({
						HostGraph->GraphGuid, Node->NodeGuid, Pin->PinName,
						Other->NodeGuid, Linked->PinName});
				}
			}
		}
	}
	return true;
}

bool RestoreExternalMacroInstances(UBlueprint* Blueprint, FRemoveGraphJournal& Journal, UEdGraph* RestoredMacro)
{
	for (const FRemoveGraphJournal::FMacroInstance& Saved : Journal.ExternalMacroInstances)
	{
		UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Saved.HostGraphGuid);
		if (!HostGraph) return false;
		UEdGraphNode* Node = FindNodeByGuid(HostGraph, Saved.NodeGuid);
		if (!Node)
		{
			if (Saved.OriginalNode.IsValid() && Saved.OriginalNode->GetOuter() == HostGraph
				&& !Saved.OriginalNode->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
			{
				return false;
			}
			Node = DuplicateObject<UEdGraphNode>(Saved.Snapshot.Get(), HostGraph, Saved.OriginalNodeName);
			if (!Node) return false;
			HostGraph->AddNode(Node, false, false);
			const int32 AddedIndex = HostGraph->Nodes.IndexOfByKey(Node);
			if (AddedIndex == INDEX_NONE) return false;
			HostGraph->Nodes.RemoveAt(AddedIndex);
			HostGraph->Nodes.Insert(Node, FMath::Clamp(Saved.OriginalIndex, 0, HostGraph->Nodes.Num()));
		}
		UK2Node_MacroInstance* Instance = Cast<UK2Node_MacroInstance>(Node);
		if (!Instance || Instance->GetFName() != Saved.OriginalNodeName
			|| HostGraph->Nodes.IndexOfByKey(Instance) != Saved.OriginalIndex) return false;
		Instance->SetMacroGraph(RestoredMacro);
	}
	for (const FRemoveGraphJournal::FMacroLink& Link : Journal.ExternalMacroLinks)
	{
		UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Link.HostGraphGuid);
		UEdGraphNode* Source = FindNodeByGuid(HostGraph, Link.SourceNodeGuid);
		UEdGraphNode* Target = FindNodeByGuid(HostGraph, Link.TargetNodeGuid);
		UEdGraphPin* SourcePin = Source ? Source->FindPin(Link.SourcePin) : nullptr;
		UEdGraphPin* TargetPin = Target ? Target->FindPin(Link.TargetPin) : nullptr;
		if (!SourcePin || !TargetPin) return false;
		if (!SourcePin->LinkedTo.Contains(TargetPin)) SourcePin->MakeLinkTo(TargetPin);
	}
	return true;
}

bool CaptureJournal(UBlueprint* Blueprint, const FCortexBPRemoveGraphPrepared& Prepared, FRemoveGraphJournal& Journal)
{
	Journal.bPackageWasDirty = Blueprint->GetOutermost()->IsDirty();
	Journal.StatusBefore = Blueprint->Status;
	Journal.GraphFingerprintBefore = Prepared.FingerprintBefore->GetStringField(TEXT("graph_authoring_hash"));
	if (Prepared.bCompile) Journal.GeneratedStateBefore = FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint);
	Journal.LastEditedDocuments = Blueprint->LastEditedDocuments;
	Journal.NewVariables = Blueprint->NewVariables;
	for (UTimelineTemplate* Timeline : Blueprint->Timelines)
	{
		if (!Timeline) continue;
		FRemoveGraphJournal::FOwnedTemplate& Saved = Journal.Timelines.AddDefaulted_GetRef();
		Saved.Object = TStrongObjectPtr<UObject>(Timeline);
		Saved.OriginalOuter = TStrongObjectPtr<UObject>(Timeline->GetOuter());
		Saved.OriginalName = Timeline->GetFName();
	}
	for (UActorComponent* Component : Blueprint->ComponentTemplates)
	{
		if (!Component) continue;
		FRemoveGraphJournal::FOwnedTemplate& Saved = Journal.ComponentTemplates.AddDefaulted_GetRef();
		Saved.Object = TStrongObjectPtr<UObject>(Component);
		Saved.OriginalOuter = TStrongObjectPtr<UObject>(Component->GetOuter());
		Saved.OriginalName = Component->GetFName();
	}
	Journal.DelegateSignatureGraphs = Blueprint->DelegateSignatureGraphs;
	Journal.ImplementedInterfaces = Blueprint->ImplementedInterfaces;
	Journal.Bookmarks = Blueprint->Bookmarks;
	Journal.SnapshotBlueprint = TStrongObjectPtr<UBlueprint>(NewObject<UBlueprint>(GetTransientPackage()));
	if (!Journal.SnapshotBlueprint.IsValid()) return false;
	const FString Kind = Prepared.Target->GetStringField(TEXT("kind"));
	if (Kind == TEXT("graph"))
	{
		FGuid Guid;
		if (!FGuid::Parse(Prepared.Target->GetStringField(TEXT("graph_guid")), Guid)) return false;
		Journal.TargetGraphGuid = Guid;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			if (Graph && Graph->GraphGuid != Guid) Journal.PreservedGraphGuids.Add(Graph->GraphGuid);
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
			if (Graph && Graph->GraphGuid != Guid) Journal.PreservedGraphGuids.Add(Graph->GraphGuid);
		for (int32 Index = 0; Index < Blueprint->UbergraphPages.Num(); ++Index)
			if (Blueprint->UbergraphPages[Index] && Blueprint->UbergraphPages[Index]->GraphGuid != Guid)
				Journal.PreservedGraphGuids.Add(Blueprint->UbergraphPages[Index]->GraphGuid);
		UEdGraph* Target = FindGraphByGuid(Blueprint, Guid, &Journal.GraphType, &Journal.GraphIndex);
		if (!Target) return false;
		Journal.OriginalGraph = TStrongObjectPtr<UEdGraph>(Target);
		Journal.GraphName = Target->GetName();
		Journal.SnapshotGraph = TStrongObjectPtr<UEdGraph>(
			DuplicateObject<UEdGraph>(Target, Journal.SnapshotBlueprint.Get(), Target->GetFName()));
		if (!Journal.SnapshotGraph.IsValid()) return false;
		return Journal.GraphType != TEXT("Macro") || CaptureExternalMacroInstances(Blueprint, Target, Journal);
	}

	FGuid GraphGuid;
	if (!FGuid::Parse(Prepared.Target->GetStringField(TEXT("graph_guid")), GraphGuid)) return false;
	UEdGraph* Graph = FindGraphByGuid(Blueprint, GraphGuid);
	if (!Graph) return false;
	Journal.TargetGraphGuid = GraphGuid;
	Journal.SnapshotGraph = TStrongObjectPtr<UEdGraph>(
		NewObject<UEdGraph>(Journal.SnapshotBlueprint.Get(), NAME_None, RF_Transient));
	TSet<FGuid> Removal;
	const TArray<TSharedPtr<FJsonValue>>& NodeGuids = Prepared.Deletion->GetArrayField(TEXT("node_guids"));
	for (const TSharedPtr<FJsonValue>& Value : NodeGuids)
	{
		FGuid Guid;
		if (!FGuid::Parse(Value->AsString(), Guid)) return false;
		UEdGraphNode* Node = FindNodeByGuid(Graph, Guid);
		if (!Node) return false;
		Removal.Add(Guid);
		Journal.RemovedNodeGuids.Add(Guid);
		FRemoveGraphJournal::FRemovedNode& Saved = Journal.RemovedNodes.AddDefaulted_GetRef();
		Saved.Guid = Guid;
		Saved.OriginalIndex = Graph->Nodes.IndexOfByKey(Node);
		Saved.OriginalNode = TStrongObjectPtr<UEdGraphNode>(Node);
		UEdGraphNode* SnapshotNode = DuplicateObject<UEdGraphNode>(Node, Journal.SnapshotGraph.Get(), Node->GetFName());
		if (!SnapshotNode) return false;
		for (UEdGraphPin* Pin : SnapshotNode->Pins)
			if (Pin) Pin->LinkedTo.Reset();
		Journal.SnapshotGraph->AddNode(SnapshotNode, false, false);
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !Removal.Contains(Node->NodeGuid)) Journal.PreservedNodeGuids.Add(Node->NodeGuid);
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || !Removal.Contains(Node->NodeGuid)) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
				if (!Other) continue;
				if (Removal.Contains(Other->NodeGuid) && Node->NodeGuid.ToString() > Other->NodeGuid.ToString()) continue;
				Journal.BoundaryLinks.Add({Node->NodeGuid, Pin->PinName, Other->NodeGuid, Linked->PinName});
			}
		}
	}
	return true;
}

bool RestoreBlueprintOwnedState(
	UBlueprint* Blueprint,
	FRemoveGraphJournal& Journal,
	UEdGraph* RestoredGraph = nullptr)
{
	Blueprint->NewVariables = Journal.NewVariables;
	Blueprint->DelegateSignatureGraphs = Journal.DelegateSignatureGraphs;
	for (TObjectPtr<UEdGraph>& Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Journal.OriginalGraph.IsValid() && Graph == Journal.OriginalGraph.Get())
			Graph = RestoredGraph;
	}
	Blueprint->ImplementedInterfaces = Journal.ImplementedInterfaces;
	for (FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		for (TObjectPtr<UEdGraph>& Graph : Interface.Graphs)
		{
			if (Journal.OriginalGraph.IsValid() && Graph == Journal.OriginalGraph.Get())
				Graph = RestoredGraph;
		}
	}
	Blueprint->LastEditedDocuments = Journal.LastEditedDocuments;
	Blueprint->Bookmarks = Journal.Bookmarks;

	Blueprint->Timelines.Reset();
	for (const FRemoveGraphJournal::FOwnedTemplate& Saved : Journal.Timelines)
	{
		UTimelineTemplate* Timeline = Cast<UTimelineTemplate>(Saved.Object.Get());
		if (!Timeline || !Saved.OriginalOuter.IsValid()) return false;
		if ((Timeline->GetOuter() != Saved.OriginalOuter.Get() || Timeline->GetFName() != Saved.OriginalName)
			&& !Timeline->Rename(*Saved.OriginalName.ToString(), Saved.OriginalOuter.Get(),
				REN_DontCreateRedirectors | REN_NonTransactional))
			return false;
		Blueprint->Timelines.Add(Timeline);
	}
	Blueprint->ComponentTemplates.Reset();
	for (const FRemoveGraphJournal::FOwnedTemplate& Saved : Journal.ComponentTemplates)
	{
		UActorComponent* Component = Cast<UActorComponent>(Saved.Object.Get());
		if (!Component || !Saved.OriginalOuter.IsValid()) return false;
		if ((Component->GetOuter() != Saved.OriginalOuter.Get() || Component->GetFName() != Saved.OriginalName)
			&& !Component->Rename(*Saved.OriginalName.ToString(), Saved.OriginalOuter.Get(),
				REN_DontCreateRedirectors | REN_NonTransactional))
			return false;
		Blueprint->ComponentTemplates.Add(Component);
	}
	return true;
}

bool RestoreJournal(UBlueprint* Blueprint, FRemoveGraphJournal& Journal)
{
	if (!Journal.GraphType.IsEmpty())
	{
		if (UEdGraph* ExistingGraph = FindGraphByGuid(Blueprint, Journal.TargetGraphGuid))
		{
			if (Journal.GraphType == TEXT("Macro")
				&& !RestoreExternalMacroInstances(Blueprint, Journal, ExistingGraph)) return false;
			return RestoreBlueprintOwnedState(Blueprint, Journal, ExistingGraph);
		}
		if (!Journal.SnapshotGraph.IsValid() || Journal.SnapshotGraph->GetOuter() != Journal.SnapshotBlueprint.Get())
			return false;
		if (Journal.OriginalGraph.IsValid() && Journal.OriginalGraph->GetOuter() == Blueprint
			&& !Journal.OriginalGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
		{
			return false;
		}
		UEdGraph* Restored = Journal.SnapshotGraph.Get();
		if (!Restored->Rename(*Journal.GraphName, Blueprint, REN_DontCreateRedirectors | REN_NonTransactional))
			return false;
		if (Journal.GraphType == TEXT("Function"))
		{
			Blueprint->FunctionGraphs.Insert(Restored, FMath::Clamp(Journal.GraphIndex, 0, Blueprint->FunctionGraphs.Num()));
		}
		else if (Journal.GraphType == TEXT("Macro"))
		{
			Blueprint->MacroGraphs.Insert(Restored, FMath::Clamp(Journal.GraphIndex, 0, Blueprint->MacroGraphs.Num()));
		}
		else
		{
			Blueprint->UbergraphPages.Insert(Restored, FMath::Clamp(Journal.GraphIndex, 1, Blueprint->UbergraphPages.Num()));
		}
		if (Journal.GraphType == TEXT("Macro") && !RestoreExternalMacroInstances(Blueprint, Journal, Restored))
			return false;
		return FindGraphByGuid(Blueprint, Journal.TargetGraphGuid) == Restored
			&& RestoreBlueprintOwnedState(Blueprint, Journal, Restored);
	}

	UEdGraph* Graph = FindGraphByGuid(Blueprint, Journal.TargetGraphGuid);
	if (!Graph || !Journal.SnapshotGraph.IsValid()) return false;
	TArray<int32> NodesByOriginalIndex;
	NodesByOriginalIndex.Reserve(Journal.RemovedNodes.Num());
	for (int32 Index = 0; Index < Journal.RemovedNodes.Num(); ++Index)
		NodesByOriginalIndex.Add(Index);
	NodesByOriginalIndex.Sort([&Journal](int32 A, int32 B)
	{
		return Journal.RemovedNodes[A].OriginalIndex < Journal.RemovedNodes[B].OriginalIndex;
	});
	for (int32 SavedIndex : NodesByOriginalIndex)
	{
		const FRemoveGraphJournal::FRemovedNode& Saved = Journal.RemovedNodes[SavedIndex];
		if (FindNodeByGuid(Graph, Saved.Guid)) continue;
		UEdGraphNode* Snapshot = FindNodeByGuid(Journal.SnapshotGraph.Get(), Saved.Guid);
		if (!Snapshot) return false;
		if (Saved.OriginalNode.IsValid() && Saved.OriginalNode->GetOuter() == Graph
			&& !Saved.OriginalNode->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
		{
			return false;
		}
		UEdGraphNode* Restored = DuplicateObject<UEdGraphNode>(Snapshot, Graph, Snapshot->GetFName());
		if (!Restored) return false;
		Graph->AddNode(Restored, false, false);
		const int32 AddedIndex = Graph->Nodes.IndexOfByKey(Restored);
		if (AddedIndex == INDEX_NONE) return false;
		Graph->Nodes.RemoveAt(AddedIndex);
		Graph->Nodes.Insert(Restored, FMath::Clamp(Saved.OriginalIndex, 0, Graph->Nodes.Num()));
	}
	for (const FRemoveGraphJournal::FBoundaryLink& Link : Journal.BoundaryLinks)
	{
		UEdGraphNode* Source = FindNodeByGuid(Graph, Link.SourceNodeGuid);
		UEdGraphNode* Target = FindNodeByGuid(Graph, Link.TargetNodeGuid);
		UEdGraphPin* SourcePin = Source ? Source->FindPin(Link.SourcePin) : nullptr;
		UEdGraphPin* TargetPin = Target ? Target->FindPin(Link.TargetPin) : nullptr;
		if (!SourcePin || !TargetPin) return false;
		if (!SourcePin->LinkedTo.Contains(TargetPin)) SourcePin->MakeLinkTo(TargetPin);
	}
	return RestoreBlueprintOwnedState(Blueprint, Journal);
}
void CollectCompilerDiagnostics(const FCompilerResultsLog& Log, TArray<FString>& OutDiagnostics)
{
	for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
	{
		const EMessageSeverity::Type Severity = Message->GetSeverity();
		if (Severity == EMessageSeverity::Error || Severity == EMessageSeverity::Warning)
		{
			OutDiagnostics.Add(Message->ToText().ToString());
		}
	}
}
void TrimRemoveGraphDiagnostics(TArray<FString>& Diagnostics)
{
	static const FString OmissionMarker = TEXT("additional compiler diagnostics omitted");
	static const FString Elision = TEXT("...");
	for (FString& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Len() + Elision.Len() > 512)
		{
			Diagnostic = Diagnostic.Left(512 - Elision.Len()) + Elision;
		}
	}
	bool bTruncated = Diagnostics.Remove(OmissionMarker) > 0;
	if (Diagnostics.Num() > 15)
	{
		Diagnostics.SetNum(15);
		bTruncated = true;
	}
	if (bTruncated)
	{
		Diagnostics.Add(OmissionMarker);
	}
}

void AttachRemoveGraphOutcome(FCortexCommandResult& Error, const FCortexBPRemoveGraphOutcome& Outcome)
{
	if (!Error.ErrorDetails.IsValid())
	{
		Error.ErrorDetails = MakeShared<FJsonObject>();
	}
	// Set phase fields after retaining any native details so callers always receive the operation outcome.
	Error.ErrorDetails->SetBoolField(TEXT("changed"), Outcome.bChanged);
	Error.ErrorDetails->SetStringField(TEXT("apply_status"), Outcome.ApplyStatus);
	Error.ErrorDetails->SetStringField(TEXT("compile_status"), Outcome.CompileStatus);
	Error.ErrorDetails->SetStringField(TEXT("readback_status"), Outcome.ReadbackStatus);
	Error.ErrorDetails->SetStringField(TEXT("rollback_status"), Outcome.RollbackStatus);
	Error.ErrorDetails->SetStringField(TEXT("save_status"), Outcome.SaveStatus);
	Error.ErrorDetails->SetStringField(TEXT("post_save_status"), Outcome.PostSaveStatus);
	Error.ErrorDetails->SetBoolField(TEXT("saved"), Outcome.bSaved);
	Error.ErrorDetails->SetBoolField(TEXT("blocked"), Outcome.bBlocked);
	Error.ErrorDetails->SetBoolField(TEXT("dirty_before"), Outcome.bDirtyBefore);
	Error.ErrorDetails->SetBoolField(TEXT("dirty_after"), Outcome.bDirtyAfter);
	Error.ErrorDetails->SetObjectField(TEXT("fingerprint_before"), Outcome.FingerprintBefore);
	Error.ErrorDetails->SetObjectField(TEXT("fingerprint_after"), Outcome.FingerprintAfter);
	Error.ErrorDetails->SetObjectField(TEXT("target"), Outcome.Target);
	Error.ErrorDetails->SetObjectField(TEXT("deletion"), Outcome.Deletion);
	Error.AddContext(TEXT("diagnostics"), Outcome.Diagnostics);
}

static bool SaveVerifiedTargetPackage(
	UBlueprint* Blueprint,
	FCortexBPRemoveGraphOutcome& Outcome,
	FCortexCommandResult& OutError)
{
	UPackage* Package = Blueprint->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;

	const bool bSaved = !ShouldInjectFault(TEXT("save"))
		&& UPackage::SavePackage(Package, Blueprint, *Filename, Args);
	if (!bSaved)
	{
		Outcome.SaveStatus = TEXT("failed");
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::SaveFailed,
			TEXT("remove_graph applied and verified in memory, but package save failed"));
		return false;
	}

	Outcome.SaveStatus = TEXT("saved");
	Outcome.bSaved = true;

	const bool bPostSaveVerified =
		!ShouldInjectFault(TEXT("post_save_verify"))
		&& IFileManager::Get().FileExists(*Filename)
		&& !Package->IsDirty();
	if (!bPostSaveVerified)
	{
		Outcome.PostSaveStatus = TEXT("failed");
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			TEXT("remove_graph package was saved but post-save verification failed; reopen/reconcile before further destructive authoring"));
		return false;
	}

	Outcome.PostSaveStatus = TEXT("verified");
	return true;
}
}

FCortexCommandResult FCortexBPRemoveGraphOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params must be an object"));
	}

	static const TSet<FString> AllowedFields = {
		TEXT("asset_path"), TEXT("name"), TEXT("cascade_exec_chain"), TEXT("dry_run"),
		TEXT("compile"), TEXT("save"), TEXT("expected_fingerprint"), TEXT("expected_validation_hash")
	};
	for (const auto& Pair : Params->Values)
	{
		const FString FieldName = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (!AllowedFields.Contains(FieldName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown remove_graph field '%s'"), *FieldName));
		}
	}

	FCortexBPRemoveGraphPrepared Prepared;
	TSharedPtr<FJsonObject> ExpectedFingerprint;
	FCortexCommandResult ParseError;
	if (!ReadRequiredStrictString(Params, TEXT("asset_path"), Prepared.AssetPath, ParseError)
		|| !ReadRequiredStrictString(Params, TEXT("name"), Prepared.Name, ParseError))
	{
		return ParseError;
	}
	if (!ReadRequiredStrictBool(Params, TEXT("dry_run"), Prepared.bDryRun, ParseError)
		|| !ReadRequiredStrictBool(Params, TEXT("compile"), Prepared.bCompile, ParseError)
		|| !ReadRequiredStrictBool(Params, TEXT("save"), Prepared.bSave, ParseError)
		|| !ReadOptionalStrictBool(Params, TEXT("cascade_exec_chain"), Prepared.bCascadeExecChain, ParseError))
	{
		return ParseError;
	}
	if (Params->HasField(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject>* ExpectedFingerprintField = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprintField)
			|| !ExpectedFingerprintField || !ExpectedFingerprintField->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		}
		ExpectedFingerprint = *ExpectedFingerprintField;
	}
	FString ExpectedValidationHash;
	if (Params->HasField(TEXT("expected_validation_hash")))
	{
		const TSharedPtr<FJsonValue> HashValue = Params->TryGetField(TEXT("expected_validation_hash"));
		if (!HashValue.IsValid() || HashValue->Type != EJson::String
			|| !Params->TryGetStringField(TEXT("expected_validation_hash"), ExpectedValidationHash))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_validation_hash must be a string"));
		}
	}
	if (Prepared.bDryRun && Prepared.bSave)
	{
		return InvalidOperation(TEXT("Preview must use save=false"));
	}
	if (!Prepared.bDryRun && Prepared.bSave && !Prepared.bCompile)
	{
		return InvalidOperation(TEXT("save=true requires compile=true"));
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(Prepared.AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}
	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(Prepared.AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}
	Prepared.AssetPath = Blueprint->GetOutermost()->GetName();
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	FCortexBPRemoveGraphOutcome Outcome;
	Outcome.bDirtyBefore = bDirtyBefore;
	Outcome.bDirtyAfter = bDirtyBefore;
	Outcome.FingerprintBefore = FCortexGraphFingerprint::Compute(Blueprint);
	Outcome.FingerprintAfter = Outcome.FingerprintBefore;
	auto ReturnErrorWithOutcome = [&Blueprint, &Outcome](FCortexCommandResult Error)
	{
		Outcome.FingerprintAfter = FCortexGraphFingerprint::Compute(Blueprint);
		Outcome.bDirtyAfter = Blueprint->GetOutermost()->IsDirty();
		TrimRemoveGraphDiagnostics(Outcome.Diagnostics);
		AttachRemoveGraphOutcome(Error, Outcome);
		return Error;
	};
	if (Prepared.bSave && bDirtyBefore)
	{
		return ReturnErrorWithOutcome(FCortexCommandRouter::Error(
			CortexErrorCodes::DirtyEditorState, TEXT("save=true requires a clean starting package")));
	}

	TArray<TSharedPtr<FJsonValue>> RemovedNodes;
	TSharedPtr<FJsonObject> Removed;
	FCortexCommandResult PlanError;
	if (!BuildPlan(Blueprint, Prepared, RemovedNodes, Removed, PlanError))
	{
		return ReturnErrorWithOutcome(PlanError);
	}
	Outcome.Target = Prepared.Target;
	Outcome.Deletion = Prepared.Deletion;
	Prepared.FingerprintBefore = FCortexGraphFingerprint::Compute(Blueprint);
	if (!Prepared.FingerprintBefore.IsValid())
	{
		return ReturnErrorWithOutcome(FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation, TEXT("Could not compute Blueprint graph fingerprint")));
	}
	Outcome.FingerprintBefore = Prepared.FingerprintBefore;
	Outcome.FingerprintAfter = Prepared.FingerprintBefore;
	BuildValidationHash(Prepared);

	if (!Prepared.bDryRun)
	{
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
		{
			return ReturnErrorWithOutcome(FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason)));
		}
		const TSharedPtr<FJsonObject> CurrentFingerprint = FCortexGraphFingerprint::Compute(Blueprint);
		FCortexCommandResult FingerprintError;
		if (!ExpectedFingerprint.IsValid() || ExpectedValidationHash.IsEmpty())
		{
			return ReturnErrorWithOutcome(FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
				TEXT("Apply requires expected_fingerprint and expected_validation_hash from preview")));
		}
		if (!FCortexGraphFingerprint::ValidatePrecondition(ExpectedFingerprint, CurrentFingerprint, FingerprintError))
		{
			return ReturnErrorWithOutcome(FingerprintError);
		}
		if (ExpectedValidationHash != Prepared.ValidationHash)
		{
			return ReturnErrorWithOutcome(FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
				TEXT("expected_validation_hash does not match the current remove_graph plan")));
		}

		FRemoveGraphJournal Journal;
		if (!CaptureJournal(Blueprint, Prepared, Journal))
		{
			return ReturnErrorWithOutcome(FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("Could not capture exact remove_graph recovery journal")));
		}
		TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
			FText::FromString(TEXT("Cortex: Remove Blueprint Graph")));
		Blueprint->Modify();
		UEdGraph* TargetGraph = FindGraphByGuid(Blueprint, Journal.TargetGraphGuid);
		const FString TargetKind = Prepared.Target->GetStringField(TEXT("kind"));
		bool bMutationSucceeded = TargetGraph != nullptr;
		if (bMutationSucceeded && TargetKind == TEXT("graph"))
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, TargetGraph, EGraphRemoveFlags::MarkTransient);
		}
		else if (bMutationSucceeded)
		{
			const TArray<TSharedPtr<FJsonValue>>& GuidValues = Prepared.Deletion->GetArrayField(TEXT("node_guids"));
			for (const TSharedPtr<FJsonValue>& Value : GuidValues)
			{
				FGuid Guid;
				FGuid::Parse(Value->AsString(), Guid);
				UEdGraphNode* Node = FindNodeByGuid(TargetGraph, Guid);
				if (!Node)
				{
					bMutationSucceeded = false;
					break;
				}
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
			}
		}
		Outcome.ApplyStatus = bMutationSucceeded ? TEXT("applied") : TEXT("failed");
		Outcome.bChanged = bMutationSucceeded;
		bool bFailed = !bMutationSucceeded;
		FString FailurePhase = bMutationSucceeded ? TEXT("") : TEXT("mutation");
#if WITH_AUTOMATION_TESTS
		if (!bFailed && RemoveGraphFaultPoint == TEXT("after_mutation"))
		{
			bFailed = true;
			FailurePhase = TEXT("mutation");
		}
#endif
		if (!bFailed && Prepared.bCompile)
		{
			FCompilerResultsLog Log;
			Log.bAnnotateMentionedNodes = false;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
			Outcome.CompileStatus = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings)
				? TEXT("compiled") : TEXT("failed");
#if WITH_AUTOMATION_TESTS
			if (RemoveGraphFaultPoint == TEXT("compile")) Outcome.CompileStatus = TEXT("failed");
#endif
			if (Outcome.CompileStatus != TEXT("compiled"))
			{
				bFailed = true;
				FailurePhase = TEXT("compile");
			}
		}
		if (!bFailed)
		{
			bool bMatched = true;
			if (TargetKind == TEXT("graph"))
			{
				bMatched = FindGraphByGuid(Blueprint, Journal.TargetGraphGuid) == nullptr;
				for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
					if (Candidate && Candidate->GraphGuid == Journal.TargetGraphGuid) bMatched = false;
				for (UEdGraph* Candidate : Blueprint->MacroGraphs)
					if (Candidate && Candidate->GraphGuid == Journal.TargetGraphGuid) bMatched = false;
				for (int32 Index = 0; Index < Blueprint->UbergraphPages.Num(); ++Index)
					if (Blueprint->UbergraphPages[Index] && Blueprint->UbergraphPages[Index]->GraphGuid == Journal.TargetGraphGuid) bMatched = false;
				for (FGuid Guid : Journal.PreservedGraphGuids)
					bMatched &= FindGraphByGuid(Blueprint, Guid) != nullptr;
				for (const FRemoveGraphJournal::FMacroInstance& Instance : Journal.ExternalMacroInstances)
				{
					UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Instance.HostGraphGuid);
					bMatched &= !FindNodeByGuid(HostGraph, Instance.NodeGuid);
				}
			}
			else
			{
				UEdGraph* Graph = FindGraphByGuid(Blueprint, Journal.TargetGraphGuid);
				for (FGuid Guid : Journal.RemovedNodeGuids) bMatched &= FindNodeByGuid(Graph, Guid) == nullptr;
				for (FGuid Guid : Journal.PreservedNodeGuids) bMatched &= FindNodeByGuid(Graph, Guid) != nullptr;
				for (const FRemoveGraphJournal::FBoundaryLink& Link : Journal.BoundaryLinks)
				{
					if (!Journal.RemovedNodeGuids.Contains(Link.TargetNodeGuid))
					{
						UEdGraphNode* Preserved = FindNodeByGuid(Graph, Link.TargetNodeGuid);
						bMatched &= Preserved && Preserved->FindPin(Link.TargetPin);
					}
				}
			}
			if (Prepared.bCompile)
				bMatched &= Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
#if WITH_AUTOMATION_TESTS
			if (RemoveGraphFaultPoint == TEXT("readback") || RemoveGraphFaultPoint == TEXT("rollback_verify")) bMatched = false;
#endif
			Outcome.ReadbackStatus = bMatched ? TEXT("matched") : TEXT("mismatched");
			if (!bMatched)
			{
				bFailed = true;
				FailurePhase = TEXT("readback");
			}
		}
		if (bFailed)
		{
			Transaction->Cancel();
			bool bRestored = RestoreJournal(Blueprint, Journal);
			bool bRecoveryCompile = true;
			if (bRestored && Prepared.bCompile && FailurePhase != TEXT("mutation"))
			{
				FCompilerResultsLog RecoveryLog;
				RecoveryLog.bAnnotateMentionedNodes = false;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &RecoveryLog);
				CollectCompilerDiagnostics(RecoveryLog, Outcome.Diagnostics);
				bRecoveryCompile = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
			}
			const TSharedPtr<FJsonObject> RestoredFingerprint = FCortexGraphFingerprint::Compute(Blueprint);
			const bool bAuthoringMatches = RestoredFingerprint.IsValid()
				&& RestoredFingerprint->GetStringField(TEXT("graph_authoring_hash")) == Journal.GraphFingerprintBefore;
			const bool bGeneratedMatches = !Prepared.bCompile || (bRecoveryCompile
				&& FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint) == Journal.GeneratedStateBefore);
			bool bVerified = bRestored && bAuthoringMatches && bGeneratedMatches;
#if WITH_AUTOMATION_TESTS
			if (RemoveGraphFaultPoint == TEXT("rollback_verify")) bVerified = false;
#endif
			if (bVerified)
			{
				Blueprint->GetOutermost()->SetDirtyFlag(Journal.bPackageWasDirty);
				Blueprint->Status = Journal.StatusBefore;
				Outcome.RollbackStatus = TEXT("restored");
			}
			else
			{
				Blueprint->GetOutermost()->SetDirtyFlag(true);
				FCortexAssetMutationGuard::Block(Blueprint, TEXT("remove_graph rollback verification failed"));
				Outcome.RollbackStatus = TEXT("unverified");
				Outcome.bBlocked = true;
			}
			Outcome.bChanged = !bVerified;
			Outcome.CompileStatus = FailurePhase == TEXT("compile") ? TEXT("failed") : Outcome.CompileStatus;
			Outcome.FingerprintAfter = FCortexGraphFingerprint::Compute(Blueprint);
			Outcome.bDirtyAfter = Blueprint->GetOutermost()->IsDirty();
			TrimRemoveGraphDiagnostics(Outcome.Diagnostics);
			FCortexCommandResult Error = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("remove_graph %s failed"), *FailurePhase));
			AttachRemoveGraphOutcome(Error, Outcome);
			Error.ErrorDetails->SetBoolField(TEXT("rollback_content_restored"), bRestored);
			Error.ErrorDetails->SetBoolField(TEXT("rollback_authoring_matches"), bAuthoringMatches);
			Error.ErrorDetails->SetBoolField(TEXT("rollback_generated_matches"), bGeneratedMatches);
			return Error;
		}

		if (Prepared.bSave)
		{
			FCortexCommandResult SaveError;
			if (!SaveVerifiedTargetPackage(Blueprint, Outcome, SaveError))
			{
				Outcome.FingerprintAfter = FCortexGraphFingerprint::Compute(Blueprint);
				Outcome.bDirtyAfter = Blueprint->GetOutermost()->IsDirty();
				TrimRemoveGraphDiagnostics(Outcome.Diagnostics);
				AttachRemoveGraphOutcome(SaveError, Outcome);
				return SaveError;
			}
		}

		Outcome.FingerprintAfter = FCortexGraphFingerprint::Compute(Blueprint);
		Outcome.bDirtyAfter = Blueprint->GetOutermost()->IsDirty();
		TrimRemoveGraphDiagnostics(Outcome.Diagnostics);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Prepared.AssetPath);
	Data->SetObjectField(TEXT("removed"), Removed);
	Data->SetArrayField(TEXT("removed_nodes"), RemovedNodes);
	Data->SetBoolField(TEXT("dry_run"), Prepared.bDryRun);
	Data->SetBoolField(TEXT("compiled"), Prepared.bCompile && !Prepared.bDryRun);
	Data->SetStringField(TEXT("apply_status"), Outcome.ApplyStatus);
	Data->SetBoolField(TEXT("changed"), Outcome.bChanged);
	Data->SetStringField(TEXT("compile_status"), Outcome.CompileStatus);
	Data->SetStringField(TEXT("readback_status"), Outcome.ReadbackStatus);
	Data->SetStringField(TEXT("rollback_status"), Outcome.RollbackStatus);
	Data->SetStringField(TEXT("save_status"), Outcome.SaveStatus);
	Data->SetStringField(TEXT("post_save_status"), Outcome.PostSaveStatus);
	Data->SetBoolField(TEXT("saved"), Outcome.bSaved);
	Data->SetBoolField(TEXT("blocked"), Outcome.bBlocked);
	Data->SetBoolField(TEXT("dirty_before"), Outcome.bDirtyBefore);
	Data->SetBoolField(TEXT("dirty_after"), Outcome.bDirtyAfter);
	Data->SetObjectField(TEXT("fingerprint_before"), Outcome.FingerprintBefore);
	Data->SetObjectField(TEXT("fingerprint_after"), Outcome.FingerprintAfter);
	Data->SetObjectField(TEXT("target"), Outcome.Target);
	Data->SetObjectField(TEXT("deletion"), Outcome.Deletion);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FString& Diagnostic : Outcome.Diagnostics)
	{
		Diagnostics.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	Data->SetStringField(TEXT("validation_hash"), Prepared.ValidationHash);
	return FCortexCommandRouter::Success(Data);
}

#if WITH_AUTOMATION_TESTS
void FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName Point)
{
	RemoveGraphFaultPoint = Point;
}

void FCortexBPRemoveGraphOps::ClearFaultPointForTesting()
{
	RemoveGraphFaultPoint = NAME_None;
}
#endif
