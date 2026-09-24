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
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "IO/IoHash.h"
#include "Misc/Char.h"
#include "Containers/StringConv.h"

namespace
{
#if WITH_AUTOMATION_TESTS
FName RemoveGraphFaultPoint;
#endif

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
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	if (Prepared.bSave && bDirtyBefore)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::DirtyEditorState, TEXT("save=true requires a clean starting package"));
	}

	TArray<TSharedPtr<FJsonValue>> RemovedNodes;
	TSharedPtr<FJsonObject> Removed;
	FCortexCommandResult PlanError;
	if (!BuildPlan(Blueprint, Prepared, RemovedNodes, Removed, PlanError))
	{
		return PlanError;
	}
	Prepared.FingerprintBefore = FCortexGraphFingerprint::Compute(Blueprint);
	if (!Prepared.FingerprintBefore.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Could not compute Blueprint graph fingerprint"));
	}
	BuildValidationHash(Prepared);

	if (!Prepared.bDryRun)
	{
		const bool bHasExpectedFingerprint = ExpectedFingerprint.IsValid();
		if (!bHasExpectedFingerprint || ExpectedValidationHash.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::StalePrecondition,
				TEXT("Apply requires expected_fingerprint and expected_validation_hash from preview"));
		}
		const TSharedPtr<FJsonObject> CurrentFingerprint = FCortexGraphFingerprint::Compute(Blueprint);
		FCortexCommandResult FingerprintError;
		if (!FCortexGraphFingerprint::ValidatePrecondition(ExpectedFingerprint, CurrentFingerprint, FingerprintError))
		{
			return FingerprintError;
		}
		if (ExpectedValidationHash != Prepared.ValidationHash)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::StalePrecondition,
				TEXT("expected_validation_hash does not match the current remove_graph plan"));
		}
		return InvalidOperation(TEXT("remove_graph apply coordinator is not enabled in this intermediate commit"));
	}

	FCortexBPRemoveGraphOutcome Outcome;
	Outcome.bChanged = true;
	Outcome.FingerprintBefore = Prepared.FingerprintBefore;
	Outcome.FingerprintAfter = Prepared.FingerprintBefore;
	Outcome.bDirtyBefore = bDirtyBefore;
	Outcome.bDirtyAfter = Outcome.bDirtyBefore;
	Outcome.Target = Prepared.Target;
	Outcome.Deletion = Prepared.Deletion;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Prepared.AssetPath);
	Data->SetObjectField(TEXT("removed"), Removed);
	Data->SetArrayField(TEXT("removed_nodes"), RemovedNodes);
	Data->SetBoolField(TEXT("dry_run"), true);
	Data->SetBoolField(TEXT("compiled"), false);
	Data->SetStringField(TEXT("apply_status"), Outcome.ApplyStatus);
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
