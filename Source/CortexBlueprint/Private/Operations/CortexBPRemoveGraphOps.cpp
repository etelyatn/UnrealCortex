#include "Operations/CortexBPRemoveGraphOps.h"

#include "CortexGraphFingerprint.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
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
	const FString& Name,
	bool bCascadeExecChain,
	FCortexBPRemoveGraphItem& Item,
	FCortexCommandResult& OutError)
{
	const FString ResolvedName = Name == TEXT("ConstructionScript")
		? TEXT("UserConstructionScript") : Name;

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
		Item.Target = MakeShared<FJsonObject>();
		Item.Target->SetStringField(TEXT("kind"), TEXT("graph"));
		Item.Target->SetStringField(TEXT("graph_guid"), FoundGraph->GraphGuid.ToString());
		Item.Target->SetStringField(TEXT("graph_type"), FoundType);
		Item.Target->SetStringField(TEXT("name"), FoundGraph->GetName());

		Item.Deletion = MakeShared<FJsonObject>();
		Item.Deletion->SetStringField(TEXT("kind"), TEXT("graph"));
		Item.Deletion->SetStringField(TEXT("graph_guid"), FoundGraph->GraphGuid.ToString());
		Item.Deletion->SetNumberField(TEXT("node_count"), FoundGraph->Nodes.Num());

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
			AddNodeInventory(Node, Item.RemovedNodes);
		}
		Item.Removed = MakeShared<FJsonObject>();
		Item.Removed->SetStringField(TEXT("name"), Name);
		Item.Removed->SetStringField(TEXT("type"), FoundType);
		Item.Removed->SetNumberField(TEXT("node_count"), FoundGraph->Nodes.Num());
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
		if (bCascadeExecChain)
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

		Item.Target = MakeShared<FJsonObject>();
		Item.Target->SetStringField(TEXT("kind"), TEXT("custom_event"));
		Item.Target->SetStringField(TEXT("graph_guid"), OwningGraph->GraphGuid.ToString());
		Item.Target->SetStringField(TEXT("node_guid"), FoundEvent->NodeGuid.ToString());
		Item.Target->SetStringField(TEXT("name"), FoundEvent->CustomFunctionName.ToString());

		Item.Deletion = MakeShared<FJsonObject>();
		Item.Deletion->SetStringField(TEXT("kind"), TEXT("nodes"));
		TArray<TSharedPtr<FJsonValue>> NodeGuids;
		for (UEdGraphNode* Node : SortedNodes)
		{
			NodeGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
			AddNodeInventory(Node, Item.RemovedNodes);
		}
		Item.Deletion->SetArrayField(TEXT("node_guids"), NodeGuids);
		Item.Removed = MakeShared<FJsonObject>();
		Item.Removed->SetStringField(TEXT("name"), Name);
		Item.Removed->SetStringField(TEXT("type"), TEXT("CustomEvent"));
		Item.Removed->SetNumberField(TEXT("node_count"), RemovalSet.Num());
		Item.Removed->SetBoolField(TEXT("cascade_exec_chain"), bCascadeExecChain);
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
			*Name, *Blueprint->GetName(), *AvailableHint));
	return false;
}

void BuildValidationHash(FCortexBPRemoveGraphPrepared& Prepared)
{
	TSharedPtr<FJsonObject> Intent = MakeShared<FJsonObject>();
	Intent->SetStringField(TEXT("asset_path"), Prepared.AssetPath);
	Intent->SetBoolField(TEXT("cascade_exec_chain"), Prepared.bCascadeExecChain);
	Intent->SetBoolField(TEXT("compile"), Prepared.bCompile);
	Intent->SetObjectField(TEXT("expected_fingerprint"), Prepared.FingerprintBefore);
	if (Prepared.bBatch)
	{
		// One hash over the whole set: names in request order plus every resolved target and deletion.
		TArray<TSharedPtr<FJsonValue>> Names;
		TArray<TSharedPtr<FJsonValue>> Targets;
		TArray<TSharedPtr<FJsonValue>> Deletions;
		for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
		{
			Names.Add(MakeShared<FJsonValueString>(Item.Name));
			Targets.Add(MakeShared<FJsonValueObject>(Item.Target));
			Deletions.Add(MakeShared<FJsonValueObject>(Item.Deletion));
		}
		Intent->SetArrayField(TEXT("names"), Names);
		Intent->SetArrayField(TEXT("targets"), Targets);
		Intent->SetArrayField(TEXT("deletions"), Deletions);
	}
	else
	{
		const FCortexBPRemoveGraphItem& Item = Prepared.Items[0];
		Intent->SetStringField(TEXT("name"), Item.Name);
		Intent->SetObjectField(TEXT("target"), Item.Target);
		Intent->SetObjectField(TEXT("deletion"), Item.Deletion);
	}
	const FString Source = FString(Prepared.bBatch ? TEXT("blueprint_remove_graph_set_v1|") : TEXT("blueprint_remove_graph_v1|"))
		+ CanonicalObject(Intent);
	FTCHARToUTF8 Utf8(*Source);
	Prepared.ValidationHash = LexToString(FIoHash::HashBuffer(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

struct FRemoveBoundaryLink
{
	FGuid SourceNodeGuid;
	FName SourcePin;
	FGuid TargetNodeGuid;
	FName TargetPin;
};
struct FRemovedGraphNode
{
	FGuid Guid;
	int32 OriginalIndex = INDEX_NONE;
	TStrongObjectPtr<UEdGraphNode> OriginalNode;
};
struct FRemoveMacroInstance
{
	FGuid HostGraphGuid;
	FGuid NodeGuid;
	FName OriginalNodeName;
	int32 OriginalIndex = INDEX_NONE;
	TStrongObjectPtr<UEdGraphNode> Snapshot;
	TStrongObjectPtr<UEdGraphNode> OriginalNode;
};
struct FRemoveMacroLink
{
	FGuid HostGraphGuid;
	FGuid SourceNodeGuid;
	FName SourcePin;
	FGuid TargetNodeGuid;
	FName TargetPin;
};

// Recovery data for one removal target: a whole graph, or the node set of one custom event.
struct FRemoveTargetJournal
{
	bool bWholeGraph = false;
	FGuid TargetGraphGuid;
	FString GraphType;
	FString GraphName;
	TStrongObjectPtr<UEdGraph> OriginalGraph;
	int32 GraphIndex = INDEX_NONE;
	TArray<FGuid> RemovedNodeGuids;
	TArray<FRemovedGraphNode> RemovedNodes;
	TArray<FGuid> PreservedNodeGuids;
	TStrongObjectPtr<UEdGraph> SnapshotGraph;
	TArray<FRemoveBoundaryLink> BoundaryLinks;
	TArray<FRemoveMacroInstance> ExternalMacroInstances;
	TArray<FRemoveMacroLink> ExternalMacroLinks;
};

// Recovery data for the whole request: Blueprint-owned state plus one journal per target.
struct FRemoveGraphJournal
{
	bool bPackageWasDirty = false;
	EBlueprintStatus StatusBefore = BS_Unknown;
	FString GraphFingerprintBefore;
	FString GeneratedStateBefore;
	TStrongObjectPtr<UBlueprint> SnapshotBlueprint;
	TArray<FGuid> PreservedGraphGuids;
	TSet<FGuid> RemovedNodeGuidSet;
	/** Macro instances removed because their macro is removed; they leave their host graph with the macro. */
	TSet<FGuid> RemovedMacroInstanceGuids;
	TArray<FRemoveTargetJournal> Targets;
	struct FCompositeBoundGraph
	{
		FGuid HostGraphGuid;
		FGuid CompositeNodeGuid;
		TStrongObjectPtr<UEdGraph> BoundGraph;
		FName OriginalName;
		int32 OriginalSubGraphIndex = INDEX_NONE;
		int32 HostDepth = 0;
		EObjectFlags OriginalPersistentFlags = RF_NoFlags;
		bool bWasRooted = false;
	};
	TArray<FCompositeBoundGraph> CompositeBoundGraphs;
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

bool CaptureCompositeBoundGraph(
	UEdGraph* HostGraph,
	UK2Node_Composite* Composite,
	FRemoveGraphJournal& Journal,
	TSet<UEdGraph*>& CapturedGraphs,
	int32 HostDepth)
{
	if (!HostGraph || !Composite || !Composite->BoundGraph) return false;
	UEdGraph* BoundGraph = Composite->BoundGraph;
	if (CapturedGraphs.Contains(BoundGraph)) return true;
	if (!HostGraph->GraphGuid.IsValid() || !Composite->NodeGuid.IsValid()) return false;
	const int32 SubGraphIndex = HostGraph->SubGraphs.IndexOfByKey(BoundGraph);
	if (SubGraphIndex == INDEX_NONE) return false;
	CapturedGraphs.Add(BoundGraph);
	FRemoveGraphJournal::FCompositeBoundGraph& Saved =
		Journal.CompositeBoundGraphs.AddDefaulted_GetRef();
	Saved.HostGraphGuid = HostGraph->GraphGuid;
	Saved.CompositeNodeGuid = Composite->NodeGuid;
	Saved.BoundGraph = TStrongObjectPtr<UEdGraph>(BoundGraph);
	Saved.OriginalName = BoundGraph->GetFName();
	Saved.HostDepth = HostDepth;
	Saved.OriginalPersistentFlags = BoundGraph->GetFlags() & (RF_Public | RF_Standalone | RF_Transient);
	Saved.bWasRooted = BoundGraph->IsRooted();
	Saved.OriginalSubGraphIndex = SubGraphIndex;
	for (UEdGraphNode* ChildNode : BoundGraph->Nodes)
	{
		if (UK2Node_Composite* ChildComposite = Cast<UK2Node_Composite>(ChildNode))
		{
			if (!CaptureCompositeBoundGraph(
				BoundGraph, ChildComposite, Journal, CapturedGraphs, HostDepth + 1))
				return false;
		}
	}
	return true;
}

bool CaptureCompositeBoundGraphsIn(UEdGraph* HostGraph, FRemoveGraphJournal& Journal)
{
	if (!HostGraph) return false;
	TSet<UEdGraph*> CapturedGraphs;
	for (UEdGraphNode* Node : HostGraph->Nodes)
	{
		if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			if (!CaptureCompositeBoundGraph(HostGraph, Composite, Journal, CapturedGraphs, 0))
				return false;
		}
	}
	return true;
}

bool CaptureCompositeBoundGraphsForNodes(
	UEdGraph* HostGraph,
	const TArray<FRemovedGraphNode>& Nodes,
	FRemoveGraphJournal& Journal)
{
	TSet<UEdGraph*> CapturedGraphs;
	for (const FRemovedGraphNode& Saved : Nodes)
	{
		if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Saved.OriginalNode.Get()))
		{
			if (!CaptureCompositeBoundGraph(HostGraph, Composite, Journal, CapturedGraphs, 0))
				return false;
		}
	}
	return true;
}
bool RestoreCompositeBoundGraphs(UBlueprint* Blueprint, FRemoveGraphJournal& Journal)
{
	Journal.CompositeBoundGraphs.Sort(
		[](const FRemoveGraphJournal::FCompositeBoundGraph& A,
			const FRemoveGraphJournal::FCompositeBoundGraph& B)
		{
			if (A.HostDepth != B.HostDepth) return A.HostDepth < B.HostDepth;
			return A.OriginalSubGraphIndex < B.OriginalSubGraphIndex;
		});
	for (const FRemoveGraphJournal::FCompositeBoundGraph& Saved : Journal.CompositeBoundGraphs)
	{
		UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Saved.HostGraphGuid);
		UK2Node_Composite* Composite = Cast<UK2Node_Composite>(
			FindNodeByGuid(HostGraph, Saved.CompositeNodeGuid));
		UEdGraph* BoundGraph = Saved.BoundGraph.Get();
		if (!HostGraph || !Composite || !BoundGraph) return false;
		if (Composite->BoundGraph && Composite->BoundGraph != BoundGraph)
		{
			UEdGraph* DuplicateGraph = Composite->BoundGraph;
			HostGraph->SubGraphs.Remove(DuplicateGraph);
			Composite->BoundGraph = nullptr;
			if (DuplicateGraph->GetOuter() == Composite
				&& !DuplicateGraph->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional))
				return false;
		}
		if ((BoundGraph->GetOuter() != Composite || BoundGraph->GetFName() != Saved.OriginalName)
			&& !BoundGraph->Rename(*Saved.OriginalName.ToString(), Composite,
				REN_DontCreateRedirectors | REN_NonTransactional))
			return false;
		Composite->BoundGraph = BoundGraph;
		BoundGraph->ClearFlags(RF_Public | RF_Standalone | RF_Transient);
		BoundGraph->SetFlags(Saved.OriginalPersistentFlags);
		if (Saved.bWasRooted)
		{
			BoundGraph->AddToRoot();
		}
		else
		{
			BoundGraph->RemoveFromRoot();
		}
		Composite->InputSinkNode = nullptr;
		Composite->OutputSourceNode = nullptr;
		for (UEdGraphNode* Node : BoundGraph->Nodes)
		{
			UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
			if (!Tunnel) continue;
			if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs)
			{
				Tunnel->OutputSourceNode = Composite;
				Composite->InputSinkNode = Tunnel;
			}
			else if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs)
			{
				Tunnel->InputSinkNode = Composite;
				Composite->OutputSourceNode = Tunnel;
			}
		}
		HostGraph->SubGraphs.Remove(BoundGraph);
		HostGraph->SubGraphs.Insert(
			BoundGraph, FMath::Clamp(Saved.OriginalSubGraphIndex, 0, HostGraph->SubGraphs.Num()));
		if ((BoundGraph->GetFlags() & (RF_Public | RF_Standalone | RF_Transient))
				!= Saved.OriginalPersistentFlags
			|| BoundGraph->IsRooted() != Saved.bWasRooted)
			return false;
	}
	for (const FRemoveGraphJournal::FCompositeBoundGraph& Saved : Journal.CompositeBoundGraphs)
	{
		UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Saved.HostGraphGuid);
		UEdGraph* BoundGraph = Saved.BoundGraph.Get();
		if (!HostGraph || !BoundGraph
			|| HostGraph->SubGraphs.IndexOfByKey(BoundGraph) != Saved.OriginalSubGraphIndex)
			return false;
	}
	return true;
}

bool CaptureExternalMacroInstances(
	UBlueprint* Blueprint,
	UEdGraph* Target,
	FRemoveGraphJournal& Journal,
	FRemoveTargetJournal& TargetJournal)
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
			FRemoveMacroInstance& Saved = TargetJournal.ExternalMacroInstances.AddDefaulted_GetRef();
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
					TargetJournal.ExternalMacroLinks.Add({
						HostGraph->GraphGuid, Node->NodeGuid, Pin->PinName,
						Other->NodeGuid, Linked->PinName});
				}
			}
		}
	}
	return true;
}

bool RestoreMacroInstance(UBlueprint* Blueprint, const FRemoveMacroInstance& Saved, UEdGraph* RestoredMacro)
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
	return true;
}

bool RestoreMacroInstanceLinks(UBlueprint* Blueprint, const FRemoveTargetJournal& TargetJournal)
{
	for (const FRemoveMacroLink& Link : TargetJournal.ExternalMacroLinks)
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

bool CaptureTargetJournal(
	UBlueprint* Blueprint,
	const FCortexBPRemoveGraphItem& Item,
	FRemoveGraphJournal& Journal,
	FRemoveTargetJournal& TargetJournal)
{
	FGuid GraphGuid;
	if (!FGuid::Parse(Item.Target->GetStringField(TEXT("graph_guid")), GraphGuid)) return false;
	TargetJournal.TargetGraphGuid = GraphGuid;
	if (Item.Target->GetStringField(TEXT("kind")) == TEXT("graph"))
	{
		TargetJournal.bWholeGraph = true;
		UEdGraph* Target = FindGraphByGuid(Blueprint, GraphGuid, &TargetJournal.GraphType, &TargetJournal.GraphIndex);
		if (!Target || TargetJournal.GraphType.IsEmpty()) return false;
		if (!CaptureCompositeBoundGraphsIn(Target, Journal)) return false;
		TargetJournal.OriginalGraph = TStrongObjectPtr<UEdGraph>(Target);
		TargetJournal.GraphName = Target->GetName();
		TargetJournal.SnapshotGraph = TStrongObjectPtr<UEdGraph>(
			DuplicateObject<UEdGraph>(Target, Journal.SnapshotBlueprint.Get(), Target->GetFName()));
		if (!TargetJournal.SnapshotGraph.IsValid()) return false;
		return TargetJournal.GraphType != TEXT("Macro")
			|| CaptureExternalMacroInstances(Blueprint, Target, Journal, TargetJournal);
	}

	UEdGraph* Graph = FindGraphByGuid(Blueprint, GraphGuid);
	if (!Graph) return false;
	TargetJournal.SnapshotGraph = TStrongObjectPtr<UEdGraph>(
		NewObject<UEdGraph>(Journal.SnapshotBlueprint.Get(), NAME_None, RF_Transient));
	TSet<FGuid> Removal;
	const TArray<TSharedPtr<FJsonValue>>& NodeGuids = Item.Deletion->GetArrayField(TEXT("node_guids"));
	for (const TSharedPtr<FJsonValue>& Value : NodeGuids)
	{
		FGuid Guid;
		if (!FGuid::Parse(Value->AsString(), Guid)) return false;
		UEdGraphNode* Node = FindNodeByGuid(Graph, Guid);
		if (!Node) return false;
		Removal.Add(Guid);
		TargetJournal.RemovedNodeGuids.Add(Guid);
		FRemovedGraphNode& Saved = TargetJournal.RemovedNodes.AddDefaulted_GetRef();
		Saved.Guid = Guid;
		Saved.OriginalIndex = Graph->Nodes.IndexOfByKey(Node);
		Saved.OriginalNode = TStrongObjectPtr<UEdGraphNode>(Node);
		UEdGraphNode* SnapshotNode = DuplicateObject<UEdGraphNode>(Node, TargetJournal.SnapshotGraph.Get(), Node->GetFName());
		if (!SnapshotNode) return false;
		for (UEdGraphPin* Pin : SnapshotNode->Pins)
			if (Pin) Pin->LinkedTo.Reset();
		TargetJournal.SnapshotGraph->AddNode(SnapshotNode, false, false);
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		// Nodes removed by another target of the same request are not "preserved".
		if (Node && !Journal.RemovedNodeGuidSet.Contains(Node->NodeGuid)) TargetJournal.PreservedNodeGuids.Add(Node->NodeGuid);
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
				TargetJournal.BoundaryLinks.Add({Node->NodeGuid, Pin->PinName, Other->NodeGuid, Linked->PinName});
			}
		}
	}
	return CaptureCompositeBoundGraphsForNodes(Graph, TargetJournal.RemovedNodes, Journal);
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

	TSet<FGuid> RemovedGraphGuids;
	for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
	{
		FGuid GraphGuid;
		if (!FGuid::Parse(Item.Target->GetStringField(TEXT("graph_guid")), GraphGuid)) return false;
		if (Item.Target->GetStringField(TEXT("kind")) == TEXT("graph"))
		{
			RemovedGraphGuids.Add(GraphGuid);
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Value : Item.Deletion->GetArrayField(TEXT("node_guids")))
		{
			FGuid NodeGuid;
			if (!FGuid::Parse(Value->AsString(), NodeGuid)) return false;
			Journal.RemovedNodeGuidSet.Add(NodeGuid);
		}
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		if (Graph && !RemovedGraphGuids.Contains(Graph->GraphGuid)) Journal.PreservedGraphGuids.Add(Graph->GraphGuid);
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
		if (Graph && !RemovedGraphGuids.Contains(Graph->GraphGuid)) Journal.PreservedGraphGuids.Add(Graph->GraphGuid);
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
		if (Graph && !RemovedGraphGuids.Contains(Graph->GraphGuid)) Journal.PreservedGraphGuids.Add(Graph->GraphGuid);

	Journal.Targets.Reserve(Prepared.Items.Num());
	for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
	{
		if (!CaptureTargetJournal(Blueprint, Item, Journal, Journal.Targets.AddDefaulted_GetRef())) return false;
	}
	// Macro instances of a removed macro are removed too. A custom-event target in the same host graph must not
	// expect them to survive (preserved nodes, boundary links), and its own node loop must tolerate them gone.
	for (const FRemoveTargetJournal& Target : Journal.Targets)
	{
		for (const FRemoveMacroInstance& Instance : Target.ExternalMacroInstances)
		{
			Journal.RemovedMacroInstanceGuids.Add(Instance.NodeGuid);
			Journal.RemovedNodeGuidSet.Add(Instance.NodeGuid);
		}
	}
	for (FRemoveTargetJournal& Target : Journal.Targets)
	{
		Target.PreservedNodeGuids.RemoveAll([&Journal](const FGuid& Guid)
		{
			return Journal.RemovedMacroInstanceGuids.Contains(Guid);
		});
	}
	return true;
}

bool RestoreBlueprintOwnedState(
	UBlueprint* Blueprint,
	FRemoveGraphJournal& Journal,
	const TMap<UEdGraph*, UEdGraph*>& RestoredGraphs)
{
	if (!RestoreCompositeBoundGraphs(Blueprint, Journal)) return false;
	// A removed graph that owned a delegate signature or interface implementation comes back as a new
	// object: point those collections at the restored graph.
	const auto RemapRestoredGraph = [&RestoredGraphs](TObjectPtr<UEdGraph>& Graph)
	{
		if (UEdGraph* const* Restored = RestoredGraphs.Find(Graph.Get())) Graph = *Restored;
	};
	Blueprint->NewVariables = Journal.NewVariables;
	Blueprint->DelegateSignatureGraphs = Journal.DelegateSignatureGraphs;
	for (TObjectPtr<UEdGraph>& Graph : Blueprint->DelegateSignatureGraphs)
	{
		RemapRestoredGraph(Graph);
	}
	Blueprint->ImplementedInterfaces = Journal.ImplementedInterfaces;
	for (FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		for (TObjectPtr<UEdGraph>& Graph : Interface.Graphs)
		{
			RemapRestoredGraph(Graph);
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
		Timeline->ClearGarbage();
		if (!IsValid(Timeline)) return false;
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

// Restores every target. Whole graphs return first (ascending original index so each collection ends up in
// its original order), then external macro instances, then removed custom-event nodes and their links, and
// finally the Blueprint-owned state. Any failed step reports false so the caller blocks the asset.
bool RestoreJournal(UBlueprint* Blueprint, FRemoveGraphJournal& Journal)
{
	TArray<UEdGraph*> RestoredByTarget;
	RestoredByTarget.Init(nullptr, Journal.Targets.Num());
	TMap<UEdGraph*, UEdGraph*> RestoredGraphs;

	TArray<int32> GraphOrder;
	for (int32 Index = 0; Index < Journal.Targets.Num(); ++Index)
	{
		if (Journal.Targets[Index].bWholeGraph) GraphOrder.Add(Index);
	}
	GraphOrder.Sort([&Journal](int32 A, int32 B)
	{
		return Journal.Targets[A].GraphIndex < Journal.Targets[B].GraphIndex;
	});
	for (const int32 Index : GraphOrder)
	{
		FRemoveTargetJournal& Target = Journal.Targets[Index];
		UEdGraph* Restored = FindGraphByGuid(Blueprint, Target.TargetGraphGuid);
		if (!Restored)
		{
			if (!Target.SnapshotGraph.IsValid() || Target.SnapshotGraph->GetOuter() != Journal.SnapshotBlueprint.Get())
				return false;
			if (Target.OriginalGraph.IsValid() && Target.OriginalGraph->GetOuter() == Blueprint
				&& !Target.OriginalGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
			{
				return false;
			}
			Restored = Target.SnapshotGraph.Get();
			if (!Restored->Rename(*Target.GraphName, Blueprint, REN_DontCreateRedirectors | REN_NonTransactional))
				return false;
			if (Target.GraphType == TEXT("Function"))
			{
				Blueprint->FunctionGraphs.Insert(Restored, FMath::Clamp(Target.GraphIndex, 0, Blueprint->FunctionGraphs.Num()));
			}
			else if (Target.GraphType == TEXT("Macro"))
			{
				Blueprint->MacroGraphs.Insert(Restored, FMath::Clamp(Target.GraphIndex, 0, Blueprint->MacroGraphs.Num()));
			}
			else
			{
				Blueprint->UbergraphPages.Insert(Restored, FMath::Clamp(Target.GraphIndex, 1, Blueprint->UbergraphPages.Num()));
			}
		}
		RestoredByTarget[Index] = Restored;
		if (Target.OriginalGraph.IsValid()) RestoredGraphs.Add(Target.OriginalGraph.Get(), Restored);
	}
	for (const int32 Index : GraphOrder)
	{
		if (FindGraphByGuid(Blueprint, Journal.Targets[Index].TargetGraphGuid) != RestoredByTarget[Index]) return false;
	}

	// Removed custom-event nodes and external macro instances return in one pass, ascending by original index,
	// so a host graph that lost both ends up in its original node order.
	struct FNodeRestoreRef
	{
		int32 TargetIndex = INDEX_NONE;
		int32 ItemIndex = INDEX_NONE;
		int32 OriginalIndex = INDEX_NONE;
		bool bMacroInstance = false;
	};
	TArray<FNodeRestoreRef> NodeRefs;
	for (int32 TargetIndex = 0; TargetIndex < Journal.Targets.Num(); ++TargetIndex)
	{
		const FRemoveTargetJournal& Target = Journal.Targets[TargetIndex];
		if (!Target.bWholeGraph)
		{
			for (int32 ItemIndex = 0; ItemIndex < Target.RemovedNodes.Num(); ++ItemIndex)
			{
				NodeRefs.Add({TargetIndex, ItemIndex, Target.RemovedNodes[ItemIndex].OriginalIndex, false});
			}
		}
		else if (Target.GraphType == TEXT("Macro"))
		{
			for (int32 ItemIndex = 0; ItemIndex < Target.ExternalMacroInstances.Num(); ++ItemIndex)
			{
				NodeRefs.Add({TargetIndex, ItemIndex, Target.ExternalMacroInstances[ItemIndex].OriginalIndex, true});
			}
		}
	}
	NodeRefs.Sort([](const FNodeRestoreRef& A, const FNodeRestoreRef& B)
	{
		return A.OriginalIndex < B.OriginalIndex;
	});
	// A macro instance inside a collapsed (composite-bound) graph cannot be restored until the composite node is back
	// and its original bound graph is reattached, so those instances wait for the pass below.
	TSet<FGuid> BoundHostGuids;
	for (const FRemoveGraphJournal::FCompositeBoundGraph& Saved : Journal.CompositeBoundGraphs)
	{
		if (Saved.BoundGraph.IsValid()) BoundHostGuids.Add(Saved.BoundGraph->GraphGuid);
	}
	TArray<FNodeRestoreRef> DeferredMacroRefs;
	for (const FNodeRestoreRef& Ref : NodeRefs)
	{
		FRemoveTargetJournal& Target = Journal.Targets[Ref.TargetIndex];
		if (Ref.bMacroInstance)
		{
			if (BoundHostGuids.Contains(Target.ExternalMacroInstances[Ref.ItemIndex].HostGraphGuid))
			{
				DeferredMacroRefs.Add(Ref);
				continue;
			}
			if (!RestoreMacroInstance(Blueprint, Target.ExternalMacroInstances[Ref.ItemIndex], RestoredByTarget[Ref.TargetIndex]))
				return false;
			continue;
		}
		const FRemovedGraphNode& Saved = Target.RemovedNodes[Ref.ItemIndex];
		UEdGraph* Graph = FindGraphByGuid(Blueprint, Target.TargetGraphGuid);
		if (!Graph || !Target.SnapshotGraph.IsValid()) return false;
		if (FindNodeByGuid(Graph, Saved.Guid)) continue;
		UEdGraphNode* Snapshot = FindNodeByGuid(Target.SnapshotGraph.Get(), Saved.Guid);
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
	if (!DeferredMacroRefs.IsEmpty())
	{
		// Every composite node is back now: reattach the original bound graphs (idempotent; the Blueprint-owned
		// restore runs it again), then restore the instances that live inside them in ascending index order.
		if (!RestoreCompositeBoundGraphs(Blueprint, Journal)) return false;
		for (const FNodeRestoreRef& Ref : DeferredMacroRefs)
		{
			FRemoveTargetJournal& Target = Journal.Targets[Ref.TargetIndex];
			if (!RestoreMacroInstance(Blueprint, Target.ExternalMacroInstances[Ref.ItemIndex], RestoredByTarget[Ref.TargetIndex]))
				return false;
		}
	}
	for (const FRemoveTargetJournal& Target : Journal.Targets)
	{
		if (Target.bWholeGraph && !RestoreMacroInstanceLinks(Blueprint, Target)) return false;
	}
	for (const FRemoveTargetJournal& Target : Journal.Targets)
	{
		if (Target.bWholeGraph) continue;
		UEdGraph* Graph = FindGraphByGuid(Blueprint, Target.TargetGraphGuid);
		for (const FRemoveBoundaryLink& Link : Target.BoundaryLinks)
		{
			UEdGraphNode* Source = FindNodeByGuid(Graph, Link.SourceNodeGuid);
			UEdGraphNode* LinkTarget = FindNodeByGuid(Graph, Link.TargetNodeGuid);
			UEdGraphPin* SourcePin = Source ? Source->FindPin(Link.SourcePin) : nullptr;
			UEdGraphPin* TargetPin = LinkTarget ? LinkTarget->FindPin(Link.TargetPin) : nullptr;
			if (!SourcePin || !TargetPin) return false;
			if (!SourcePin->LinkedTo.Contains(TargetPin)) SourcePin->MakeLinkTo(TargetPin);
		}
	}
	return RestoreBlueprintOwnedState(Blueprint, Journal, RestoredGraphs);
}

// Reads the target selector: exactly one of `name` (single graph or custom event) or `names` (a set).
bool ReadTargetNames(
	const TSharedPtr<FJsonObject>& Params,
	FCortexBPRemoveGraphPrepared& Prepared,
	FCortexCommandResult& OutError)
{
	const bool bHasName = Params->HasField(TEXT("name"));
	const bool bHasNames = Params->HasField(TEXT("names"));
	if (bHasName == bHasNames)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("remove_graph requires exactly one of name (one graph or custom event) or names (a non-empty array)"));
		return false;
	}
	if (bHasName)
	{
		FString Name;
		if (!ReadRequiredStrictString(Params, TEXT("name"), Name, OutError)) return false;
		Prepared.Names.Add(Name);
		return true;
	}
	Prepared.bBatch = true;
	const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("names"), NamesArray) || !NamesArray || NamesArray->Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("names must be a non-empty array of non-empty strings"));
		return false;
	}
	TSet<FString> Seen;
	for (const TSharedPtr<FJsonValue>& Value : *NamesArray)
	{
		FString Name;
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Name) || Name.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("names must be a non-empty array of non-empty strings"));
			return false;
		}
		if (Seen.Contains(Name))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("names lists '%s' more than once"), *Name));
			return false;
		}
		Seen.Add(Name);
		Prepared.Names.Add(Name);
	}
	return true;
}

// A set is applied as one unit, so no target may contain or share nodes with another.
bool ValidateDisjointTargets(const FCortexBPRemoveGraphPrepared& Prepared, FCortexCommandResult& OutError)
{
	TSet<FString> RemovedGraphGuids;
	for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
	{
		if (Item.Target->GetStringField(TEXT("kind")) == TEXT("graph"))
		{
			RemovedGraphGuids.Add(Item.Target->GetStringField(TEXT("graph_guid")));
		}
	}
	TSet<FString> RemovedNodeGuids;
	for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
	{
		if (Item.Target->GetStringField(TEXT("kind")) == TEXT("graph")) continue;
		if (RemovedGraphGuids.Contains(Item.Target->GetStringField(TEXT("graph_guid"))))
		{
			OutError = InvalidOperation(*FString::Printf(
				TEXT("Custom event '%s' lives in a graph that is also being removed; list only the graph"),
				*Item.Name));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : Item.Deletion->GetArrayField(TEXT("node_guids")))
		{
			bool bAlreadyRemoved = false;
			RemovedNodeGuids.Add(Value->AsString(), &bAlreadyRemoved);
			if (bAlreadyRemoved)
			{
				OutError = InvalidOperation(*FString::Printf(
					TEXT("Custom event '%s' shares nodes with another requested name; remove them in separate requests"),
					*Item.Name));
				return false;
			}
		}
	}
	return true;
}

void PublishTargets(const FCortexBPRemoveGraphPrepared& Prepared, FCortexBPRemoveGraphOutcome& Outcome)
{
	Outcome.bBatch = Prepared.bBatch;
	if (!Prepared.bBatch)
	{
		Outcome.Target = Prepared.Items[0].Target;
		Outcome.Deletion = Prepared.Items[0].Deletion;
		return;
	}
	for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
	{
		Outcome.Targets.Add(MakeShared<FJsonValueObject>(Item.Target));
		Outcome.Deletions.Add(MakeShared<FJsonValueObject>(Item.Deletion));
	}
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
	if (Outcome.bBatch)
	{
		Error.ErrorDetails->SetArrayField(TEXT("targets"), Outcome.Targets);
		Error.ErrorDetails->SetArrayField(TEXT("deletions"), Outcome.Deletions);
	}
	else
	{
		Error.ErrorDetails->SetObjectField(TEXT("target"), Outcome.Target);
		Error.ErrorDetails->SetObjectField(TEXT("deletion"), Outcome.Deletion);
	}
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
	Args.SaveFlags = SAVE_NoError;

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
		TEXT("asset_path"), TEXT("name"), TEXT("names"), TEXT("cascade_exec_chain"), TEXT("dry_run"),
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
		|| !ReadTargetNames(Params, Prepared, ParseError))
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
	if (Prepared.bDryRun
		&& (Params->HasField(TEXT("expected_fingerprint"))
			|| Params->HasField(TEXT("expected_validation_hash"))))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("expected_fingerprint and expected_validation_hash are apply-only fields"));
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
	Outcome.bBatch = Prepared.bBatch;
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
	if (!Prepared.bDryRun && (!ExpectedFingerprint.IsValid() || ExpectedValidationHash.IsEmpty()))
	{
		return ReturnErrorWithOutcome(FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
			TEXT("Apply requires expected_fingerprint and expected_validation_hash from preview")));
	}

	for (const FString& Name : Prepared.Names)
	{
		FCortexBPRemoveGraphItem Item;
		Item.Name = Name;
		FCortexCommandResult PlanError;
		if (!BuildPlan(Blueprint, Name, Prepared.bCascadeExecChain, Item, PlanError))
		{
			if (!Prepared.bDryRun && PlanError.ErrorCode == CortexErrorCodes::GraphNotFound)
			{
				PlanError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
					FString::Printf(TEXT("The preview target '%s' no longer exists; request a new preview"), *Name));
			}
			return ReturnErrorWithOutcome(PlanError);
		}
		Prepared.Items.Add(MoveTemp(Item));
	}
	FCortexCommandResult OverlapError;
	if (!ValidateDisjointTargets(Prepared, OverlapError))
	{
		return ReturnErrorWithOutcome(OverlapError);
	}
	PublishTargets(Prepared, Outcome);
	Prepared.FingerprintBefore = FCortexGraphFingerprint::Compute(Blueprint);
	if (!Prepared.FingerprintBefore.IsValid())
	{
		return ReturnErrorWithOutcome(FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation, TEXT("Could not compute Blueprint graph fingerprint")));
	}
	Outcome.FingerprintBefore = Prepared.FingerprintBefore;
	Outcome.FingerprintAfter = Prepared.FingerprintBefore;
	BuildValidationHash(Prepared);
	if (Prepared.bDryRun)
	{
		Outcome.bChanged = true;
	}

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
			FText::FromString(Prepared.bBatch
				? TEXT("Cortex: Remove Blueprint Graphs") : TEXT("Cortex: Remove Blueprint Graph")));
		Blueprint->Modify();
		bool bMutationSucceeded = true;
		TArray<UEdGraph*> TargetGraphs;
		TargetGraphs.Reserve(Journal.Targets.Num());
		for (const FRemoveTargetJournal& Target : Journal.Targets)
		{
			UEdGraph* TargetGraph = FindGraphByGuid(Blueprint, Target.TargetGraphGuid);
			bMutationSucceeded &= TargetGraph != nullptr;
			TargetGraphs.Add(TargetGraph);
		}
		// Macro instances go first, while every host graph still exists.
		for (int32 TargetIndex = 0; bMutationSucceeded && TargetIndex < Journal.Targets.Num(); ++TargetIndex)
		{
			const FRemoveTargetJournal& Target = Journal.Targets[TargetIndex];
			if (!Target.bWholeGraph || Target.GraphType != TEXT("Macro")) continue;
			for (const FRemoveMacroInstance& Saved : Target.ExternalMacroInstances)
			{
				UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Saved.HostGraphGuid);
				UK2Node_MacroInstance* Instance = Cast<UK2Node_MacroInstance>(
					FindNodeByGuid(HostGraph, Saved.NodeGuid));
				if (!Instance || Instance->GetMacroGraph() != TargetGraphs[TargetIndex])
				{
					bMutationSucceeded = false;
					break;
				}
				FBlueprintEditorUtils::RemoveNode(Blueprint, Instance, true);
			}
		}
		for (int32 TargetIndex = 0; bMutationSucceeded && TargetIndex < Journal.Targets.Num(); ++TargetIndex)
		{
			const FRemoveTargetJournal& Target = Journal.Targets[TargetIndex];
			UEdGraph* TargetGraph = TargetGraphs[TargetIndex];
			if (Target.bWholeGraph)
			{
				FBlueprintEditorUtils::RemoveGraph(Blueprint, TargetGraph, EGraphRemoveFlags::MarkTransient);
				continue;
			}
			for (const FGuid& Guid : Target.RemovedNodeGuids)
			{
				UEdGraphNode* Node = FindNodeByGuid(TargetGraph, Guid);
				if (!Node)
				{
					// Already removed with its macro (a cascade can reach a macro instance in the same host graph).
					if (Journal.RemovedMacroInstanceGuids.Contains(Guid)) continue;
					bMutationSucceeded = false;
					break;
				}
				UEdGraph* BoundGraph = nullptr;
				if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
				{
					BoundGraph = Composite->BoundGraph;
					Composite->BoundGraph = nullptr;
				}
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
				if (BoundGraph)
					FBlueprintEditorUtils::RemoveGraph(Blueprint, BoundGraph, EGraphRemoveFlags::MarkTransient);
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
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &Log);
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
			for (const FRemoveTargetJournal& Target : Journal.Targets)
			{
				if (Target.bWholeGraph)
				{
					bMatched &= FindGraphByGuid(Blueprint, Target.TargetGraphGuid) == nullptr;
					for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
						if (Candidate && Candidate->GraphGuid == Target.TargetGraphGuid) bMatched = false;
					for (UEdGraph* Candidate : Blueprint->MacroGraphs)
						if (Candidate && Candidate->GraphGuid == Target.TargetGraphGuid) bMatched = false;
					for (int32 Index = 0; Index < Blueprint->UbergraphPages.Num(); ++Index)
						if (Blueprint->UbergraphPages[Index] && Blueprint->UbergraphPages[Index]->GraphGuid == Target.TargetGraphGuid) bMatched = false;
					for (const FRemoveMacroInstance& Instance : Target.ExternalMacroInstances)
					{
						UEdGraph* HostGraph = FindGraphByGuid(Blueprint, Instance.HostGraphGuid);
						bMatched &= !FindNodeByGuid(HostGraph, Instance.NodeGuid);
					}
					continue;
				}
				UEdGraph* Graph = FindGraphByGuid(Blueprint, Target.TargetGraphGuid);
				for (const FGuid& Guid : Target.RemovedNodeGuids) bMatched &= FindNodeByGuid(Graph, Guid) == nullptr;
				for (const FGuid& Guid : Target.PreservedNodeGuids) bMatched &= FindNodeByGuid(Graph, Guid) != nullptr;
				for (const FRemoveBoundaryLink& Link : Target.BoundaryLinks)
				{
					if (!Journal.RemovedNodeGuidSet.Contains(Link.TargetNodeGuid))
					{
						UEdGraphNode* Preserved = FindNodeByGuid(Graph, Link.TargetNodeGuid);
						bMatched &= Preserved && Preserved->FindPin(Link.TargetPin);
					}
				}
			}
			for (const FGuid& Guid : Journal.PreservedGraphGuids)
				bMatched &= FindGraphByGuid(Blueprint, Guid) != nullptr;
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
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave, &RecoveryLog);
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
	if (Prepared.bBatch)
	{
		for (FCortexBPRemoveGraphItem& Item : Prepared.Items)
		{
			Item.Removed->SetArrayField(TEXT("removed_nodes"), Item.RemovedNodes);
		}
		TArray<TSharedPtr<FJsonValue>> RemovedGraphs;
		for (const FCortexBPRemoveGraphItem& Item : Prepared.Items)
		{
			RemovedGraphs.Add(MakeShared<FJsonValueObject>(Item.Removed));
		}
		Data->SetArrayField(TEXT("removed_graphs"), RemovedGraphs);
		Data->SetNumberField(TEXT("removed_count"), Prepared.Items.Num());
		Data->SetArrayField(TEXT("targets"), Outcome.Targets);
		Data->SetArrayField(TEXT("deletions"), Outcome.Deletions);
	}
	else
	{
		Data->SetObjectField(TEXT("removed"), Prepared.Items[0].Removed);
		Data->SetArrayField(TEXT("removed_nodes"), Prepared.Items[0].RemovedNodes);
		Data->SetObjectField(TEXT("target"), Outcome.Target);
		Data->SetObjectField(TEXT("deletion"), Outcome.Deletion);
	}
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FString& Diagnostic : Outcome.Diagnostics)
	{
		Diagnostics.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	if (Prepared.bDryRun) Data->SetStringField(TEXT("validation_hash"), Prepared.ValidationHash);
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
