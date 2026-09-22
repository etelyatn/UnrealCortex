#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphAuthoringContext.h"
#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphNodeContract.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "CortexEngineCompat.h"
#include "IO/IoHash.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "String/BytesToHex.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/Char.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Engine/World.h"
#include "CortexSerializer.h"
#include "CortexAssetMutationGuard.h"

namespace
{
/**
 * The authoring shell and the migration shell validate and canonicalize through the same class
 * statics; these forwarders keep the existing unqualified call sites inside this file.
 */
bool HasOnlyFields(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Allowed, FCortexCommandResult& OutError, const FString& Context)
{
	return FCortexGraphPatchOps::HasOnlyFields(Object, Allowed, OutError, Context);
}

bool ReadStrictBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool DefaultValue, bool& OutValue, FCortexCommandResult& OutError)
{
	return FCortexGraphPatchOps::ReadStrictBool(Object, Field, DefaultValue, OutValue, OutError);
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue, FCortexCommandResult& OutError)
{
	return FCortexGraphPatchOps::ReadRequiredString(Object, Field, OutValue, OutError);
}

bool ParseGuidField(const TSharedPtr<FJsonObject>& Object, const FString& Field, FGuid& OutGuid, FCortexCommandResult& OutError)
{
	return FCortexGraphPatchOps::ParseGuidField(Object, Field, OutGuid, OutError);
}

bool ResolveGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid, const FString& SubgraphPath, UEdGraph*& OutGraph, FCortexCommandResult& OutError)
{
	return FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, GraphGuid, SubgraphPath, OutGraph, OutError);
}

TSharedPtr<FJsonObject> MakePinSignatureDescriptor(const UEdGraphPin& Pin)
{
	return FCortexGraphPatchOps::MakePinSignatureDescriptor(Pin);
}

FString CanonicalPinSignature(const TSharedPtr<FJsonObject>& Descriptor)
{
	return FCortexGraphPatchOps::CanonicalPinSignature(Descriptor);
}

constexpr int32 MaxNodes = 64;
constexpr int32 MaxEdges = 256;
constexpr int32 MaxClientIdLength = 32;
constexpr int32 MaxRequestSize = 64 * 1024;
constexpr int32 MaxScannedNodes = 2048;

#if WITH_AUTOMATION_TESTS
FName ApplyFaultPointForTesting = NAME_None;
bool bSaveFaultForTesting = false;
FName PostSaveVerificationFaultForTesting = NAME_None;
FName ReadbackFaultForTesting = NAME_None;
TFunction<void(FName, UBlueprint*)> OperationObserverForTesting;
/** Test-only native-state mutator invoked after apply and before readback. */
TFunction<void(UBlueprint*)> PreReadbackMutatorForTesting;
#endif

bool ShouldInjectApplyFault(const FName Point)
{
#if WITH_AUTOMATION_TESTS
	return ApplyFaultPointForTesting == Point;
#else
	return false;
#endif
}

/** Test-only persistence fault: makes the single target save report failure. */
bool ShouldInjectSaveFault()
{
#if WITH_AUTOMATION_TESTS
	return bSaveFaultForTesting;
#else
	return false;
#endif
}

/** Test-only persistence fault: fails exactly one named post-save persistence check. */
bool ShouldInjectPostSaveFault(const FName Check)
{
#if WITH_AUTOMATION_TESTS
	return PostSaveVerificationFaultForTesting == Check;
#else
	(void)Check;
	return false;
#endif
}

void MutateNativeStateBeforeReadback(UBlueprint* Blueprint)
{
#if WITH_AUTOMATION_TESTS
	if (PreReadbackMutatorForTesting)
	{
		PreReadbackMutatorForTesting(Blueprint);
	}
#else
	(void)Blueprint;
#endif
}

bool ShouldInjectReadbackFault(const FName Field)
{
#if WITH_AUTOMATION_TESTS
	return ReadbackFaultForTesting == Field;
#else
	(void)Field;
	return false;
#endif
}

void NotifyOperation(const FName Operation, UBlueprint* Blueprint)
{
#if WITH_AUTOMATION_TESTS
	if (OperationObserverForTesting)
	{
		OperationObserverForTesting(Operation, Blueprint);
	}
#else
	(void)Operation;
	(void)Blueprint;
#endif
}

/**
 * Divergence ledger of one planned-intent comparison pass. Every canonical dimension is corrupted
 * at most once, and only when the comparison pass explicitly allows test fault injection, so a
 * single injected fault proves the comparison really runs.
 */
struct FReadbackFaultState
{
	bool bInjectionAllowed = false;
	bool bClass = false;
	bool bSymbol = false;
	bool bDefault = false;
	bool bEdge = false;

	/** True when the named divergence must corrupt this comparison pass exactly once. */
	bool Inject(const FName Field, bool& bDimension)
	{
		if (!bInjectionAllowed || bDimension || !ShouldInjectReadbackFault(Field)) return false;
		bDimension = true;
		return true;
	}

	bool InjectClass() { return Inject(TEXT("readback_class"), bClass); }
	bool InjectSymbol() { return Inject(TEXT("readback_symbol"), bSymbol); }
	bool InjectDefault() { return Inject(TEXT("readback_default"), bDefault); }
	bool InjectEdge() { return Inject(TEXT("readback_edge"), bEdge); }
};

/**
 * Deterministic node identity of one planned client id: a stable hash of the canonical patch GUID
 * and the client id. No clock, pointer or random input is involved, so the same pair derives the
 * same GUID in every asset and every process.
 */
FGuid DerivePlannedNodeGuid(const FString& PatchId, const FString& ClientId)
{
	const FString Seed = FString::Printf(TEXT("cortex.graph.patch:%s:%s"), *PatchId, *ClientId);
	FTCHARToUTF8 SeedUtf8(*Seed);
	FMD5 Md5;
	Md5.Update(reinterpret_cast<const uint8*>(SeedUtf8.Get()), SeedUtf8.Length());
	FMD5Hash Digest;
	Digest.Set(Md5);
	FGuid Guid = MD5HashToGuid(Digest);
	if (!Guid.IsValid())
	{
		// A valid GUID is a hard requirement for node identity, and an all-zero digest is the only
		// way this derivation could produce one; keep the repair deterministic instead of random.
		Guid.A = 1;
	}
	return Guid;
}

/**
 * Every graph that owns a node GUID anywhere in the Blueprint. A deterministic identity must be
 * unambiguous inside the asset, so the caller refuses when more than one graph owns the GUID or when
 * the only owner is not the target graph; a single match inside the target graph is the reuse case.
 */
void FindGraphsOwningNodeGuid(
	UBlueprint* Blueprint,
	const FGuid& NodeGuid,
	TArray<UEdGraph*>& OutGraphs,
	UEdGraphNode*& OutFirstNode)
{
	OutGraphs.Reset();
	OutFirstNode = nullptr;
	if (!Blueprint || !NodeGuid.IsValid()) return;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				OutGraphs.Add(Graph);
				if (!OutFirstNode) OutFirstNode = Node;
				break;
			}
		}
	}
}

/** Durable locators of a prepared patch: every planned identity, known before the first mutation. */
FCortexGraphPatchLocators MakePreparedLocators(const FCortexGraphPreparedPatch& Prepared)
{
	FCortexGraphPatchLocators Locators;
	FGuid::Parse(Prepared.GraphGuid, Locators.GraphGuid);
	Locators.SubgraphPath = Prepared.SubgraphPath;
	Locators.NodeGuidByClientId = Prepared.NodeGuidByClientId;
	Locators.EntryNodeGuid = Prepared.EntryNodeGuid;
	Locators.bHasEntryNode = Prepared.bHasEntryNode;
	return Locators;
}

/**
 * Compares the whole planned intent against the live native asset. It is defined together with the
 * native comparison helpers below and is shared by readback and by the planning-time reuse
 * reconciliation, so exactly one comparator defines what "matches the planned intent" means.
 */
bool ComparePlannedIntentAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FCortexGraphPatchLocators& Locators,
	bool bCompiled,
	bool bAllowFaultInjection,
	FString& OutFailure);

/**
 * Compares one normalized planned node against a live native node on every canonical planned
 * dimension: node class, resolved symbol, planned pin signatures, planned tagged defaults and the
 * authored layout where a position was planned.
 */
bool ComparePlannedNodeAgainstNative(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeJson,
	UEdGraphNode* Live,
	FReadbackFaultState& Faults,
	FString& OutFailure);

bool IsJsonType(const TSharedPtr<FJsonValue>& Value, EJson Expected)
{
	return Value.IsValid() && Value->Type == Expected;
}

bool IsAsciiClientId(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > MaxClientIdLength) return false;
	for (const TCHAR Char : Value)
	{
		if (Char > 127 || !(FChar::IsAlnum(Char) || Char == TCHAR('_') || Char == TCHAR('-')))
		{
			return false;
		}
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
	case TCHAR('\n'): Out += TEXT("\\n"); break;
	case TCHAR('\r'): Out += TEXT("\\r"); break;
	case TCHAR('\t'): Out += TEXT("\\t"); break;
	default: Out += Char; break;
		}
	}
	Out += TCHAR('"');
}

void AppendCanonical(const TSharedPtr<FJsonValue>& Value, FString& Out);

void AppendCanonicalObject(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
	Out += TCHAR('{');
	TArray<FString> Keys;
	for (const auto& Pair : Object->Values) Keys.Add(CortexEngineCompat::JsonKeyToString(Pair.Key));
	Keys.Sort();
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		if (Index > 0) Out += TCHAR(',');
		AppendQuoted(Keys[Index], Out);
		Out += TCHAR(':');
		for (const auto& Pair : Object->Values)
		{
			if (CortexEngineCompat::JsonKeyToString(Pair.Key) == Keys[Index])
			{
				AppendCanonical(Pair.Value, Out);
				break;
			}
		}
	}
	Out += TCHAR('}');
}

void AppendCanonical(const TSharedPtr<FJsonValue>& Value, FString& Out)
{
	if (!Value.IsValid()) { Out += TEXT("null"); return; }
	switch (Value->Type)
	{
	case EJson::Object: AppendCanonicalObject(Value->AsObject(), Out); break;
	case EJson::Array:
		{
			Out += TCHAR('[');
			const TArray<TSharedPtr<FJsonValue>> Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (Index > 0) Out += TCHAR(',');
				AppendCanonical(Array[Index], Out);
			}
			Out += TCHAR(']');
		}
		break;
	case EJson::String: AppendQuoted(Value->AsString(), Out); break;
	case EJson::Number: Out += FString::Printf(TEXT("%.17g"), Value->AsNumber()); break;
	case EJson::Boolean: Out += Value->AsBool() ? TEXT("true") : TEXT("false"); break;
	case EJson::Null: Out += TEXT("null"); break;
	default: Out += TEXT("null"); break;
	}
}

FString CanonicalObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Result;
	AppendCanonicalObject(Object, Result);
	return Result;
}
void AddPropertyTypeIdentity(const FProperty* Property, const TSharedPtr<FJsonObject>& Out)
{
	if (!Property || !Out.IsValid()) return;
	Out->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
	Out->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
	Out->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		Out->SetStringField(TEXT("object_class"), ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : FString());
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		Out->SetStringField(TEXT("struct"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
	}
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Out->SetStringField(TEXT("enum"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : FString());
		AddPropertyTypeIdentity(EnumProperty->GetUnderlyingProperty(), Out);
	}
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		Out->SetStringField(TEXT("enum"), ByteProperty->Enum ? ByteProperty->Enum->GetPathName() : FString());
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		TSharedPtr<FJsonObject> Inner = MakeShared<FJsonObject>();
		AddPropertyTypeIdentity(ArrayProperty->Inner, Inner);
		Out->SetObjectField(TEXT("inner"), Inner);
	}
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		TSharedPtr<FJsonObject> Element = MakeShared<FJsonObject>();
		AddPropertyTypeIdentity(SetProperty->ElementProp, Element);
		Out->SetObjectField(TEXT("element"), Element);
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		AddPropertyTypeIdentity(MapProperty->KeyProp, Key);
		AddPropertyTypeIdentity(MapProperty->ValueProp, Value);
		Out->SetObjectField(TEXT("key"), Key);
		Out->SetObjectField(TEXT("value"), Value);
	}
}

void AddFunctionSignature(UFunction* Function, const TSharedPtr<FJsonObject>& Out)
{
	if (!Function || !Out.IsValid()) return;
	Out->SetStringField(TEXT("signature_owner"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
	Out->SetNumberField(TEXT("signature_flags"), static_cast<double>(Function->FunctionFlags));
	TArray<TSharedPtr<FJsonValue>> Parameters;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm)) continue;
		TSharedPtr<FJsonObject> Parameter = MakeShared<FJsonObject>();
		Parameter->SetStringField(TEXT("name"), Property->GetName());
		Parameter->SetNumberField(TEXT("flags"), static_cast<double>(Property->PropertyFlags));
		Parameter->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
		AddPropertyTypeIdentity(Property, Parameter);
		Parameters.Add(MakeShared<FJsonValueObject>(Parameter));
	}
	Out->SetArrayField(TEXT("signature_parameters"), Parameters);
}


bool CountGraphNodesBounded(UEdGraph* Graph, int32& InOutCount, TSet<const UEdGraph*>& Visited)
{
	if (!Graph || Visited.Contains(Graph)) return true;
	Visited.Add(Graph);
	InOutCount += Graph->Nodes.Num();
	if (InOutCount > MaxScannedNodes) return false;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
		if (Composite && !CountGraphNodesBounded(Composite->BoundGraph, InOutCount, Visited)) return false;
	}
	return true;
}

bool CountBlueprintNodesBounded(UBlueprint* Blueprint)
{
	TArray<FCortexGraphEntry> Entries;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);
	TSet<const UEdGraph*> Visited;
	int32 Count = 0;
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (!CountGraphNodesBounded(Entry.Graph, Count, Visited)) return false;
	}
	return true;
}

bool ParseTarget(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	TSharedPtr<FJsonObject>& OutTarget,
	UEdGraph*& OutGraph,
	TSharedPtr<FJsonObject>& OutSymbolJson,
	bool& OutImplementationWouldCreate,
	bool& OutImplementationIsEvent,
	bool& OutImplementationHasParentCall,
	FCortexCommandResult& OutError)
{
	OutImplementationWouldCreate = false;
	OutImplementationIsEvent = false;
	OutImplementationHasParentCall = false;
	const TSharedPtr<FJsonObject>* TargetPtr = nullptr;
	if (!Request->TryGetObjectField(TEXT("target"), TargetPtr) || !TargetPtr || !TargetPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target must be an object"));
		return false;
	}
	OutTarget = *TargetPtr;
	if (!HasOnlyFields(OutTarget, { TEXT("graph_ref"), TEXT("implementation") }, OutError, TEXT("target"))) return false;
	const bool bGraph = OutTarget->HasField(TEXT("graph_ref"));
	const bool bImplementation = OutTarget->HasField(TEXT("implementation"));
	if (bGraph == bImplementation)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target must specify exactly one of graph_ref or implementation"));
		return false;
	}

	if (bGraph)
	{
		const TSharedPtr<FJsonObject>* RefPtr = nullptr;
		if (!OutTarget->TryGetObjectField(TEXT("graph_ref"), RefPtr) || !RefPtr || !RefPtr->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target.graph_ref must be an object"));
			return false;
		}
		const TSharedPtr<FJsonObject>& Ref = *RefPtr;
		if (!HasOnlyFields(Ref, { TEXT("graph_guid"), TEXT("graph_kind"), TEXT("subgraph_path") }, OutError, TEXT("target.graph_ref"))) return false;
		FGuid GraphGuid;
		if (!ParseGuidField(Ref, TEXT("graph_guid"), GraphGuid, OutError)) return false;
		FString SubgraphPath;
		if (Ref->HasField(TEXT("subgraph_path")) && !Ref->TryGetStringField(TEXT("subgraph_path"), SubgraphPath))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target.graph_ref.subgraph_path must be a string"));
			return false;
		}
		if (!ResolveGraphByGuid(Blueprint, GraphGuid, SubgraphPath, OutGraph, OutError)) return false;
		if (Ref->HasField(TEXT("graph_kind")))
		{
			FString RequestedKind;
			if (!Ref->TryGetStringField(TEXT("graph_kind"), RequestedKind))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target.graph_ref.graph_kind must be a string"));
				return false;
			}
			TArray<FCortexGraphEntry> Entries;
			FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);
			for (const FCortexGraphEntry& Entry : Entries)
			{
				if (Entry.Graph && Entry.Graph->GraphGuid == GraphGuid && FCortexGraphNodeOps::GraphKindToString(Entry.Kind) != RequestedKind)
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target graph_kind conflicts with graph identity"));
					return false;
				}
			}
		}
		return true;
	}

	const TSharedPtr<FJsonObject>* ImplementationPtr = nullptr;
	if (!OutTarget->TryGetObjectField(TEXT("implementation"), ImplementationPtr) || !ImplementationPtr || !ImplementationPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target.implementation must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& Selector = *ImplementationPtr;
	if (!HasOnlyFields(Selector, { TEXT("owner_class"), TEXT("function_name"), TEXT("call_kind") }, OutError, TEXT("target.implementation"))) return false;
	FCortexGraphImplementationPlan Plan;
	if (!FCortexGraphImplementationOps::ValidateEligibility(Blueprint, Selector, Plan, OutError)) return false;
	OutGraph = Plan.ExistingGraph;
	OutImplementationWouldCreate = Plan.bWouldCreate;
	OutImplementationIsEvent = Plan.bCanBePlacedAsEvent;
	OutImplementationHasParentCall = Plan.bParentCall;
	OutSymbolJson = MakeShared<FJsonObject>();
	OutSymbolJson->SetStringField(TEXT("function_name"), Plan.Function->GetName());
	OutSymbolJson->SetStringField(TEXT("owner_class"), Plan.FunctionClass ? Plan.FunctionClass->GetPathName() : FString());
	AddFunctionSignature(Plan.Function, OutSymbolJson);
	return true;
}
bool ParseEndpoint(const TSharedPtr<FJsonObject>& Endpoint, FString& OutIdentity, FString& OutPin, bool& OutEntry, FCortexCommandResult& OutError, const FString& Context)
{
	OutEntry = false;
	if (!Endpoint.IsValid() || !HasOnlyFields(Endpoint, { TEXT("client_id"), TEXT("node_guid"), TEXT("entry"), TEXT("pin") }, OutError, Context)) return false;
	const bool bClient = Endpoint->HasField(TEXT("client_id"));
	const bool bGuid = Endpoint->HasField(TEXT("node_guid"));
	const bool bEntry = Endpoint->HasField(TEXT("entry"));
	if (static_cast<int32>(bClient) + static_cast<int32>(bGuid) + static_cast<int32>(bEntry) != 1)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s requires exactly one endpoint identity"), *Context));
		return false;
	}
	if (bEntry)
	{
		bool EntryValue = false;
		if (!Endpoint->TryGetBoolField(TEXT("entry"), EntryValue) || !EntryValue)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s.entry must be true"), *Context));
			return false;
		}
		OutEntry = true;
		OutIdentity = TEXT("entry");
	}
	else if (bClient)
	{
		if (!ReadRequiredString(Endpoint, TEXT("client_id"), OutIdentity, OutError) || !IsAsciiClientId(OutIdentity))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s.client_id is invalid"), *Context));
			return false;
		}
	}
	else
	{
		FGuid Guid;
		if (!ParseGuidField(Endpoint, TEXT("node_guid"), Guid, OutError)) return false;
		OutIdentity = Guid.ToString();
	}
	if (!ReadRequiredString(Endpoint, TEXT("pin"), OutPin, OutError)) return false;
	return true;
}

UEdGraphPin* FindPlannedPin(const TMap<FString, UEdGraphNode*>& Nodes, const FString& Identity, const FString& PinName, UEdGraph* ExistingGraph, FCortexCommandResult& OutError)
{
	if (UEdGraphNode* const* Planned = Nodes.Find(Identity))
	{
		UEdGraphPin* Pin = (*Planned)->FindPin(FName(*PinName));
		if (!Pin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound, FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *Identity));
		}
		return Pin;
	}
	if (!ExistingGraph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, FString::Printf(TEXT("Node '%s' not found"), *Identity));
		return nullptr;
	}
	FGuid Guid;
	const bool bGuid = FGuid::Parse(Identity, Guid);
	for (UEdGraphNode* Node : ExistingGraph->Nodes)
	{
		if (Node && ((bGuid && Node->NodeGuid == Guid) || (!bGuid && Node->GetName() == Identity)))
		{
			UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
			if (!Pin) OutError = FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound, FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *Identity));
			return Pin;
		}
	}
	OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, FString::Printf(TEXT("Node '%s' not found"), *Identity));
	return nullptr;
}

void CloneExistingGraphIntoPlanningGraph(
	UEdGraph* ExistingGraph,
	UEdGraph* PlanningGraph,
	TMap<FString, UEdGraphNode*>& InOutPlannedNodes)
{
	if (!ExistingGraph || !PlanningGraph) return;

	TMap<const UEdGraphPin*, UEdGraphPin*> PinClones;
	for (UEdGraphNode* ExistingNode : ExistingGraph->Nodes)
	{
		if (!ExistingNode) continue;
		UEdGraphNode* Clone = Cast<UEdGraphNode>(StaticDuplicateObject(ExistingNode, PlanningGraph, NAME_None, RF_Transient));
		if (!Clone) continue;
		PlanningGraph->AddNode(Clone, false, false);
		InOutPlannedNodes.Add(ExistingNode->NodeGuid.ToString(), Clone);
		for (int32 PinIndex = 0; PinIndex < ExistingNode->Pins.Num() && PinIndex < Clone->Pins.Num(); ++PinIndex)
		{
			if (ExistingNode->Pins[PinIndex] && Clone->Pins[PinIndex])
			{
				Clone->Pins[PinIndex]->LinkedTo.Reset();
				PinClones.Add(ExistingNode->Pins[PinIndex], Clone->Pins[PinIndex]);
			}
		}
	}

	for (const TPair<const UEdGraphPin*, UEdGraphPin*>& Pair : PinClones)
	{
		for (UEdGraphPin* LinkedPin : Pair.Key->LinkedTo)
		{
			if (UEdGraphPin* const* LinkedClone = PinClones.Find(LinkedPin))
			{
				if (!Pair.Value->LinkedTo.Contains(*LinkedClone))
				{
					Pair.Value->MakeLinkTo(*LinkedClone);
				}
			}
		}
	}
}

bool AddNormalizedNode(const TSharedPtr<FJsonObject>& Node, TSharedPtr<FJsonObject>& OutNode, FCortexCommandResult& OutError)
{
	if (!HasOnlyFields(Node, { TEXT("client_id"), TEXT("node_class"), TEXT("params"), TEXT("defaults"), TEXT("position") }, OutError, TEXT("node"))) return false;
	FString ClientId;
	FString NodeClass;
	if (!ReadRequiredString(Node, TEXT("client_id"), ClientId, OutError) || !IsAsciiClientId(ClientId))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.client_id must be 1-32 ASCII letters, digits, '_' or '-'"));
		return false;
	}
	if (!ReadRequiredString(Node, TEXT("node_class"), NodeClass, OutError)) return false;
	OutNode = MakeShared<FJsonObject>();
	OutNode->SetStringField(TEXT("client_id"), ClientId);
	OutNode->SetStringField(TEXT("node_class"), NodeClass);
	if (Node->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Node->TryGetObjectField(TEXT("params"), Obj) || !Obj || !Obj->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.params must be an object"));
			return false;
		}
		OutNode->SetObjectField(TEXT("params"), *Obj);
	}
	if (Node->HasField(TEXT("defaults")))
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Node->TryGetObjectField(TEXT("defaults"), Obj) || !Obj || !Obj->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.defaults must be an object"));
			return false;
		}
		OutNode->SetObjectField(TEXT("defaults"), *Obj);
	}
	if (Node->HasField(TEXT("position")))
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Node->TryGetObjectField(TEXT("position"), Obj) || !Obj || !Obj->IsValid() || !(*Obj)->HasField(TEXT("x")) || !(*Obj)->HasField(TEXT("y")))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.position must contain numeric x and y"));
			return false;
		}
		int32 X = 0, Y = 0;
		if (!(*Obj)->TryGetNumberField(TEXT("x"), X) || !(*Obj)->TryGetNumberField(TEXT("y"), Y))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.position x and y must be numbers"));
			return false;
		}
		if (!HasOnlyFields(*Obj, { TEXT("x"), TEXT("y") }, OutError, TEXT("node.position"))) return false;
		OutNode->SetObjectField(TEXT("position"), *Obj);
	}
	return true;
}
bool ValidateConstructionParamShape(const FString& NodeClass, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
	if (!Params.IsValid()) return true;
	const FCortexNodeConstructionContract Contract = FCortexGraphNodeContract::Describe(NodeClass);
	TSet<FString> Allowed;
	for (const FCortexNodeConstructionParam& Param : Contract.RequiredParams) Allowed.Add(Param.Name);
	for (const FCortexNodeConstructionParam& Param : Contract.OptionalParams) Allowed.Add(Param.Name);
	for (const FString& Selector : Contract.Selectors) Allowed.Add(Selector);
	FName FamilyName;
	UClass* ResolvedClass = nullptr;
	FCortexGraphNodeContract::ResolveFamily(NodeClass, FamilyName, ResolvedClass);
	const FString Family = FamilyName.ToString();
	if (Family == TEXT("DynamicCast"))
	{
		Allowed.Add(TEXT("target_class"));
		Allowed.Add(TEXT("is_pure"));
		Allowed.Add(TEXT("pure"));
		Allowed.Add(TEXT("bIsPureCast"));
	}
	for (const auto& Pair : Params->Values)
	{
		const FString Name = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (!Allowed.Contains(Name))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown construction parameter '%s' for node class '%s'"), *Name, *NodeClass));
			return false;
		}
	}
	if (Family == TEXT("CallFunction") && Params->HasField(TEXT("function_name")) && Params->HasField(TEXT("member")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Conflicting function selector fields"));
		return false;
	}
	if (Family == TEXT("Event") && !Params->HasField(TEXT("function_name"))
		&& (Params->HasField(TEXT("owner_class")) || Params->HasField(TEXT("variable_class"))))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("Event node requires function_name when an owner class is declared"));
		return false;
	}
	if (Family == TEXT("DynamicCast"))
	{
		if (Params->HasField(TEXT("class")) && Params->HasField(TEXT("target_class")))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Conflicting dynamic-cast class selector aliases"));
			return false;
		}
		const int32 PurityAliases = static_cast<int32>(Params->HasField(TEXT("is_pure")))
			+ static_cast<int32>(Params->HasField(TEXT("pure")))
			+ static_cast<int32>(Params->HasField(TEXT("bIsPureCast")));
		if (PurityAliases > 1)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Conflicting dynamic-cast purity aliases"));
			return false;
		}
	}
	for (const auto& Pair : Params->Values)
	{
		const FString Name = CortexEngineCompat::JsonKeyToString(Pair.Key);
		const FCortexNodeConstructionParam* Spec = nullptr;
		for (const FCortexNodeConstructionParam& Candidate : Contract.RequiredParams) if (Candidate.Name == Name) Spec = &Candidate;
		for (const FCortexNodeConstructionParam& Candidate : Contract.OptionalParams) if (Candidate.Name == Name) Spec = &Candidate;
		if (!Spec) continue;
		const FString Type = Spec->Type.ToLower();
		const EJson Actual = Pair.Value.IsValid() ? Pair.Value->Type : EJson::Null;
		const bool bTypeOk = (Type == TEXT("string") && Actual == EJson::String)
			|| ((Type == TEXT("bool") || Type == TEXT("boolean")) && Actual == EJson::Boolean)
			|| ((Type == TEXT("number") || Type == TEXT("integer")) && Actual == EJson::Number)
			|| (Type == TEXT("object") && Actual == EJson::Object)
			|| (Type == TEXT("array") && Actual == EJson::Array);
		if (!bTypeOk)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Construction parameter '%s' for node class '%s' has invalid JSON type"), *Name, *NodeClass));
			return false;
		}
	}
	if (Family == TEXT("DynamicCast"))
	{
		FString DeclaredClass;
		const bool bHasClass = (Params->TryGetStringField(TEXT("class"), DeclaredClass)
			|| Params->TryGetStringField(TEXT("target_class"), DeclaredClass)) && !DeclaredClass.IsEmpty();
		if (!bHasClass)
		{
			// The validated patch contract requires the target: a cast without one is a half-built
			// node whose identity can never be verified, so it never reaches apply.
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("DynamicCast requires params.class (alias target_class): a cast without a target class cannot be verified"));
			return false;
		}
		for (const TCHAR* Alias : { TEXT("class"), TEXT("target_class") })
		{
			if (Params->HasField(Alias) && Params->Values.FindRef(Alias)->Type != EJson::String)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("DynamicCast class selector aliases must be strings"));
				return false;
			}
		}
		for (const TCHAR* Alias : { TEXT("is_pure"), TEXT("pure"), TEXT("bIsPureCast") })
		{
			if (Params->HasField(Alias) && Params->Values.FindRef(Alias)->Type != EJson::Boolean)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("DynamicCast purity aliases must be booleans"));
				return false;
			}
		}
	}
	return true;
}

bool ValidateTaggedDefaults(const TSharedPtr<FJsonObject>& Defaults, UEdGraphNode* Node, TSet<FString>& OutDefaultPins, FCortexCommandResult& OutError)
{
	if (!Defaults.IsValid()) return true;
	for (const auto& Pair : Defaults->Values)
	{
		const FString PinName = CortexEngineCompat::JsonKeyToString(Pair.Key);
		const TSharedPtr<FJsonObject> Literal = Pair.Value->AsObject();
		if (!Literal.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Node defaults must be tagged objects"));
			return false;
		}
		UEdGraphPin* Pin = Node ? Node->FindPin(FName(*PinName)) : nullptr;
		if (!Pin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound, FString::Printf(TEXT("Pin '%s' not found for default"), *PinName));
			return false;
		}
		const FString CanonicalPinName = Pin->PinName.ToString();
		if (OutDefaultPins.Contains(CanonicalPinName))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Duplicate default pin"));
			return false;
		}
		OutDefaultPins.Add(CanonicalPinName);
		if (!FCortexGraphPinDefaults::Validate(Pin, Literal, OutError)) return false;
	}
	return true;
}
/** Canonical planned signature of one pin: type, flags, container kind and map terminal type. */
void AddPlannedPinSignature(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NormalizedNode)
{
	if (!Node || !NormalizedNode.IsValid()) return;
	TArray<UEdGraphPin*> Pins;
	for (UEdGraphPin* Pin : Node->Pins) if (Pin) Pins.Add(Pin);
	Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) { return A.PinName.LexicalLess(B.PinName); });
	TArray<TSharedPtr<FJsonValue>> Serialized;
	for (const UEdGraphPin* Pin : Pins)
	{
		Serialized.Add(MakeShared<FJsonValueObject>(MakePinSignatureDescriptor(*Pin)));
	}
	NormalizedNode->SetArrayField(TEXT("resolved_pins"), Serialized);
}
}

bool FCortexGraphPatchOps::HasOnlyFields(
	const TSharedPtr<FJsonObject>& Object,
	const TSet<FString>& Allowed,
	FCortexCommandResult& OutError,
	const FString& Context)
{
	if (!Object.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be an object"), *Context));
		return false;
	}
	for (const auto& Pair : Object->Values)
	{
		const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (!Allowed.Contains(Key))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown field '%s' in %s"), *Key, *Context));
			return false;
		}
	}
	return true;
}

bool FCortexGraphPatchOps::ReadStrictBool(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Field,
	const bool DefaultValue,
	bool& OutValue,
	FCortexCommandResult& OutError)
{
	OutValue = DefaultValue;
	if (!Object.IsValid() || !Object->HasField(Field)) return true;
	// The JSON type is checked explicitly: a boolean flag is never coerced from a string or number,
	// so a malformed flag fails instead of silently selecting the default.
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Boolean)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a boolean"), *Field));
		return false;
	}
	OutValue = Value->AsBool();
	return true;
}

bool FCortexGraphPatchOps::ReadRequiredString(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Field,
	FString& OutValue,
	FCortexCommandResult& OutError)
{
	if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a non-empty string"), *Field));
		return false;
	}
	return true;
}

bool FCortexGraphPatchOps::ParseGuidField(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Field,
	FGuid& OutGuid,
	FCortexCommandResult& OutError)
{
	FString Value;
	if (!ReadRequiredString(Object, Field, Value, OutError) || !FGuid::Parse(Value, OutGuid))
	{
		if (OutError.ErrorCode.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be a valid GUID"), *Field));
		}
		return false;
	}
	return true;
}

bool FCortexGraphPatchOps::ResolveGraphByGuid(
	UBlueprint* Blueprint,
	const FGuid& GraphGuid,
	const FString& SubgraphPath,
	UEdGraph*& OutGraph,
	FCortexCommandResult& OutError)
{
	OutGraph = nullptr;
	TArray<FCortexGraphEntry> Entries;
	FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);
	for (const FCortexGraphEntry& Entry : Entries)
	{
		if (!Entry.Graph || Entry.Graph->GraphGuid != GraphGuid) continue;
		if (!FCortexGraphNodeOps::IsMutableGraphKind(Entry.Kind))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("Target graph is not mutable"));
			return false;
		}
		OutGraph = SubgraphPath.IsEmpty() ? Entry.Graph : FCortexGraphNodeOps::ResolveSubgraph(Entry.Graph, SubgraphPath, OutError);
		return OutGraph != nullptr;
	}
	OutError = FCortexCommandRouter::Error(CortexErrorCodes::GraphNotFound,
		FString::Printf(TEXT("Graph with GUID %s not found"), *GraphGuid.ToString()));
	return false;
}

TSharedPtr<FJsonObject> FCortexGraphPatchOps::MakePinSignatureDescriptor(const UEdGraphPin& Pin)
{
	TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
	Descriptor->SetStringField(TEXT("name"), Pin.PinName.ToString());
	Descriptor->SetNumberField(TEXT("direction"), static_cast<int32>(Pin.Direction));
	Descriptor->SetStringField(TEXT("category"), Pin.PinType.PinCategory.ToString());
	Descriptor->SetStringField(TEXT("subcategory"), Pin.PinType.PinSubCategory.ToString());
	Descriptor->SetStringField(TEXT("subobject"), Pin.PinType.PinSubCategoryObject.IsValid() ? Pin.PinType.PinSubCategoryObject->GetPathName() : FString());
	Descriptor->SetBoolField(TEXT("reference"), Pin.PinType.bIsReference);
	Descriptor->SetBoolField(TEXT("const"), Pin.PinType.bIsConst);
	Descriptor->SetNumberField(TEXT("container_type"), static_cast<int32>(Pin.PinType.ContainerType));
	if (Pin.PinType.ContainerType == EPinContainerType::Map || !Pin.PinType.PinValueType.TerminalCategory.IsNone())
	{
		TSharedPtr<FJsonObject> Terminal = MakeShared<FJsonObject>();
		Terminal->SetStringField(TEXT("category"), Pin.PinType.PinValueType.TerminalCategory.ToString());
		Terminal->SetStringField(TEXT("subcategory"), Pin.PinType.PinValueType.TerminalSubCategory.ToString());
		Terminal->SetStringField(TEXT("subobject"), Pin.PinType.PinValueType.TerminalSubCategoryObject.IsValid() ? Pin.PinType.PinValueType.TerminalSubCategoryObject->GetPathName() : FString());
		Terminal->SetBoolField(TEXT("const"), Pin.PinType.PinValueType.bTerminalIsConst);
		Descriptor->SetObjectField(TEXT("map_terminal_type"), Terminal);
	}
	return Descriptor;
}

FString FCortexGraphPatchOps::CanonicalPinSignature(const TSharedPtr<FJsonObject>& Descriptor)
{
	if (!Descriptor.IsValid()) return FString();
	const TSharedPtr<FJsonObject>* TerminalPtr = nullptr;
	const bool bHasTerminal = Descriptor->TryGetObjectField(TEXT("map_terminal_type"), TerminalPtr)
		&& TerminalPtr && TerminalPtr->IsValid();
	FString Canonical = FString::Printf(TEXT("%s|dir=%d|cat=%s|sub=%s|subobj=%s|ref=%d|const=%d|container=%d"),
		*Descriptor->GetStringField(TEXT("name")),
		Descriptor->GetIntegerField(TEXT("direction")),
		*Descriptor->GetStringField(TEXT("category")),
		*Descriptor->GetStringField(TEXT("subcategory")),
		*Descriptor->GetStringField(TEXT("subobject")),
		Descriptor->GetBoolField(TEXT("reference")) ? 1 : 0,
		Descriptor->GetBoolField(TEXT("const")) ? 1 : 0,
		Descriptor->GetIntegerField(TEXT("container_type")));
	if (bHasTerminal)
	{
		Canonical += FString::Printf(TEXT("|term=%s,%s,%s,%d"),
			*(*TerminalPtr)->GetStringField(TEXT("category")),
			*(*TerminalPtr)->GetStringField(TEXT("subcategory")),
			*(*TerminalPtr)->GetStringField(TEXT("subobject")),
			(*TerminalPtr)->GetBoolField(TEXT("const")) ? 1 : 0);
	}
	return Canonical;
}

bool FCortexGraphPatchOps::Preflight(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	FCortexGraphPreparedPatch& OutPrepared,
	FCortexCommandResult& OutError)
{
	OutPrepared = FCortexGraphPreparedPatch();
	OutError = FCortexCommandResult();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is required"));
		return false;
	}
	const FString CanonicalRequest = CanonicalObject(Params);
	FTCHARToUTF8 RequestUtf8(*CanonicalRequest);
	if (RequestUtf8.Length() > MaxRequestSize)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("normalized request exceeds max_request_size_bytes=65536"));
		return false;
	}
	if (!HasOnlyFields(Params, { TEXT("asset_path"), TEXT("target"), TEXT("patch_id"), TEXT("expected_fingerprint"), TEXT("nodes"), TEXT("connections"), TEXT("pin_updates"), TEXT("dry_run"), TEXT("compile"), TEXT("save"), TEXT("allow_noop"), TEXT("expected_validation_hash"), TEXT("migration") }, OutError, TEXT("patch request"))) return false;
	FString AssetPath;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError) || AssetPath != Blueprint->GetPathName())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("asset_path does not identify the supplied Blueprint"));
		return false;
	}
	if (!Params->HasField(TEXT("patch_id")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("patch_id is required"));
		return false;
	}
	FGuid PatchGuid;
	if (!ParseGuidField(Params, TEXT("patch_id"), PatchGuid, OutError)) return false;
	OutPrepared.PatchId = PatchGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);

	bool bDryRun = true, bCompile = true, bSave = false, bAllowNoop = false;
	if (!ReadStrictBool(Params, TEXT("dry_run"), true, bDryRun, OutError) || !ReadStrictBool(Params, TEXT("compile"), true, bCompile, OutError) || !ReadStrictBool(Params, TEXT("save"), false, bSave, OutError) || !ReadStrictBool(Params, TEXT("allow_noop"), false, bAllowNoop, OutError)) return false;
	OutPrepared.bDryRun = bDryRun;
	OutPrepared.bCompile = bCompile;
	OutPrepared.bSave = bSave;
	if (bDryRun && bSave)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Preview must use save=false"));
		return false;
	}
	if (!bDryRun && bSave && !bCompile)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
		return false;
	}
	if (bSave && Blueprint->GetOutermost()->IsDirty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::DirtyEditorState, TEXT("save=true requires a clean starting package"));
		return false;
	}
	if (bDryRun && Params->HasField(TEXT("expected_validation_hash")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("expected_validation_hash is only valid for apply"));
		return false;
	}
	if (!bDryRun)
	{
		FString ExpectedToken;
		if (!Params->TryGetStringField(TEXT("expected_validation_hash"), ExpectedToken) || ExpectedToken.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, TEXT("Apply requires expected_validation_hash from a preview"));
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* FingerprintPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), FingerprintPtr) || !FingerprintPtr || !FingerprintPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		return false;
	}
	OutPrepared.FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	if (!FCortexGraphPatchState::ValidatePrecondition(*FingerprintPtr, OutPrepared.FingerprintBefore, OutError)) return false;

	// The migration shell shares the envelope, the flags, the fingerprint guard and the validation
	// token with authoring, but never the authoring arrays: a request selects exactly one shell.
	const TSharedPtr<FJsonObject>* MigrationPtr = nullptr;
	if (Params->HasField(TEXT("migration")))
	{
		if (!Params->TryGetObjectField(TEXT("migration"), MigrationPtr) || !MigrationPtr || !MigrationPtr->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
			return false;
		}
		for (const TCHAR* AuthoringField : { TEXT("nodes"), TEXT("connections"), TEXT("pin_updates") })
		{
			if (!Params->HasField(AuthoringField)) continue;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Params->TryGetArrayField(AuthoringField, Array) || !Array)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s must be an array"), AuthoringField));
				return false;
			}
			if (Array->Num() > 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("migration cannot be combined with a non-empty '%s' array; a migration request carries no authoring nodes, connections or pin_updates"), AuthoringField));
				return false;
			}
		}
	}
	if (MigrationPtr != nullptr)
	{
		if (!CountBlueprintNodesBounded(Blueprint))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("graph scan exceeds max_scanned_nodes=2048"));
			return false;
		}
		// The target shape is validated here without resolving the declaration: eligibility (and so
		// the shadowing-member policy) belongs to the migration planner, which reports a same-named
		// app member as this operation's own conflict instead of the generic implementation conflict.
		const TSharedPtr<FJsonObject>* MigrationTargetPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("target"), MigrationTargetPtr) || !MigrationTargetPtr || !MigrationTargetPtr->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target must be an object"));
			return false;
		}
		TSharedPtr<FJsonObject> MigrationTarget = *MigrationTargetPtr;
		if (!HasOnlyFields(MigrationTarget, { TEXT("graph_ref"), TEXT("implementation") }, OutError, TEXT("target"))) return false;
		if (MigrationTarget->HasField(TEXT("graph_ref")) || !MigrationTarget->HasField(TEXT("implementation")))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration requires an implementation target declaring the inherited implementation the entry must become"));
			return false;
		}
		const TSharedPtr<FJsonObject>* ImplementationSelector = nullptr;
		if (!MigrationTarget->TryGetObjectField(TEXT("implementation"), ImplementationSelector)
			|| !ImplementationSelector || !ImplementationSelector->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("target.implementation must be an object"));
			return false;
		}
		if (!HasOnlyFields(*ImplementationSelector, { TEXT("owner_class"), TEXT("function_name"), TEXT("call_kind") }, OutError, TEXT("target.implementation"))) return false;
		FCortexGraphMigrationIdentity Identity;
		Identity.EntryGuid = DerivePlannedNodeGuid(OutPrepared.PatchId, TEXT("entry"));
		Identity.ResultGuid = DerivePlannedNodeGuid(OutPrepared.PatchId, TEXT("result"));
		FCortexGraphMigrationPlan MigrationPlan;
		bool bMigrationReused = false;
		if (!FCortexGraphMigrationOps::Plan(Blueprint, *MigrationPtr, ImplementationSelector ? *ImplementationSelector : nullptr,
			Identity, MigrationPlan, bMigrationReused, OutError))
		{
			return false;
		}
		OutPrepared.GraphGuid = MigrationPlan.GraphGuid;
		OutPrepared.SubgraphPath = MigrationPlan.SubgraphPath;
		OutPrepared.NodeGuidByClientId.Add(TEXT("entry"), MigrationPlan.Identity.EntryGuid);
		if (MigrationPlan.HasResultTerminator())
		{
			OutPrepared.NodeGuidByClientId.Add(TEXT("result"), MigrationPlan.Identity.ResultGuid);
		}
		OutPrepared.EntryNodeGuid = MigrationPlan.Identity.EntryGuid;
		OutPrepared.bHasEntryNode = true;
		OutPrepared.PlannedNodeIds.Add(TEXT("entry"));
		if (MigrationPlan.HasResultTerminator()) OutPrepared.PlannedNodeIds.Add(TEXT("result"));
		OutPrepared.MigrationPlan = MigrationPlan.ToJson();

		TSharedPtr<FJsonObject> Normalized = MakeShared<FJsonObject>();
		Normalized->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Normalized->SetStringField(TEXT("patch_id"), OutPrepared.PatchId);
		Normalized->SetObjectField(TEXT("target"), MigrationTarget);
		Normalized->SetObjectField(TEXT("expected_fingerprint"), *FingerprintPtr);
		TArray<TSharedPtr<FJsonValue>> NormalizedNodes;
		if (MigrationPlan.NormalizedNode.IsValid())
		{
			NormalizedNodes.Add(MakeShared<FJsonValueObject>(MigrationPlan.NormalizedNode));
		}
		Normalized->SetArrayField(TEXT("nodes"), NormalizedNodes);
		Normalized->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
		Normalized->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
		Normalized->SetBoolField(TEXT("compile"), bCompile);
		Normalized->SetBoolField(TEXT("allow_noop"), bAllowNoop);
		Normalized->SetObjectField(TEXT("resolved_symbol"),
			MigrationPlan.ResolvedSymbol.IsValid() ? MigrationPlan.ResolvedSymbol : MakeShared<FJsonObject>());
		Normalized->SetObjectField(TEXT("migration"), OutPrepared.MigrationPlan);
		OutPrepared.NormalizedRequest = Normalized;

		if (bMigrationReused)
		{
			// A complete reuse is an idempotent replay: the whole planned intent and the replacement
			// must already match the live asset before this request may claim no work is needed.
			FString ReuseFailure;
			if (!ComparePlannedIntentAgainstNative(
				Blueprint, OutPrepared, MakePreparedLocators(OutPrepared), false, false, ReuseFailure))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the existing deterministic replacement identity does not match the planned intent: %s"), *ReuseFailure));
				return false;
			}
			if (!FCortexGraphMigrationOps::VerifyReplacementAgainstNative(
				Blueprint, MigrationPlan, false, false, ReuseFailure))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the existing deterministic replacement identity does not match the planned replace_entry: %s"), *ReuseFailure));
				return false;
			}
			OutPrepared.ReusedClientIds.Add(TEXT("entry"));
			if (MigrationPlan.HasResultTerminator()) OutPrepared.ReusedClientIds.Add(TEXT("result"));
			OutPrepared.bFullyReused = true;
		}
		OutPrepared.bChanged = !OutPrepared.bFullyReused;
		OutPrepared.bReplayedWithAbsentSource = MigrationPlan.bReplayedWithAbsentSource;

		FString MigrationIntent;
		MigrationIntent += TEXT("graph_patch_v1|");
		MigrationIntent += CanonicalObject(Normalized);
		MigrationIntent += TEXT("|fingerprint=");
		MigrationIntent += OutPrepared.FingerprintBefore->GetStringField(TEXT("graph_authoring_hash"));
		MigrationIntent += TEXT("|engine=UE5.8|schema=K2");
		FTCHARToUTF8 MigrationUtf8(*MigrationIntent);
		const FIoHash MigrationDigest = FIoHash::HashBuffer(
			reinterpret_cast<const uint8*>(MigrationUtf8.Get()), MigrationUtf8.Length());
		OutPrepared.ValidationHash = LexToString(MigrationDigest);

		if (!bDryRun)
		{
			FString ExpectedToken;
			Params->TryGetStringField(TEXT("expected_validation_hash"), ExpectedToken);
			if (ExpectedToken != OutPrepared.ValidationHash)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
					TEXT("expected_validation_hash does not match current preflight intent"));
				return false;
			}
		}
		return true;
	}

	TSharedPtr<FJsonObject> Target;
	UEdGraph* TargetGraph = nullptr;
	TSharedPtr<FJsonObject> SymbolJson;
	bool bImplementationWouldCreate = false;
	bool bImplementationIsEvent = false;
	bool bImplementationHasParentCall = false;
	if (!CountBlueprintNodesBounded(Blueprint))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("graph scan exceeds max_scanned_nodes=2048"));
		return false;
	}
	if (!ParseTarget(Blueprint, Params, Target, TargetGraph, SymbolJson, bImplementationWouldCreate, bImplementationIsEvent, bImplementationHasParentCall, OutError)) return false;
	if (TargetGraph)
	{
		const TSharedPtr<FJsonObject>* RefPtr = nullptr;
		Target->TryGetObjectField(TEXT("graph_ref"), RefPtr);
		if (RefPtr && RefPtr->IsValid())
		{
			FGuid GraphGuid;
			if (!ParseGuidField(*RefPtr, TEXT("graph_guid"), GraphGuid, OutError)) return false;
			OutPrepared.GraphGuid = GraphGuid.ToString();
			(*RefPtr)->TryGetStringField(TEXT("subgraph_path"), OutPrepared.SubgraphPath);
		}
		else
		{
			// An implementation target writes into the graph that already owns the entry, so its
			// durable graph locator is known before the first mutation.
			OutPrepared.GraphGuid = TargetGraph->GraphGuid.ToString();
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Params->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("nodes must be an array"));
		return false;
	}
	if (Nodes->Num() > MaxNodes)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("nodes exceeds max_nodes=64"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
	if (!Params->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("connections must be an array"));
		return false;
	}
	if (Connections->Num() > MaxEdges)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("connections exceeds max_edges=256"));
		return false;
	}

	TSharedPtr<FJsonObject> Normalized = MakeShared<FJsonObject>();
	Normalized->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Normalized->SetStringField(TEXT("patch_id"), OutPrepared.PatchId);
	Normalized->SetObjectField(TEXT("target"), Target);
	Normalized->SetObjectField(TEXT("expected_fingerprint"), *FingerprintPtr);
	TArray<TSharedPtr<FJsonValue>> NormalizedNodes;
	TSet<FString> ClientIds;
	/** Owner of every derived GUID in this request, used to refuse intra-request collisions. */
	TMap<FGuid, FString> DerivedGuidOwners;
	/** Client ids whose deterministic node does not exist yet and must therefore be created. */
	TArray<FString> CreatedClientIds;
	TMap<FString, UEdGraphNode*> PlannedNodes;
	TMap<FString, TSet<FString>> DefaultPins;
	UBlueprint* PlanningBlueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	PlanningBlueprint->ParentClass = Blueprint->ParentClass;
	PlanningBlueprint->GeneratedClass = Blueprint->GeneratedClass;
	PlanningBlueprint->SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass;
	UEdGraph* PlanningGraph = NewObject<UEdGraph>(PlanningBlueprint, NAME_None, RF_Transient);
	PlanningGraph->Schema = UEdGraphSchema_K2::StaticClass();
	if (bImplementationIsEvent)
	{
		PlanningBlueprint->UbergraphPages.Add(PlanningGraph);
	}
	else
	{
		PlanningBlueprint->FunctionGraphs.Add(PlanningGraph);
	}
	bool bNeedsExistingModel = Params->HasField(TEXT("pin_updates"));
	if (!bNeedsExistingModel)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Connections)
		{
			const TSharedPtr<FJsonObject> Connection = Value->AsObject();
			const TSharedPtr<FJsonObject>* Endpoint = nullptr;
			if (Connection.IsValid()
				&& ((Connection->TryGetObjectField(TEXT("from"), Endpoint) && Endpoint && (*Endpoint)->HasField(TEXT("node_guid")))
					|| (Connection->TryGetObjectField(TEXT("to"), Endpoint) && Endpoint && (*Endpoint)->HasField(TEXT("node_guid")))))
			{
				bNeedsExistingModel = true;
				break;
			}
		}
	}
	if (bNeedsExistingModel)
	{
		CloneExistingGraphIntoPlanningGraph(TargetGraph, PlanningGraph, PlannedNodes);
	}
	const bool bImplementationTarget = Target->HasField(TEXT("implementation"));
	if (bImplementationTarget)
	{
		const TSharedPtr<FJsonObject>* SelectorPtr = nullptr;
		Target->TryGetObjectField(TEXT("implementation"), SelectorPtr);
		FCortexGraphImplementationPlan ImplementationPlan;
		if (!SelectorPtr || !SelectorPtr->IsValid() || !FCortexGraphImplementationOps::ValidateEligibility(Blueprint, *SelectorPtr, ImplementationPlan, OutError))
		{
			return false;
		}
		if (ImplementationPlan.ExistingEntryNode)
		{
			if (!PlannedNodes.Contains(ImplementationPlan.ExistingEntryNode->NodeGuid.ToString()))
			{
				CloneExistingGraphIntoPlanningGraph(TargetGraph, PlanningGraph, PlannedNodes);
			}
			UEdGraphNode* const* EntryClone = PlannedNodes.Find(ImplementationPlan.ExistingEntryNode->NodeGuid.ToString());
			if (!EntryClone)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Failed to model existing implementation entry node"));
				return false;
			}
			PlannedNodes.Add(TEXT("entry"), *EntryClone);
			OutPrepared.EntryNodeGuid = ImplementationPlan.ExistingEntryNode->NodeGuid;
			OutPrepared.bHasEntryNode = true;
		}
		else if (bImplementationIsEvent)
		{
			UK2Node_Event* EventNode = NewObject<UK2Node_Event>(PlanningGraph);
			EventNode->CreateNewGuid();
			EventNode->EventReference.SetExternalMember(ImplementationPlan.Function->GetFName(), ImplementationPlan.FunctionClass);
			EventNode->bOverrideFunction = true;
			EventNode->AllocateDefaultPins();
			PlanningGraph->AddNode(EventNode, false, false);
			PlannedNodes.Add(TEXT("entry"), EventNode);
		}
		else
		{
			GetDefault<UEdGraphSchema_K2>()->CreateFunctionGraphTerminators(*PlanningGraph, ImplementationPlan.Function);
			for (UEdGraphNode* Node : PlanningGraph->Nodes)
			{
				if (Cast<UK2Node_FunctionEntry>(Node))
				{
					PlannedNodes.Add(TEXT("entry"), Node);
					break;
				}
			}
		}
	}
	UEdGraph* const CompatibilityGraph = TargetGraph ? TargetGraph : PlanningGraph;

	for (int32 Index = 0; Index < Nodes->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Node = (*Nodes)[Index]->AsObject();
		if (!Node.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("nodes entries must be objects"));
			return false;
		}
		TSharedPtr<FJsonObject> NormalizedNode;
		if (!AddNormalizedNode(Node, NormalizedNode, OutError)) return false;
		const FString ClientId = NormalizedNode->GetStringField(TEXT("client_id"));
		if (ClientIds.Contains(ClientId))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Duplicate node client_id '%s'"), *ClientId));
			return false;
		}
		if (ClientId.Equals(TEXT("entry"), ESearchCase::CaseSensitive))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("node.client_id 'entry' is reserved"));
			return false;
		}
		ClientIds.Add(ClientId);
		const FString NodeClass = NormalizedNode->GetStringField(TEXT("node_class"));
		FName Family;
		UClass* ResolvedNodeClass = nullptr;
		if (!FCortexGraphNodeContract::ResolveFamily(NodeClass, Family, ResolvedNodeClass) || !ResolvedNodeClass)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Unsupported node class '%s'"), *NodeClass));
			return false;
		}
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		if (NormalizedNode->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr && ParamsPtr->IsValid()) NodeParams = *ParamsPtr;
		if (!ValidateConstructionParamShape(NodeClass, NodeParams, OutError)) return false;
		if (!FCortexGraphNodeContract::Validate(NodeClass, Blueprint, NodeParams, OutError)) return false;
		const FGuid DerivedGuid = DerivePlannedNodeGuid(OutPrepared.PatchId, ClientId);
		if (const FString* OtherClientId = DerivedGuidOwners.Find(DerivedGuid))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("client_id '%s' and client_id '%s' derive the same deterministic node GUID %s"),
					*ClientId, **OtherClientId, *DerivedGuid.ToString()));
			return false;
		}
		DerivedGuidOwners.Add(DerivedGuid, ClientId);
		OutPrepared.NodeGuidByClientId.Add(ClientId, DerivedGuid);
		UEdGraphNode* PlannedNode = NewObject<UEdGraphNode>(PlanningGraph, ResolvedNodeClass, NAME_None, RF_Transient);
		PlannedNode->NodeGuid = DerivedGuid;
		FString ApplyError;
		if (!FCortexGraphNodeContract::ApplyNodeConstructionParams(PlanningGraph, PlannedNode, Blueprint, NodeParams, ApplyError))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ApplyError);
			return false;
		}
		if (PlannedNode->Pins.Num() == 0) PlannedNode->AllocateDefaultPins();
		if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(PlannedNode))
		{
			Composite->PostPlacedNewNode();
			Composite->AllocateDefaultPins();
		}
		if (!PlannedNode->IsCompatibleWithGraph(CompatibilityGraph) || !PlannedNode->CanPasteHere(CompatibilityGraph))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Node class '%s' is incompatible with the selected graph"), *NodeClass));
			return false;
		}
		AddPlannedPinSignature(PlannedNode, NormalizedNode);

		// Deterministic identity reconciliation: an identity that already exists in the target graph
		// is reused when its native state matches the canonical planned intent and refused when any
		// dimension conflicts. An identity that exists in another graph of the same asset is a
		// cross-graph collision and is always refused, because existing nodes are never overwritten.
		UEdGraphNode* ExistingNode = nullptr;
		TArray<UEdGraph*> OwningGraphs;
		FindGraphsOwningNodeGuid(Blueprint, DerivedGuid, OwningGraphs, ExistingNode);
		if (OwningGraphs.Num() > 1)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("client_id '%s' derives node GUID %s which is owned by %d graphs of this asset; a deterministic node identity must be unique inside the asset"),
					*ClientId, *DerivedGuid.ToString(), OwningGraphs.Num()));
			return false;
		}
		if (ExistingNode && OwningGraphs[0] != TargetGraph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("client_id '%s' derives node GUID %s which already exists in graph '%s'; a deterministic node identity must not collide with a node in another graph"),
					*ClientId, *DerivedGuid.ToString(), OwningGraphs[0] ? *OwningGraphs[0]->GetName() : TEXT("<unknown>")));
			return false;
		}
		if (ExistingNode)
		{
			FReadbackFaultState ReuseFaults;
			FString ReuseFailure;
			if (!ComparePlannedNodeAgainstNative(Blueprint, NormalizedNode, ExistingNode, ReuseFaults, ReuseFailure))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("client_id '%s' already resolves to node GUID %s but its native state conflicts with the planned intent: %s"),
						*ClientId, *DerivedGuid.ToString(), *ReuseFailure));
				return false;
			}
			OutPrepared.ReusedClientIds.Add(ClientId);
		}
		else
		{
			CreatedClientIds.Add(ClientId);
		}

		PlanningGraph->AddNode(PlannedNode, false, false);
		TSet<FString> Pins;
		const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
		if (NormalizedNode->TryGetObjectField(TEXT("defaults"), DefaultsPtr) && DefaultsPtr && DefaultsPtr->IsValid())
		{
			if (!ValidateTaggedDefaults(*DefaultsPtr, PlannedNode, Pins, OutError)) return false;
		}
		DefaultPins.Add(ClientId, MoveTemp(Pins));
		PlannedNodes.Add(ClientId, PlannedNode);
		OutPrepared.PlannedNodeIds.Add(ClientId);
		NormalizedNodes.Add(MakeShared<FJsonValueObject>(NormalizedNode));
	}

	// Identity-set refusals are decided by the planned node identities alone and are reported before
	// any connection-level check could answer with a less specific reason: a partial identity set is
	// refused instead of appended to, and a complete identity set is refused when the implementation
	// entry would still have to be created next to it.
	if (OutPrepared.ReusedClientIds.Num() > 0 && CreatedClientIds.Num() > 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("patch identity set is partial: client_id(s) %s already exist exactly while client_id(s) %s are missing; a patch must be entirely new or a complete replay"),
				*FString::Join(OutPrepared.ReusedClientIds, TEXT(", ")), *FString::Join(CreatedClientIds, TEXT(", "))));
		return false;
	}
	if (OutPrepared.ReusedClientIds.Num() > 0 && bImplementationWouldCreate)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("client_id(s) %s already exist exactly while the implementation target still has to be created; a patch must be entirely new or a complete replay"),
				*FString::Join(OutPrepared.ReusedClientIds, TEXT(", "))));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> NormalizedPinUpdates;
	TSet<FString> ExistingDefaultInputs;
	const TArray<TSharedPtr<FJsonValue>>* PinUpdates = nullptr;
	if (Params->HasField(TEXT("pin_updates")))
	{
		if (!Params->TryGetArrayField(TEXT("pin_updates"), PinUpdates) || !PinUpdates)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("pin_updates must be an array"));
			return false;
		}
		if (PinUpdates->Num() > MaxNodes)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded, TEXT("pin_updates exceeds the bounded node limit"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *PinUpdates)
		{
			const TSharedPtr<FJsonObject> Update = Value->AsObject();
			if (!Update.IsValid() || !HasOnlyFields(Update, { TEXT("node_guid"), TEXT("pin"), TEXT("default"), TEXT("value") }, OutError, TEXT("pin_update"))) return false;
			FGuid NodeGuid;
			if (!ParseGuidField(Update, TEXT("node_guid"), NodeGuid, OutError)) return false;
			FString PinName;
			if (!ReadRequiredString(Update, TEXT("pin"), PinName, OutError)) return false;
			const bool bDefault = Update->HasField(TEXT("default"));
			const bool bValue = Update->HasField(TEXT("value"));
			if (bDefault == bValue)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("pin_update requires exactly one of default or value"));
				return false;
			}
			UEdGraphPin* Pin = FindPlannedPin(PlannedNodes, NodeGuid.ToString(), PinName, nullptr, OutError);
			if (!Pin) return false;
			const FString InputKey = NodeGuid.ToString() + TEXT(".") + Pin->PinName.ToString();
			if (Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() > 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("pin_update target must be an unconnected input"));
				return false;
			}
			const TSharedPtr<FJsonObject>* LiteralPtr = nullptr;
			if (!Update->TryGetObjectField(bDefault ? TEXT("default") : TEXT("value"), LiteralPtr) || !LiteralPtr || !LiteralPtr->IsValid())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("pin_update default/value must be a tagged object"));
				return false;
			}
			if (!FCortexGraphPinDefaults::Validate(Pin, *LiteralPtr, OutError)) return false;
			if (ExistingDefaultInputs.Contains(InputKey))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Duplicate pin_update target"));
				return false;
			}
			ExistingDefaultInputs.Add(InputKey);
			TSharedPtr<FJsonObject> NormalizedUpdate = MakeShared<FJsonObject>();
			NormalizedUpdate->SetStringField(TEXT("node_guid"), Update->GetStringField(TEXT("node_guid")));
			NormalizedUpdate->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			NormalizedUpdate->SetObjectField(bDefault ? TEXT("default") : TEXT("value"), *LiteralPtr);
			NormalizedUpdate->SetObjectField(TEXT("resolved_pin"), MakePinSignatureDescriptor(*Pin));
			NormalizedPinUpdates.Add(MakeShared<FJsonValueObject>(NormalizedUpdate));
		}
	}
	Normalized->SetArrayField(TEXT("pin_updates"), NormalizedPinUpdates);
	Normalized->SetArrayField(TEXT("nodes"), NormalizedNodes);

	TArray<TSharedPtr<FJsonValue>> NormalizedConnections;
	TSet<FString> ConnectedInputs;
	for (int32 Index = 0; Index < Connections->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Connection = (*Connections)[Index]->AsObject();
		if (!Connection.IsValid() || !HasOnlyFields(Connection, { TEXT("from"), TEXT("to") }, OutError, TEXT("connection"))) return false;
		const TSharedPtr<FJsonObject>* FromPtr = nullptr;
		const TSharedPtr<FJsonObject>* ToPtr = nullptr;
		if (!Connection->TryGetObjectField(TEXT("from"), FromPtr) || !FromPtr || !FromPtr->IsValid() || !Connection->TryGetObjectField(TEXT("to"), ToPtr) || !ToPtr || !ToPtr->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("connection requires from and to objects"));
			return false;
		}
		FString FromId, FromPinName, ToId, ToPinName;
		bool bFromEntry = false;
		bool bToEntry = false;
		if (!ParseEndpoint(*FromPtr, FromId, FromPinName, bFromEntry, OutError, TEXT("connection.from")) || !ParseEndpoint(*ToPtr, ToId, ToPinName, bToEntry, OutError, TEXT("connection.to"))) return false;
		if (FromId != FromId.TrimStartAndEnd() || ToId != ToId.TrimStartAndEnd())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("connection endpoint identity cannot contain whitespace"));
			return false;
		}
		if ((bFromEntry || bToEntry) && (!bImplementationTarget || bImplementationHasParentCall))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				bImplementationHasParentCall ? TEXT("entry endpoints are not supported for explicit parent-call implementations") : TEXT("entry endpoints require an implementation target"));
			return false;
		}
		if ((*FromPtr)->HasField(TEXT("client_id")) && !ClientIds.Contains(FromId))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Forward or unknown source client_id '%s'"), *FromId));
			return false;
		}
		if ((*ToPtr)->HasField(TEXT("client_id")) && !ClientIds.Contains(ToId))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Forward or unknown target client_id '%s'"), *ToId));
			return false;
		}
		UEdGraphPin* SourcePin = FindPlannedPin(PlannedNodes, FromId, FromPinName, nullptr, OutError);
		UEdGraphPin* TargetPin = FindPlannedPin(PlannedNodes, ToId, ToPinName, nullptr, OutError);
		if (!SourcePin || !TargetPin) return false;
		if (SourcePin->Direction != EGPD_Output || TargetPin->Direction != EGPD_Input)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Connections must be output-to-input"));
			return false;
		}
		const FString InputKey = ToId + TEXT(".") + TargetPin->PinName.ToString();
		bool bHasPlannedDefault = false;
		for (const FString& DefaultPin : DefaultPins.FindRef(ToId))
		{
			if (DefaultPin.Equals(ToPinName, ESearchCase::IgnoreCase))
			{
				bHasPlannedDefault = true;
				break;
			}
		}
		if (ConnectedInputs.Contains(InputKey) || ExistingDefaultInputs.Contains(InputKey) || bHasPlannedDefault)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Input '%s' has competing connection/default"), *InputKey));
			return false;
		}
		// Replay edges: when every planned node identity already exists exactly, the planning graph
		// may already model the native link through its cloned proxies, so re-creating the link here
		// would look like replacing a competing native link. The exact native edge is verified
		// against the live asset by the reuse comparison that decides this request is a replay.
		const bool bReplayEdge = CreatedClientIds.Num() == 0 && OutPrepared.ReusedClientIds.Num() > 0
			&& (!bFromEntry || OutPrepared.bHasEntryNode) && (!bToEntry || OutPrepared.bHasEntryNode);
		if (!bReplayEdge)
		{
			const UEdGraphSchema* Schema = PlanningGraph->GetSchema();
			if (Schema)
			{
				const FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
				if (Response.Response != CONNECT_RESPONSE_MAKE)
				{
					OutError = FCortexCommandRouter::Error(Response.Response == CONNECT_RESPONSE_DISALLOW ? CortexErrorCodes::PinTypeMismatch : CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Schema rejected connection: %s"), *Response.Message.ToString()));
					return false;
				}
			}
			if (TargetPin->LinkedTo.Num() > 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Input '%s' has competing connection/default"), *InputKey));
				return false;
			}
			if (Schema && !Schema->TryCreateConnection(SourcePin, TargetPin))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Transient schema connection failed"));
				return false;
			}
		}
		ConnectedInputs.Add(InputKey);
		TSharedPtr<FJsonObject> NormalizedConnection = MakeShared<FJsonObject>();
		NormalizedConnection->SetObjectField(TEXT("from"), *FromPtr);
		NormalizedConnection->SetObjectField(TEXT("to"), *ToPtr);
		NormalizedConnections.Add(MakeShared<FJsonValueObject>(NormalizedConnection));
		OutPrepared.PlannedConnectionKeys.Add(FromId + TEXT(".") + FromPinName + TEXT("->") + InputKey);
	}
	Normalized->SetArrayField(TEXT("connections"), NormalizedConnections);
	Normalized->SetBoolField(TEXT("compile"), bCompile);
	Normalized->SetBoolField(TEXT("allow_noop"), bAllowNoop);
	Normalized->SetObjectField(TEXT("resolved_symbol"), SymbolJson.IsValid() ? SymbolJson : MakeShared<FJsonObject>());
	OutPrepared.NormalizedRequest = Normalized;

	// A complete reuse match is an idempotent replay rather than an empty request: it must prove the
	// whole planned intent against the live asset before it may claim no work is needed.
	if (OutPrepared.ReusedClientIds.Num() > 0)
	{
		// The reuse match is proven without the compiled-class check: an idempotent replay never
		// compiles, so it can only claim the authoring intent, not the compiled artifact.
		FString ReuseFailure;
		if (!ComparePlannedIntentAgainstNative(
			Blueprint, OutPrepared, MakePreparedLocators(OutPrepared), false, false, ReuseFailure))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the existing deterministic identity set does not match the planned intent: %s"), *ReuseFailure));
			return false;
		}
		OutPrepared.bFullyReused = true;
	}

	const bool bHasPlannedIntent = NormalizedNodes.Num() > 0 || NormalizedConnections.Num() > 0
		|| NormalizedPinUpdates.Num() > 0 || bImplementationWouldCreate;
	OutPrepared.bChanged = bHasPlannedIntent && !OutPrepared.bFullyReused;
	if (TargetGraph && !OutPrepared.bChanged && !bAllowNoop && !bHasPlannedIntent)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("graph patch has no prospective change; set allow_noop=true for an idempotent request"));
		return false;
	}

	FString Intent;
	Intent += TEXT("graph_patch_v1|");
	Intent += CanonicalObject(Normalized);
	Intent += TEXT("|fingerprint=");
	Intent += OutPrepared.FingerprintBefore->GetStringField(TEXT("graph_authoring_hash"));
	Intent += TEXT("|engine=UE5.8|schema=K2");
	FTCHARToUTF8 Utf8(*Intent);
	const FIoHash Digest = FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	OutPrepared.ValidationHash = LexToString(Digest);

	if (!bDryRun)
	{
		FString ExpectedToken;
		Params->TryGetStringField(TEXT("expected_validation_hash"), ExpectedToken);
		if (ExpectedToken != OutPrepared.ValidationHash)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, TEXT("expected_validation_hash does not match current preflight intent"));
			return false;
		}
	}
	return true;
}


namespace
{
constexpr int32 MaxStoredDiagnostics = 16;
constexpr int32 MaxDiagnosticLength = 512;

/**
 * Journal of one prepared apply. Every entry is a durable identity (GUID, pin name, literal) so
 * recovery keeps working after a target compile has reconstructed nodes and pins.
 */
struct FGraphPatchJournal
{
	TUniquePtr<FScopedTransaction> Transaction;
	bool bPackageWasDirty = false;
	EBlueprintStatus StatusBefore = BS_Unknown;
	FString GeneratedStateBefore;
	TArray<UEdGraph*> AddedGraphs;
	TArray<FGuid> AddedNodeGuids;

	struct FDefaultEntry
	{
		FGuid NodeGuid;
		FName PinName;
		FString PriorDefaultValue;
		FString PriorDefaultObjectPath;
		bool bPriorDefaultObjectSet = false;
		FText PriorDefaultTextValue;
	};
	TArray<FDefaultEntry> Defaults;

	struct FLinkEntry
	{
		FGuid SourceNodeGuid;
		FName SourcePinName;
		FGuid TargetNodeGuid;
		FName TargetPinName;
	};
	TArray<FLinkEntry> Links;

	struct FNodeStateEntry
	{
		FGuid NodeGuid;
		FString Comment;
		ENodeEnabledState EnabledState = ENodeEnabledState::Enabled;
		bool bUserSetEnabledState = false;
		bool bForceDisplayAsDisabled = false;
		bool bCommentBubblePinned = false;
		bool bCommentBubbleVisible = false;
	};
	/** Pre-request editor state of touched existing nodes, which engine paths derive from links. */
	TArray<FNodeStateEntry> NodeStates;

	struct FPinLinkTarget
	{
		FGuid NodeGuid;
		FName PinName;
	};

	struct FPinStateEntry
	{
		FName PinName;
		EEdGraphPinDirection Direction = EGPD_Input;
		FEdGraphPinType PinType;
		FString DefaultValue;
		FString DefaultObjectPath;
		FText DefaultTextValue;
		bool bSplitChild = false;
		TArray<FPinLinkTarget> LinkedTo;
	};

	/**
	 * Complete pre-request pin identity of one existing node. A class-pin default rebuilds the
	 * class-dependent pin set, so recovery has to replay that lifecycle and then restore the exact
	 * pin types, defaults and durable links instead of only the changed pin's default.
	 */
	struct FNodePinSnapshot
	{
		FGuid NodeGuid;
		TArray<FPinStateEntry> Pins;
	};
	TArray<FNodePinSnapshot> NodePins;

	/**
	 * One terminator node the migration detached from its graph. The node object stays alive here so
	 * its identity (GUID and object name) survives, and the pin snapshot carries the state that the
	 * detach broke: defaults, pin types and durable links back to the preserved downstream nodes.
	 */
	struct FRemovedNodeEntry
	{
		FGuid NodeGuid;
		FGuid GraphGuid;
		int32 NodePosX = 0;
		int32 NodePosY = 0;
		FString NodeComment;
		bool bCommentBubblePinned = false;
		bool bCommentBubbleVisible = false;
		int32 EnabledState = 0;
		bool bUserSetEnabledState = false;
		bool bForceDisplayAsDisabled = false;
		bool bOverrideFunction = false;
		FName MemberName = NAME_None;
		UClass* MemberParentClass = nullptr;
		UEdGraphNode* Node = nullptr;
		FNodePinSnapshot Snapshot;
	};
	TArray<FRemovedNodeEntry> RemovedNodes;

	/** One shadowing member removed as part of a migration, journaled with its exact description. */
	struct FMemberEntry
	{
		FName Name = NAME_None;
		int32 Index = INDEX_NONE;
		TSharedPtr<FJsonObject> Captured;
		FString Diagnostic;
	};
	bool bMemberCaptured = false;
	FMemberEntry Member;

	/**
	 * Preservation contract of a migration: the canonical capture of every node outside the replaced
	 * set, verified again after recovery so a restored body is proven, not assumed.
	 */
	bool bVerifyPreservation = false;
	FGuid PreservationGraphGuid;
	TArray<FGuid> PreservationExcludedGuids;
	FString PreservationBefore;

	FCortexGraphPatchLocators Locators;
	bool bCompileAttempted = false;

	bool WasNodeCreated(const FGuid& NodeGuid) const { return AddedNodeGuids.Contains(NodeGuid); }

	bool WasPinDefaulted(const FGuid& NodeGuid, const FName PinName) const
	{
		for (const FDefaultEntry& Entry : Defaults)
		{
			if (Entry.NodeGuid == NodeGuid && Entry.PinName == PinName) return true;
		}
		return false;
	}
};

/**
 * Canonical pre-request pin identity of one node: names, directions, types, defaults and durable
 * links. One implementation serves the default-mutation journal and the removal journal.
 */
FGraphPatchJournal::FNodePinSnapshot MakeNodePinSnapshot(UEdGraphNode* Node)
{
	FGraphPatchJournal::FNodePinSnapshot Snapshot;
	if (!Node || !Node->NodeGuid.IsValid()) return Snapshot;
	Snapshot.NodeGuid = Node->NodeGuid;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		FGraphPatchJournal::FPinStateEntry Entry;
		Entry.PinName = Pin->PinName;
		Entry.Direction = Pin->Direction;
		Entry.PinType = Pin->PinType;
		Entry.DefaultValue = Pin->DefaultValue;
		Entry.DefaultObjectPath = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString();
		Entry.DefaultTextValue = Pin->DefaultTextValue;
		Entry.bSplitChild = Pin->ParentPin != nullptr;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			if (!LinkedNode || !LinkedNode->NodeGuid.IsValid()) continue;
			Entry.LinkedTo.Add({ LinkedNode->NodeGuid, LinkedPin->PinName });
		}
		Snapshot.Pins.Add(MoveTemp(Entry));
	}
	return Snapshot;
}

/**
 * Journals the complete pin identity of an existing node the first time one of its defaults is
 * changed. Later mutations of the same node stay outside the snapshot so it stays pre-request
 * state even when the first change already rebuilt the pin set.
 */
void JournalNodePins(UEdGraphNode* Node, FGraphPatchJournal& Journal)
{
	if (!Node || !Node->NodeGuid.IsValid()) return;
	for (const FGraphPatchJournal::FNodePinSnapshot& Existing : Journal.NodePins)
	{
		if (Existing.NodeGuid == Node->NodeGuid) return;
	}
	Journal.NodePins.Add(MakeNodePinSnapshot(Node));
}

/**
 * Journals the complete pre-removal identity of a node the migration detaches. The object itself is
 * kept alive by the journal, so recovery re-registers the same node and its identity (including the
 * object name the authoring fingerprint compares) is restored exactly.
 */
void JournalNodeRemoved(UEdGraphNode* Node, FGraphPatchJournal& Journal)
{
	if (!Node || !Node->NodeGuid.IsValid()) return;
	for (const FGraphPatchJournal::FRemovedNodeEntry& Existing : Journal.RemovedNodes)
	{
		if (Existing.NodeGuid == Node->NodeGuid) return;
	}
	FGraphPatchJournal::FRemovedNodeEntry Entry;
	Entry.NodeGuid = Node->NodeGuid;
	Entry.GraphGuid = Node->GetGraph() ? Node->GetGraph()->GraphGuid : FGuid();
	Entry.NodePosX = Node->NodePosX;
	Entry.NodePosY = Node->NodePosY;
	Entry.NodeComment = Node->NodeComment;
	Entry.bCommentBubblePinned = Node->bCommentBubblePinned;
	Entry.bCommentBubbleVisible = Node->bCommentBubbleVisible;
	Entry.EnabledState = static_cast<int32>(Node->GetDesiredEnabledState());
	Entry.bUserSetEnabledState = Node->HasUserSetTheEnabledState();
	Entry.bForceDisplayAsDisabled = Node->IsDisplayAsDisabledForced();
	if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
	{
		Entry.bOverrideFunction = Event->bOverrideFunction;
		Entry.MemberName = Event->EventReference.GetMemberName();
		Entry.MemberParentClass = Event->EventReference.GetMemberParentClass();
	}
	else if (const UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
	{
		Entry.MemberName = FunctionEntry->FunctionReference.GetMemberName();
		Entry.MemberParentClass = FunctionEntry->FunctionReference.GetMemberParentClass();
	}
	else if (const UK2Node_FunctionResult* FunctionResult = Cast<UK2Node_FunctionResult>(Node))
	{
		Entry.MemberName = FunctionResult->FunctionReference.GetMemberName();
		Entry.MemberParentClass = FunctionResult->FunctionReference.GetMemberParentClass();
	}
	else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
	{
		Entry.MemberName = Variable->VariableReference.GetMemberName();
		Entry.MemberParentClass = Variable->VariableReference.GetMemberParentClass();
	}
	Entry.Node = Node;
	Entry.Snapshot = MakeNodePinSnapshot(Node);
	Journal.RemovedNodes.Add(MoveTemp(Entry));
}

bool RestoreNodePinState(
	UBlueprint* Blueprint,
	const FGraphPatchJournal& Journal,
	const FGraphPatchJournal::FNodePinSnapshot& Snapshot);

/**
 * Re-registers every detached node in its original graph and restores its complete state. Existing
 * nodes are never destroyed, so the restored node keeps its object identity and one authoring
 * fingerprint comparison proves the whole body came back.
 */
bool RestoreRemovedNodes(UBlueprint* Blueprint, const FGraphPatchJournal& Journal)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (int32 Index = Journal.RemovedNodes.Num() - 1; Index >= 0; --Index)
	{
		const FGraphPatchJournal::FRemovedNodeEntry& Entry = Journal.RemovedNodes[Index];
		UEdGraph* Graph = nullptr;
		for (UEdGraph* Candidate : Graphs)
		{
			if (Candidate && Candidate->GraphGuid == Entry.GraphGuid)
			{
				Graph = Candidate;
				break;
			}
		}
		if (!Graph) return false;
		UEdGraphNode* Node = nullptr;
		for (UEdGraphNode* Candidate : Graph->Nodes)
		{
			if (Candidate && Candidate->NodeGuid == Entry.NodeGuid)
			{
				Node = Candidate;
				break;
			}
		}
		if (!Node)
		{
			if (!IsValid(Entry.Node)) return false;
			Node = Entry.Node;
			Graph->AddNode(Node, false, false);
		}
		Node->Modify();
		Node->NodeGuid = Entry.NodeGuid;
		Node->NodePosX = Entry.NodePosX;
		Node->NodePosY = Entry.NodePosY;
		Node->NodeComment = Entry.NodeComment;
		Node->bCommentBubblePinned = Entry.bCommentBubblePinned;
		Node->bCommentBubbleVisible = Entry.bCommentBubbleVisible;
		Node->SetEnabledState(static_cast<ENodeEnabledState>(Entry.EnabledState), Entry.bUserSetEnabledState);
		Node->SetForceDisplayAsDisabled(Entry.bForceDisplayAsDisabled);
		if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
		{
			Event->bOverrideFunction = Entry.bOverrideFunction;
			if (Entry.MemberParentClass)
			{
				Event->EventReference.SetExternalMember(Entry.MemberName, Entry.MemberParentClass);
			}
			else if (!Entry.MemberName.IsNone())
			{
				Event->EventReference.SetSelfMember(Entry.MemberName);
			}
		}
		else if (UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
		{
			if (Entry.MemberParentClass) FunctionEntry->FunctionReference.SetExternalMember(Entry.MemberName, Entry.MemberParentClass);
		}
		else if (UK2Node_FunctionResult* FunctionResult = Cast<UK2Node_FunctionResult>(Node))
		{
			if (Entry.MemberParentClass) FunctionResult->FunctionReference.SetExternalMember(Entry.MemberName, Entry.MemberParentClass);
		}
		else if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			if (Entry.MemberParentClass) Variable->VariableReference.SetExternalMember(Entry.MemberName, Entry.MemberParentClass);
		}
		if (!RestoreNodePinState(Blueprint, Journal, Entry.Snapshot)) return false;
		Graph->NotifyGraphChanged();
	}
	return true;
}

/** Re-inserts the shadowing member a migration removed, with its exact captured description. */
bool RestoreRemovedMember(UBlueprint* Blueprint, FGraphPatchJournal& Journal)
{
	if (!Journal.bMemberCaptured) return true;
	FCortexCommandResult MemberError;
	if (!FCortexGraphMigrationOps::RestoreShadowingMember(Blueprint, Journal.Member.Captured, Journal.Member.Index, MemberError))
	{
		return false;
	}
	// The authoring fingerprint cannot see rep-notify, replication, metadata or the remaining pin-type
	// flags, so restoration is proven field by field here; a lost field is an unverified recovery.
	FString MemberFailure;
	if (!FCortexGraphMigrationOps::MemberMatchesCapture(Blueprint, Journal.Member.Name, Journal.Member.Captured, MemberFailure))
	{
		Journal.Member.Diagnostic = MemberFailure;
		return false;
	}
	return true;
}

/**
 * Journals a pin's complete native default state. A tagged literal descriptor cannot represent an
 * unset default exactly (for example an empty integer pin reads back as 0), so the raw state is
 * captured instead of a lossy round-trip.
 */
void JournalPinDefault(UEdGraphPin* Pin, FGraphPatchJournal& Journal)
{
	if (!Pin) return;
	FGraphPatchJournal::FDefaultEntry Entry;
	const UEdGraphNode* OwningNode = Pin->GetOwningNode();
	Entry.NodeGuid = OwningNode ? OwningNode->NodeGuid : FGuid();
	Entry.PinName = Pin->PinName;
	Entry.PriorDefaultValue = Pin->DefaultValue;
	Entry.PriorDefaultTextValue = Pin->DefaultTextValue;
	Entry.bPriorDefaultObjectSet = Pin->DefaultObject != nullptr;
	Entry.PriorDefaultObjectPath = Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString();
	Journal.Defaults.Add(MoveTemp(Entry));
	if (!Journal.WasNodeCreated(Entry.NodeGuid))
	{
		JournalNodePins(Pin->GetOwningNode(), Journal);
	}
}

void JournalNodeState(UEdGraphNode* Node, FGraphPatchJournal& Journal)
{
	if (!Node || !Node->NodeGuid.IsValid()) return;
	for (const FGraphPatchJournal::FNodeStateEntry& Existing : Journal.NodeStates)
	{
		if (Existing.NodeGuid == Node->NodeGuid) return;
	}
	FGraphPatchJournal::FNodeStateEntry Entry;
	Entry.NodeGuid = Node->NodeGuid;
	Entry.Comment = Node->NodeComment;
	Entry.EnabledState = Node->GetDesiredEnabledState();
	Entry.bUserSetEnabledState = Node->HasUserSetTheEnabledState();
	Entry.bForceDisplayAsDisabled = Node->IsDisplayAsDisabledForced();
	Entry.bCommentBubblePinned = Node->bCommentBubblePinned;
	Entry.bCommentBubbleVisible = Node->bCommentBubbleVisible;
	Journal.NodeStates.Add(MoveTemp(Entry));
}

void RestorePinDefault(UEdGraphPin* Pin, const FGraphPatchJournal::FDefaultEntry& Entry)
{
	UEdGraphNode* OwningNode = Pin->GetOwningNode();
	OwningNode->Modify();
	UEdGraph* OwningGraph = OwningNode->GetGraph();
	if (OwningGraph)
	{
		OwningGraph->Modify();
	}
	UObject* PriorObject = nullptr;
	if (Entry.bPriorDefaultObjectSet)
	{
		PriorObject = FindObject<UObject>(nullptr, *Entry.PriorDefaultObjectPath);
	}
	Pin->DefaultValue = Entry.PriorDefaultValue;
	Pin->DefaultObject = PriorObject;
	Pin->DefaultTextValue = Entry.PriorDefaultTextValue;
	if (OwningGraph)
	{
		OwningGraph->NotifyGraphChanged();
	}
}

void FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid, UEdGraphNode*& OutNode)
{
	OutNode = nullptr;
	if (!Blueprint || !NodeGuid.IsValid()) return;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				OutNode = Node;
				return;
			}
		}
	}
}

bool RestorePinState(UEdGraphPin* Pin, const FGraphPatchJournal::FPinStateEntry& Entry)
{
	UObject* PriorObject = nullptr;
	if (!Entry.DefaultObjectPath.IsEmpty())
	{
		PriorObject = FindObject<UObject>(nullptr, *Entry.DefaultObjectPath);
		if (!PriorObject)
		{
			// A recorded default object vanished: recovery cannot be verified, so fail closed.
			return false;
		}
	}
	Pin->PinType = Entry.PinType;
	Pin->DefaultValue = Entry.DefaultValue;
	Pin->DefaultObject = PriorObject;
	Pin->DefaultTextValue = Entry.DefaultTextValue;
	return true;
}

FString LinkTargetKey(const FGuid& NodeGuid, const FName PinName)
{
	return FString::Printf(TEXT("%s.%s"), *NodeGuid.ToString(), *PinName.ToString());
}

/**
 * Restores one existing node's complete pin identity after an apply, including the class-driven
 * reconstruction a class-pin default triggers. The engine lifecycle the apply ran
 * (PinDefaultValueChanged then ReconstructNode) is replayed so class-dependent pins are rebuilt
 * from the restored class, then the journaled pin set, defaults and durable links are reconciled.
 */
bool RestoreNodePinState(
	UBlueprint* Blueprint,
	const FGraphPatchJournal& Journal,
	const FGraphPatchJournal::FNodePinSnapshot& Snapshot)
{
	UEdGraphNode* Node = nullptr;
	FindNodeByGuid(Blueprint, Snapshot.NodeGuid, Node);
	if (!Node)
	{
		return false;
	}
	Node->Modify();
	UEdGraph* Graph = Node->GetGraph();
	if (Graph)
	{
		Graph->Modify();
	}

	// 1. Put every pin the node still has back into its journaled state. The class default has to
	//    be restored before the replay, because the replay rebuilds pins from the class it reads.
	for (const FGraphPatchJournal::FPinStateEntry& Entry : Snapshot.Pins)
	{
		if (UEdGraphPin* Pin = Node->FindPin(Entry.PinName))
		{
			if (!RestorePinState(Pin, Entry)) return false;
		}
	}

	// 2. Replay the apply lifecycle when a class-pin default rebuilt the class-dependent pins.
	if (UK2Node_ConstructObjectFromClass* Construct = Cast<UK2Node_ConstructObjectFromClass>(Node))
	{
		UEdGraphPin* ClassPin = Construct->GetClassPin();
		if (ClassPin && Journal.WasPinDefaulted(Snapshot.NodeGuid, ClassPin->PinName))
		{
			const bool bPreviousDisableOrphanSaving = Construct->bDisableOrphanPinSaving;
			Construct->bDisableOrphanPinSaving = true;
			Construct->PinDefaultValueChanged(ClassPin);
			Construct->ReconstructNode();
			Construct->bDisableOrphanPinSaving = bPreviousDisableOrphanSaving;
		}
	}

	// 3. Reconcile the pin set: drop what the reconstruction added, recreate what it removed.
	TSet<FName> SnapshotPinNames;
	for (const FGraphPatchJournal::FPinStateEntry& Entry : Snapshot.Pins)
	{
		SnapshotPinNames.Add(Entry.PinName);
	}
	TArray<UEdGraphPin*> CurrentPins = Node->Pins;
	for (UEdGraphPin* Pin : CurrentPins)
	{
		if (!Pin || Pin->ParentPin != nullptr) continue;
		if (SnapshotPinNames.Contains(Pin->PinName)) continue;
		Node->RemovePin(Pin);
		if (Node->FindPin(Pin->PinName))
		{
			return false;
		}
	}
	for (const FGraphPatchJournal::FPinStateEntry& Entry : Snapshot.Pins)
	{
		UEdGraphPin* Pin = Node->FindPin(Entry.PinName);
		if (!Pin)
		{
			if (Entry.bSplitChild)
			{
				// Split children cannot be recreated faithfully; recovery must fail closed.
				return false;
			}
			Pin = Node->CreatePin(Entry.Direction, Entry.PinType, Entry.PinName);
		}
		if (!Pin || !RestorePinState(Pin, Entry))
		{
			return false;
		}
	}

	// 4. Reconcile the durable link set through the schema.
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	for (const FGraphPatchJournal::FPinStateEntry& Entry : Snapshot.Pins)
	{
		UEdGraphPin* Pin = Node->FindPin(Entry.PinName);
		if (!Pin)
		{
			return false;
		}
		TSet<FString> DesiredLinks;
		for (const FGraphPatchJournal::FPinLinkTarget& Link : Entry.LinkedTo)
		{
			DesiredLinks.Add(LinkTargetKey(Link.NodeGuid, Link.PinName));
		}
		TArray<UEdGraphPin*> CurrentLinks = Pin->LinkedTo;
		for (UEdGraphPin* LinkedPin : CurrentLinks)
		{
			UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			if (!LinkedNode || !DesiredLinks.Contains(LinkTargetKey(LinkedNode->NodeGuid, LinkedPin->PinName)))
			{
				Pin->BreakLinkTo(LinkedPin);
			}
		}
		for (const FGraphPatchJournal::FPinLinkTarget& Link : Entry.LinkedTo)
		{
			UEdGraphNode* LinkedNode = nullptr;
			FindNodeByGuid(Blueprint, Link.NodeGuid, LinkedNode);
			UEdGraphPin* LinkedPin = LinkedNode ? LinkedNode->FindPin(Link.PinName) : nullptr;
			if (!LinkedPin)
			{
				return false;
			}
			if (Pin->LinkedTo.Contains(LinkedPin)) continue;
			if (!Schema || !Schema->TryCreateConnection(Pin, LinkedPin))
			{
				return false;
			}
		}
	}

	if (Graph)
	{
		Graph->NotifyGraphChanged();
	}
	return true;
}

/** Reverses the journal in reverse order. Returns false when a recorded change cannot be undone. */
bool RestoreJournal(UBlueprint* Blueprint, FGraphPatchJournal& Journal)
{
	for (int32 Index = Journal.Links.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphNode* SourceNode = nullptr;
		UEdGraphNode* TargetNode = nullptr;
		FindNodeByGuid(Blueprint, Journal.Links[Index].SourceNodeGuid, SourceNode);
		FindNodeByGuid(Blueprint, Journal.Links[Index].TargetNodeGuid, TargetNode);
		UEdGraphPin* SourcePin = SourceNode ? SourceNode->FindPin(Journal.Links[Index].SourcePinName) : nullptr;
		UEdGraphPin* TargetPin = TargetNode ? TargetNode->FindPin(Journal.Links[Index].TargetPinName) : nullptr;
		if (SourcePin && TargetPin && SourcePin->LinkedTo.Contains(TargetPin))
		{
			SourcePin->BreakLinkTo(TargetPin);
		}
	}

	// Existing nodes are restored by full pin identity first: a class-pin apply rebuilds the
	// class-dependent pin set, which a default-only restore leaves diverged.
	for (const FGraphPatchJournal::FNodePinSnapshot& Snapshot : Journal.NodePins)
	{
		if (!RestoreNodePinState(Blueprint, Journal, Snapshot)) return false;
	}

	for (int32 Index = Journal.Defaults.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphNode* Node = nullptr;
		FindNodeByGuid(Blueprint, Journal.Defaults[Index].NodeGuid, Node);
		UEdGraphPin* Pin = Node ? Node->FindPin(Journal.Defaults[Index].PinName) : nullptr;
		if (!Pin)
		{
			// A default recorded on a node this patch created disappears with that node.
			if (Journal.WasNodeCreated(Journal.Defaults[Index].NodeGuid)) continue;
			return false;
		}
		if (Journal.Defaults[Index].bPriorDefaultObjectSet
			&& FindObject<UObject>(nullptr, *Journal.Defaults[Index].PriorDefaultObjectPath) == nullptr)
		{
			return false;
		}
		RestorePinDefault(Pin, Journal.Defaults[Index]);
	}

	for (int32 Index = Journal.NodeStates.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphNode* Node = nullptr;
		FindNodeByGuid(Blueprint, Journal.NodeStates[Index].NodeGuid, Node);
		if (!Node) continue;
		Node->Modify();
		Node->NodeComment = Journal.NodeStates[Index].Comment;
		Node->SetEnabledState(Journal.NodeStates[Index].EnabledState, Journal.NodeStates[Index].bUserSetEnabledState);
		Node->SetForceDisplayAsDisabled(Journal.NodeStates[Index].bForceDisplayAsDisabled);
		Node->bCommentBubblePinned = Journal.NodeStates[Index].bCommentBubblePinned;
		Node->bCommentBubbleVisible = Journal.NodeStates[Index].bCommentBubbleVisible;
	}

	for (int32 Index = Journal.AddedNodeGuids.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphNode* Node = nullptr;
		FindNodeByGuid(Blueprint, Journal.AddedNodeGuids[Index], Node);
		if (Node)
		{
			Node->DestroyNode();
		}
	}

	// Detached nodes are re-registered only after everything this patch created is gone, so the
	// preserved downstream pins are free again and the journaled links can be re-created exactly.
	if (!RestoreRemovedNodes(Blueprint, Journal)) return false;
	if (!RestoreRemovedMember(Blueprint, Journal)) return false;

	for (int32 Index = Journal.AddedGraphs.Num() - 1; Index >= 0; --Index)
	{
		if (Journal.AddedGraphs[Index])
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Journal.AddedGraphs[Index]);
		}
	}
	return true;
}

bool RestoredAuthoringMatches(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& FingerprintBefore)
{
	const TSharedPtr<FJsonObject> Restored = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	return Restored.IsValid() && FingerprintBefore.IsValid()
		&& Restored->GetStringField(TEXT("graph_authoring_hash"))
			== FingerprintBefore->GetStringField(TEXT("graph_authoring_hash"));
}

/** Compiles the target exactly once, reporting the real operation and preserving diagnostics. */
bool CompileTargetBlueprint(UBlueprint* Blueprint, const FName Operation, TArray<FString>& OutDiagnostics)
{
	NotifyOperation(Operation, Blueprint);
	FCompilerResultsLog Log;
	Log.bAnnotateMentionedNodes = false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
	FCortexGraphPatchOps::CollectCompilerDiagnostics(Log, OutDiagnostics);
	return Log.NumErrors == 0 && Blueprint->Status != BS_Error;
}

bool ResolveLiveNode(
	UBlueprint* Blueprint,
	const FCortexGraphPatchLocators& Locators,
	const FString& Identity,
	UEdGraphNode*& OutNode)
{
	OutNode = nullptr;
	FGuid NodeGuid;
	if (Identity == TEXT("entry"))
	{
		if (!Locators.bHasEntryNode) return false;
		NodeGuid = Locators.EntryNodeGuid;
	}
	else if (const FGuid* Mapped = Locators.NodeGuidByClientId.Find(Identity))
	{
		NodeGuid = *Mapped;
	}
	else if (!FGuid::Parse(Identity, NodeGuid))
	{
		return false;
	}
	FindNodeByGuid(Blueprint, NodeGuid, OutNode);
	return OutNode != nullptr;
}

bool RequestedOwnerDeclared(const TSharedPtr<FJsonObject>& Params, const TCHAR* MemberField)
{
	if (!Params.IsValid()) return false;
	FString Member;
	if (Params->TryGetStringField(MemberField, Member) && Member.Contains(TEXT("."))) return true;
	return Params->HasField(TEXT("owner_class")) || Params->HasField(TEXT("variable_class"));
}

FString DeclaredOwnerPath(UClass* OwnerClass)
{
	return OwnerClass ? OwnerClass->GetPathName() : FString(TEXT("none"));
}

/** Compares the re-resolved request symbol of one planned node against the applied native state. */
bool CompareNodeSymbol(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeJson,
	UEdGraphNode* Live,
	FName Family,
	FString& OutExpected,
	FString& OutActual,
	FString& OutFailure)
{
	OutExpected.Reset();
	OutActual.Reset();
	(void)Family;

	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	TSharedPtr<FJsonObject> Params;
	if (NodeJson->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr && ParamsPtr->IsValid())
	{
		Params = *ParamsPtr;
	}

	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Live))
	{
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult SymbolError;
		if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, Params, Symbol, SymbolError) || !Symbol.Function)
		{
			OutFailure = FString::Printf(TEXT("planned call symbol no longer resolves: %s"), *SymbolError.ErrorMessage);
			return false;
		}
		const bool bOwnerDeclared = RequestedOwnerDeclared(Params, TEXT("function_name"));
		UFunction* Target = Call->GetTargetFunction();
		OutExpected = bOwnerDeclared
			? FString::Printf(TEXT("%s|%s"), *Symbol.Function->GetName(), *DeclaredOwnerPath(Symbol.Function->GetOwnerClass()))
			: Symbol.Function->GetName();
		OutActual = Target
			? (bOwnerDeclared
				? FString::Printf(TEXT("%s|%s"), *Target->GetName(), *DeclaredOwnerPath(Target->GetOwnerClass()))
				: Target->GetName())
			: FString(TEXT("none"));
		return true;
	}

	if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Live))
	{
		const bool bWrite = Variable->IsA<UK2Node_VariableSet>();
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult SymbolError;
		if (!FCortexGraphSymbolResolver::ResolveProperty(Blueprint, Params, bWrite, Symbol, SymbolError))
		{
			OutFailure = FString::Printf(TEXT("planned variable symbol no longer resolves: %s"), *SymbolError.ErrorMessage);
			return false;
		}
		const bool bOwnerDeclared = RequestedOwnerDeclared(Params, TEXT("variable_name"));
		const FString NativeName = Variable->VariableReference.GetMemberName().ToString();
		const FString NativeOwner = Variable->VariableReference.IsSelfContext()
			? FString(TEXT("self"))
			: DeclaredOwnerPath(Variable->VariableReference.GetMemberParentClass());
		OutExpected = bOwnerDeclared
			? FString::Printf(TEXT("%s|%s"), *Symbol.MemberName.ToString(), *Symbol.ContextClassPath)
			: Symbol.MemberName.ToString();
		OutActual = bOwnerDeclared
			? FString::Printf(TEXT("%s|%s"), *NativeName, *NativeOwner)
			: NativeName;
		return true;
	}

	if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Live))
	{
		if (!Params.IsValid()) return true;
		if (!Params->HasField(TEXT("function_name")))
		{
			// An owner without a function name never identified an event: fail closed instead of
			// reporting a match for an uninitialized node.
			if (RequestedOwnerDeclared(Params, TEXT("function_name")))
			{
				OutFailure = TEXT("planned event selector declares an owner without a function name");
				return false;
			}
			return true;
		}
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult SymbolError;
		if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, Params, Symbol, SymbolError) || !Symbol.Function)
		{
			OutFailure = FString::Printf(TEXT("planned event symbol no longer resolves: %s"), *SymbolError.ErrorMessage);
			return false;
		}
		const bool bOwnerDeclared = RequestedOwnerDeclared(Params, TEXT("function_name"));
		const FString ActualOwner = Event->EventReference.GetMemberParentClass()
			? Event->EventReference.GetMemberParentClass()->GetPathName()
			: FString(TEXT("none"));
		// Apply stores the resolved request context (Symbol.ContextClass) in the event reference,
		// so readback must compare that same context owner rather than the declaring class.
		OutExpected = bOwnerDeclared
			? FString::Printf(TEXT("%s|%s"), *Symbol.Function->GetName(), *DeclaredOwnerPath(Symbol.ContextClass))
			: Symbol.Function->GetName();
		OutActual = bOwnerDeclared
			? FString::Printf(TEXT("%s|%s"), *Event->EventReference.GetMemberName().ToString(), *ActualOwner)
			: Event->EventReference.GetMemberName().ToString();
		return true;
	}

	if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Live))
	{
		if (!Params.IsValid()) return true;
		FString ClassIdentifier;
		const bool bHasClass = Params->TryGetStringField(TEXT("class"), ClassIdentifier)
			|| Params->TryGetStringField(TEXT("target_class"), ClassIdentifier);
		bool bRequestedPure = false;
		const bool bHasPurity = Params->TryGetBoolField(TEXT("is_pure"), bRequestedPure)
			|| Params->TryGetBoolField(TEXT("pure"), bRequestedPure)
			|| Params->TryGetBoolField(TEXT("bIsPureCast"), bRequestedPure);
		if (!bHasClass && !bHasPurity) return true;

		if (!bHasClass)
		{
			// No target class was requested, so no cast identity exists to compare: fail closed
			// instead of comparing the native target against a sentinel that can never match.
			OutFailure = TEXT("planned cast selector declares no target class");
			return false;
		}

		UClass* Target = nullptr;
		{
			FCortexCommandResult ResolveError;
			if (!FCortexGraphSymbolResolver::ResolveClass(ClassIdentifier, Target, ResolveError) || !Target)
			{
				OutFailure = FString::Printf(TEXT("planned cast target no longer resolves: %s"), *ResolveError.ErrorMessage);
				return false;
			}
		}
		const FString ExpectedClass = Target->GetPathName();
		// Purity is part of the requested node identity because the contract applies SetPurity.
		const int32 ExpectedPure = bHasPurity ? (bRequestedPure ? 1 : 0) : -1;
		const FString ActualClass = CastNode->TargetType ? CastNode->TargetType->GetPathName() : FString(TEXT("none"));
		OutExpected = FString::Printf(TEXT("%s|pure=%d"), *ExpectedClass, ExpectedPure);
		OutActual = FString::Printf(TEXT("%s|pure=%d"), *ActualClass,
			ExpectedPure < 0 ? -1 : (CastNode->IsNodePure() ? 1 : 0));
		return true;
	}

	if (const UK2Node_GenericCreateObject* Create = Cast<UK2Node_GenericCreateObject>(Live))
	{
		FString ClassIdentifier;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class"), ClassIdentifier)) return true;
		UClass* Target = nullptr;
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveClass(ClassIdentifier, Target, ResolveError) || !Target)
		{
			OutFailure = FString::Printf(TEXT("planned constructed class no longer resolves: %s"), *ResolveError.ErrorMessage);
			return false;
		}
		const UEdGraphPin* ClassPin = Create->GetClassPin();
		OutExpected = Target->GetPathName();
		OutActual = ClassPin && ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : FString(TEXT("none"));
		return true;
	}

	if (const UK2Node_Timeline* Timeline = Cast<UK2Node_Timeline>(Live))
	{
		FString TimelineName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("timeline_name"), TimelineName)) return true;
		OutExpected = TimelineName;
		OutActual = Timeline->TimelineName.ToString();
		return true;
	}

	if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Live))
	{
		OutExpected = TEXT("bound_graph");
		OutActual = Composite->BoundGraph ? TEXT("bound_graph") : TEXT("none");
		return true;
	}

	if (const UK2Node_SwitchEnum* SwitchEnum = Cast<UK2Node_SwitchEnum>(Live))
	{
		FString EnumName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("enum_name"), EnumName) || EnumName.IsEmpty()) return true;
		UEnum* RequestedEnum = FindFirstObject<UEnum>(*EnumName);
		if (!RequestedEnum)
		{
			OutFailure = FString::Printf(TEXT("planned switch enum no longer resolves: %s"), *EnumName);
			return false;
		}
		UEnum* NativeEnum = SwitchEnum->GetEnum();
		OutExpected = RequestedEnum->GetPathName();
		OutActual = NativeEnum ? NativeEnum->GetPathName() : FString(TEXT("none"));
		return true;
	}

	if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Live))
	{
		FString DelegateName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("delegate_name"), DelegateName) || DelegateName.IsEmpty()) return true;
		FString OwnerIdentifier;
		const bool bOwnerDeclared = Params->TryGetStringField(TEXT("delegate_class"), OwnerIdentifier)
			|| Params->TryGetStringField(TEXT("owner_class"), OwnerIdentifier);
		const FString NativeName = Delegate->GetPropertyName().ToString();
		if (!bOwnerDeclared)
		{
			OutExpected = DelegateName;
			OutActual = NativeName;
			return true;
		}
		UClass* Owner = nullptr;
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveClass(OwnerIdentifier, Owner, ResolveError) || !Owner)
		{
			OutFailure = FString::Printf(TEXT("planned delegate owner no longer resolves: %s"), *ResolveError.ErrorMessage);
			return false;
		}
		const FString NativeOwner = Delegate->DelegateReference.IsSelfContext()
			? FString(TEXT("self"))
			: DeclaredOwnerPath(Delegate->DelegateReference.GetMemberParentClass());
		OutExpected = FString::Printf(TEXT("%s|%s"), *DelegateName, *Owner->GetPathName());
		OutActual = FString::Printf(TEXT("%s|%s"), *NativeName, *NativeOwner);
		return true;
	}

	if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Live))
	{
		FString FunctionName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty()) return true;
		OutExpected = FunctionName;
		OutActual = CreateDelegate->GetFunctionName().ToString();
		return true;
	}

	if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Live))
	{
		FString MacroPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("macro_path"), MacroPath) || MacroPath.IsEmpty()) return true;
		// macro_path is presence-validated only, so a node without the requested macro graph is not
		// the requested node: report the mismatch instead of a false match. Apply resolves the
		// selector through ResolveMacroGraph (macro graph short name or full graph path), so the
		// expectation is canonicalized through the same resolution or a valid selector never matches.
		const UEdGraph* RequestedGraph = FCortexGraphNodeContract::ResolveMacroGraph(Blueprint, MacroPath);
		if (!RequestedGraph)
		{
			OutFailure = FString::Printf(TEXT("planned macro selector no longer resolves: %s"), *MacroPath);
			return false;
		}
		OutExpected = RequestedGraph->GetPathName();
		OutActual = Macro->GetMacroGraph() ? Macro->GetMacroGraph()->GetPathName() : FString(TEXT("none"));
		return true;
	}

	// A selector this build cannot compare natively means the node identity was never verified:
	// fail closed instead of reporting success.
	if (Params.IsValid())
	{
		static const TCHAR* const UnverifiableSelectors[] = {
			TEXT("function_name"), TEXT("member"), TEXT("variable_name"), TEXT("class"), TEXT("target_class"),
			TEXT("enum_name"), TEXT("macro_path"), TEXT("delegate_name"), TEXT("timeline_name")
		};
		for (const TCHAR* Selector : UnverifiableSelectors)
		{
			if (Params->HasField(Selector))
			{
				OutFailure = FString::Printf(TEXT("planned selector '%s' has no native readback comparison for this node"), Selector);
				return false;
			}
		}
	}

	return true;
}

/** Compares every planned pin signature of a created node against its native pins. */
bool ComparePlannedPinSignatures(
	const TSharedPtr<FJsonObject>& NodeJson,
	UEdGraphNode* Live,
	FString& OutFailure)
{
	const TArray<TSharedPtr<FJsonValue>>* Signatures = nullptr;
	if (!NodeJson->TryGetArrayField(TEXT("resolved_pins"), Signatures) || !Signatures)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Signatures)
	{
		const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Descriptor.IsValid())
		{
			OutFailure = TEXT("planned pin signature is invalid");
			return false;
		}
		const FString PinName = Descriptor->GetStringField(TEXT("name"));
		const UEdGraphPin* Pin = Live->FindPin(FName(*PinName));
		if (!Pin)
		{
			OutFailure = FString::Printf(TEXT("planned pin '%s' no longer exists on the applied node"), *PinName);
			return false;
		}
		const FString Expected = CanonicalPinSignature(Descriptor);
		const FString Actual = CanonicalPinSignature(MakePinSignatureDescriptor(*Pin));
		if (Expected != Actual)
		{
			OutFailure = FString::Printf(TEXT("planned pin '%s' signature mismatch: expected '%s', found '%s'"),
				*PinName, *Expected, *Actual);
			return false;
		}
	}

	// Every native pin must be a planned pin: an extra pin means the applied node is not the node
	// the request planned, even when all planned pins still match. The planned snapshot is taken
	// after the construction params ran, so no engine-added pin is expected here; an unexpected
	// pin therefore fails closed instead of being ignored.
	TSet<FString> PlannedPinNames;
	PlannedPinNames.Reserve(Signatures->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Signatures)
	{
		const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() ? Value->AsObject() : nullptr;
		if (Descriptor.IsValid())
		{
			PlannedPinNames.Add(Descriptor->GetStringField(TEXT("name")));
		}
	}
	for (const UEdGraphPin* Pin : Live->Pins)
	{
		if (!Pin) continue;
		const FString PinName = Pin->PinName.ToString();
		if (!PlannedPinNames.Contains(PinName))
		{
			OutFailure = FString::Printf(TEXT("applied node has an unexpected native pin '%s' that the request never planned"),
				*PinName);
			return false;
		}
	}
	return true;
}

bool ComparePlannedDefaults(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeJson,
	UEdGraphNode* Live,
	FReadbackFaultState& Faults,
	FString& OutFailure)
{
	(void)Blueprint;
	const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
	if (!NodeJson->TryGetObjectField(TEXT("defaults"), DefaultsPtr) || !DefaultsPtr || !DefaultsPtr->IsValid())
	{
		return true;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DefaultsPtr)->Values)
	{
		const FString PinName = Pair.Key;
		const TSharedPtr<FJsonObject> Literal = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
		UEdGraphPin* Pin = Live ? Live->FindPin(FName(*PinName)) : nullptr;
		FString Expected;
		FString Actual;
		if (!FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, Literal, Expected, Actual, OutFailure))
		{
			if (OutFailure.IsEmpty())
			{
				OutFailure = FString::Printf(TEXT("planned default pin '%s' no longer resolves"), *PinName);
			}
			return false;
		}
		if (Faults.InjectDefault())
		{
			Expected += TEXT("#injected");
		}
		if (Expected != Actual)
		{
			OutFailure = FString::Printf(TEXT("pin '%s' default readback mismatch: expected '%s', found '%s'"),
				*PinName, *Expected, *Actual);
			return false;
		}
	}
	return true;
}

/**
 * Compares one normalized planned node against a live native node on every canonical planned
 * dimension: node class, re-resolved symbol, planned pin signatures, planned tagged defaults and the
 * authored layout where a position was planned. Readback uses it after apply, and the planning-time
 * reuse reconciliation uses it for every identity that already exists, so exactly one comparator
 * defines what "matches the planned intent" means.
 */
bool ComparePlannedNodeAgainstNative(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeJson,
	UEdGraphNode* Live,
	FReadbackFaultState& Faults,
	FString& OutFailure)
{
	if (!NodeJson.IsValid() || !Live)
	{
		OutFailure = TEXT("planned node or live node is missing");
		return false;
	}
	const FString ClientId = NodeJson->GetStringField(TEXT("client_id"));
	const FString NodeClassName = NodeJson->GetStringField(TEXT("node_class"));

	FName Family;
	UClass* ResolvedClass = nullptr;
	FCortexGraphNodeContract::ResolveFamily(NodeClassName, Family, ResolvedClass);
	FString ExpectedClass = ResolvedClass ? ResolvedClass->GetPathName() : FString();
	if (Faults.InjectClass())
	{
		ExpectedClass += TEXT("#injected");
	}
	const FString ActualClass = Live->GetClass()->GetPathName();
	if (ExpectedClass != ActualClass)
	{
		OutFailure = FString::Printf(TEXT("planned node '%s' canonical class mismatch: expected '%s', found '%s'"),
			*ClientId, *ExpectedClass, *ActualClass);
		return false;
	}

	FString ExpectedSymbol;
	FString ActualSymbol;
	FString SymbolFailure;
	if (!CompareNodeSymbol(Blueprint, NodeJson, Live, Family, ExpectedSymbol, ActualSymbol, SymbolFailure))
	{
		OutFailure = SymbolFailure;
		return false;
	}
	if (Faults.InjectSymbol())
	{
		ExpectedSymbol += TEXT("#injected");
	}
	if (ExpectedSymbol != ActualSymbol)
	{
		OutFailure = FString::Printf(TEXT("planned node '%s' symbol mismatch: expected '%s', found '%s'"),
			*ClientId, *ExpectedSymbol, *ActualSymbol);
		return false;
	}

	if (!ComparePlannedPinSignatures(NodeJson, Live, OutFailure)) return false;
	if (!ComparePlannedDefaults(Blueprint, NodeJson, Live, Faults, OutFailure)) return false;

	// Authored layout is part of the planned intent: when a position was planned, the native layout
	// must match it exactly, otherwise the identity points at a node the request never authored.
	const TSharedPtr<FJsonObject>* PositionPtr = nullptr;
	if (NodeJson->TryGetObjectField(TEXT("position"), PositionPtr) && PositionPtr && PositionPtr->IsValid())
	{
		const int32 PlannedX = (*PositionPtr)->GetIntegerField(TEXT("x"));
		const int32 PlannedY = (*PositionPtr)->GetIntegerField(TEXT("y"));
		if (Live->NodePosX != PlannedX || Live->NodePosY != PlannedY)
		{
			OutFailure = FString::Printf(TEXT("planned node '%s' position mismatch: expected (%d,%d), found (%d,%d)"),
				*ClientId, PlannedX, PlannedY, Live->NodePosX, Live->NodePosY);
			return false;
		}
	}
	return true;
}

bool ComparePlannedPinUpdates(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	FReadbackFaultState& Faults,
	FString& OutFailure)
{
	const TArray<TSharedPtr<FJsonValue>>& Updates = Prepared.NormalizedRequest->GetArrayField(TEXT("pin_updates"));
	for (const TSharedPtr<FJsonValue>& Value : Updates)
	{
		const TSharedPtr<FJsonObject> Update = Value->AsObject();
		if (!Update.IsValid())
		{
			OutFailure = TEXT("normalized pin update is invalid");
			return false;
		}
		const FString NodeGuidText = Update->GetStringField(TEXT("node_guid"));
		FGuid NodeGuid;
		FGuid::Parse(NodeGuidText, NodeGuid);
		UEdGraphNode* Node = nullptr;
		FindNodeByGuid(Blueprint, NodeGuid, Node);
		const FString PinName = Update->GetStringField(TEXT("pin"));
		UEdGraphPin* Pin = Node ? Node->FindPin(FName(*PinName)) : nullptr;
		const TSharedPtr<FJsonObject>* LiteralPtr = nullptr;
		if (!Pin || !Update->TryGetObjectField(Update->HasField(TEXT("default")) ? TEXT("default") : TEXT("value"), LiteralPtr)
			|| !LiteralPtr || !LiteralPtr->IsValid())
		{
			OutFailure = FString::Printf(TEXT("planned pin update '%s.%s' no longer resolves"), *NodeGuidText, *PinName);
			return false;
		}
		FString Expected;
		FString Actual;
		if (!FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, *LiteralPtr, Expected, Actual, OutFailure)) return false;
		if (Faults.InjectDefault())
		{
			Expected += TEXT("#injected");
		}
		if (Expected != Actual)
		{
			OutFailure = FString::Printf(TEXT("pin update '%s.%s' default readback mismatch: expected '%s', found '%s'"),
				*NodeGuidText, *PinName, *Expected, *Actual);
			return false;
		}
		const TSharedPtr<FJsonObject>* SignaturePtr = nullptr;
		if (Update->TryGetObjectField(TEXT("resolved_pin"), SignaturePtr) && SignaturePtr && SignaturePtr->IsValid())
		{
			const FString ExpectedSignature = CanonicalPinSignature(*SignaturePtr);
			const FString ActualSignature = CanonicalPinSignature(MakePinSignatureDescriptor(*Pin));
			if (ExpectedSignature != ActualSignature)
			{
				OutFailure = FString::Printf(TEXT("pin update '%s.%s' signature mismatch: expected '%s', found '%s'"),
					*NodeGuidText, *PinName, *ExpectedSignature, *ActualSignature);
				return false;
			}
		}
	}
	return true;
}

bool GeneratedClassDeclaresFunction(UBlueprint* Blueprint, const FName FunctionName)
{
	UClass* GeneratedClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
	if (!GeneratedClass || FunctionName.IsNone()) return false;
	for (TFieldIterator<UFunction> It(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (UFunction* Function = *It)
		{
			if (Function->GetFName() == FunctionName) return true;
		}
	}
	return false;
}

/** Re-resolves the planned entry locator and compares it with the applied native entry symbol. */
bool CompareEntrySymbol(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FCortexGraphPatchLocators& Locators,
	bool bCompiled,
	FString& OutFailure)
{
	if (!Locators.bHasEntryNode) return true;
	const TSharedPtr<FJsonObject>* SymbolPtr = nullptr;
	if (!Prepared.NormalizedRequest->TryGetObjectField(TEXT("resolved_symbol"), SymbolPtr)
		|| !SymbolPtr || !SymbolPtr->IsValid())
	{
		return true;
	}
	FString ExpectedName;
	FString ExpectedOwner;
	(*SymbolPtr)->TryGetStringField(TEXT("function_name"), ExpectedName);
	(*SymbolPtr)->TryGetStringField(TEXT("owner_class"), ExpectedOwner);
	if (ExpectedName.IsEmpty()) return true;

	UEdGraphNode* Entry = nullptr;
	FindNodeByGuid(Blueprint, Locators.EntryNodeGuid, Entry);
	if (!Entry)
	{
		OutFailure = TEXT("implementation entry locator did not re-resolve after apply");
		return false;
	}

	if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Entry))
	{
		const FString ActualName = Event->EventReference.GetMemberName().ToString();
		const FString ActualOwner = Event->EventReference.GetMemberParentClass()
			? Event->EventReference.GetMemberParentClass()->GetPathName()
			: FString(TEXT("none"));
		if (ActualName != ExpectedName || (!ExpectedOwner.IsEmpty() && ActualOwner != ExpectedOwner))
		{
			OutFailure = FString::Printf(TEXT("implementation entry symbol mismatch: expected '%s' on '%s', found '%s' on '%s'"),
				*ExpectedName, *ExpectedOwner, *ActualName, *ActualOwner);
			return false;
		}
	}
	else if (const UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Entry))
	{
		const FString ActualName = FunctionEntry->GetGraph() ? FunctionEntry->GetGraph()->GetName() : FString();
		if (ActualName != ExpectedName)
		{
			OutFailure = FString::Printf(TEXT("implementation function entry mismatch: expected '%s', found '%s'"),
				*ExpectedName, *ActualName);
			return false;
		}
	}

	if (bCompiled && !GeneratedClassDeclaresFunction(Blueprint, FName(*ExpectedName)))
	{
		OutFailure = FString::Printf(TEXT("compiled generated class does not declare the requested symbol '%s'"), *ExpectedName);
		return false;
	}
	return true;
}

bool ComparePlannedEdges(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FCortexGraphPatchLocators& Locators,
	FReadbackFaultState& Faults,
	FString& OutFailure)
{
	const TArray<TSharedPtr<FJsonValue>>& Connections = Prepared.NormalizedRequest->GetArrayField(TEXT("connections"));
	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Connection = Connections[Index]->AsObject();
		const TSharedPtr<FJsonObject>* FromPtr = nullptr;
		const TSharedPtr<FJsonObject>* ToPtr = nullptr;
		if (!Connection.IsValid()
			|| !Connection->TryGetObjectField(TEXT("from"), FromPtr) || !FromPtr || !FromPtr->IsValid()
			|| !Connection->TryGetObjectField(TEXT("to"), ToPtr) || !ToPtr || !ToPtr->IsValid())
		{
			OutFailure = FString::Printf(TEXT("normalized planned edge %d is invalid"), Index);
			return false;
		}
		FString FromId;
		FString FromPinName;
		FString ToId;
		FString ToPinName;
		bool bFromEntry = false;
		bool bToEntry = false;
		FCortexCommandResult EndpointError;
		if (!ParseEndpoint(*FromPtr, FromId, FromPinName, bFromEntry, EndpointError, TEXT("connection.from"))
			|| !ParseEndpoint(*ToPtr, ToId, ToPinName, bToEntry, EndpointError, TEXT("connection.to")))
		{
			OutFailure = EndpointError.ErrorMessage;
			return false;
		}
		if (Faults.InjectEdge())
		{
			ToPinName += TEXT("#injected");
		}

		UEdGraphNode* SourceNode = nullptr;
		UEdGraphNode* TargetNode = nullptr;
		if (!ResolveLiveNode(Blueprint, Locators, FromId, SourceNode)
			|| !ResolveLiveNode(Blueprint, Locators, ToId, TargetNode))
		{
			OutFailure = FString::Printf(TEXT("planned edge %d no longer re-resolves its endpoints"), Index);
			return false;
		}
		UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*FromPinName));
		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*ToPinName));
		if (!SourcePin || !TargetPin)
		{
			OutFailure = FString::Printf(TEXT("planned edge %d no longer resolves pin '%s' -> '%s'"),
				Index, *FromPinName, *ToPinName);
			return false;
		}
		if (!SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Num() != 1)
		{
			OutFailure = FString::Printf(TEXT("planned edge %d is not exactly the requested native link"), Index);
			return false;
		}
	}
	return true;
}

/**
 * Verifies the implementation-owned parent call of a `call_kind="parent"` target exactly as
 * FCortexGraphImplementationOps::EnsureForPatch creates and wires it: one
 * UK2Node_CallParentFunction for the resolved symbol, fed by the entry's exec output, with every
 * entry data output mirrored to the parent parameter of the same name and, for function-graph
 * targets, the parent call's result routed into the function result node. Comparison only: nothing
 * is created, rewired or journalled here.
 */
bool CompareImplementationParentCall(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FCortexGraphPatchLocators& Locators,
	FString& OutFailure)
{
	const TSharedPtr<FJsonObject>* TargetPtr = nullptr;
	if (!Prepared.NormalizedRequest->TryGetObjectField(TEXT("target"), TargetPtr)
		|| !TargetPtr || !TargetPtr->IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* ImplementationPtr = nullptr;
	if (!(*TargetPtr)->TryGetObjectField(TEXT("implementation"), ImplementationPtr)
		|| !ImplementationPtr || !ImplementationPtr->IsValid())
	{
		return true;
	}
	FString CallKindText;
	if (!(*ImplementationPtr)->TryGetStringField(TEXT("call_kind"), CallKindText)) return true;
	// The accepted spelling contract lives in exactly one place: the resolver the apply path uses, so
	// the comparator cannot drift from the call kinds the implementation target really acts on.
	ECortexCallKind CallKind = ECortexCallKind::Ordinary;
	if (!FCortexGraphSymbolResolver::TryParseCallKind(CallKindText, CallKind)) return true;
	if (CallKind != ECortexCallKind::Parent) return true;
	if (!Locators.bHasEntryNode) return true;

	UEdGraphNode* Entry = nullptr;
	FindNodeByGuid(Blueprint, Locators.EntryNodeGuid, Entry);
	if (!Entry || !Entry->GetGraph())
	{
		OutFailure = TEXT("implementation parent call cannot be verified: the entry locator did not re-resolve");
		return false;
	}
	UEdGraph* const EntryGraph = Entry->GetGraph();
	UK2Node_CallParentFunction* ParentCall = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : EntryGraph->Nodes)
	{
		if (!ParentCall)
		{
			ParentCall = Cast<UK2Node_CallParentFunction>(Node);
		}
		if (!ResultNode)
		{
			ResultNode = Cast<UK2Node_FunctionResult>(Node);
		}
	}
	if (!ParentCall)
	{
		OutFailure = FString::Printf(TEXT("implementation parent call is missing from graph '%s'"), *EntryGraph->GetName());
		return false;
	}

	FString ExpectedName;
	FString ExpectedOwner;
	const TSharedPtr<FJsonObject>* SymbolPtr = nullptr;
	if (Prepared.NormalizedRequest->TryGetObjectField(TEXT("resolved_symbol"), SymbolPtr)
		&& SymbolPtr && SymbolPtr->IsValid())
	{
		(*SymbolPtr)->TryGetStringField(TEXT("function_name"), ExpectedName);
		(*SymbolPtr)->TryGetStringField(TEXT("owner_class"), ExpectedOwner);
	}
	const UClass* const ActualOwner = ParentCall->FunctionReference.GetMemberParentClass();
	const FString ActualOwnerPath = ActualOwner ? ActualOwner->GetPathName() : FString(TEXT("none"));
	const FString ActualName = ParentCall->FunctionReference.GetMemberName().ToString();
	if (!ExpectedName.IsEmpty() && ActualName != ExpectedName)
	{
		OutFailure = FString::Printf(TEXT("implementation parent call symbol mismatch: expected '%s', found '%s'"),
			*ExpectedName, *ActualName);
		return false;
	}
	if (!ExpectedOwner.IsEmpty() && ActualOwnerPath != ExpectedOwner)
	{
		OutFailure = FString::Printf(TEXT("implementation parent call owner mismatch: expected '%s', found '%s'"),
			*ExpectedOwner, *ActualOwnerPath);
		return false;
	}

	UEdGraphPin* const EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* const ParentExec = ParentCall->GetExecPin();
	if (!EntryThen || !ParentExec || !EntryThen->LinkedTo.Contains(ParentExec) || ParentExec->LinkedTo.Num() != 1)
	{
		OutFailure = TEXT("implementation parent call exec wiring mismatch: the entry 'then' pin does not feed the parent call exec pin");
		return false;
	}

	for (UEdGraphPin* EntryPin : Entry->Pins)
	{
		if (!EntryPin || EntryPin->Direction != EGPD_Output
			|| EntryPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		UEdGraphPin* const ParentParam = ParentCall->FindPin(EntryPin->PinName);
		if (!ParentParam || !EntryPin->LinkedTo.Contains(ParentParam))
		{
			OutFailure = FString::Printf(
				TEXT("implementation parent call parameter '%s' is not fed by the entry output of the same name"),
				*EntryPin->PinName.ToString());
			return false;
		}
	}

	if (ResultNode)
	{
		UEdGraphPin* const ParentThen = ParentCall->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* const ResultExec = ResultNode->GetExecPin();
		if (!ParentThen || !ResultExec || !ParentThen->LinkedTo.Contains(ResultExec))
		{
			OutFailure = TEXT("implementation parent call result wiring mismatch: the parent call 'then' pin does not feed the function result exec pin");
			return false;
		}
		for (UEdGraphPin* ResultPin : ResultNode->Pins)
		{
			if (!ResultPin || ResultPin->Direction != EGPD_Input
				|| ResultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			UEdGraphPin* const ParentOutput = ParentCall->FindPin(ResultPin->PinName);
			if (!ParentOutput || !ParentOutput->LinkedTo.Contains(ResultPin))
			{
				OutFailure = FString::Printf(
					TEXT("implementation parent call output '%s' does not feed the function result input of the same name"),
					*ResultPin->PinName.ToString());
				return false;
			}
		}
	}
	return true;
}

/**
 * Compares the whole planned intent against the live native asset: every planned node's canonical
 * dimensions, every planned pin update, the implementation entry symbol and every planned edge.
 * Readback after apply and the planning-time reuse reconciliation both use this one comparator, so
 * a reuse match is proven by exactly the comparison that proves an applied patch.
 */
bool ComparePlannedIntentAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FCortexGraphPatchLocators& Locators,
	bool bCompiled,
	bool bAllowFaultInjection,
	FString& OutFailure)
{
	FReadbackFaultState Faults;
	Faults.bInjectionAllowed = bAllowFaultInjection;

	const TArray<TSharedPtr<FJsonValue>>& Nodes = Prepared.NormalizedRequest->GetArrayField(TEXT("nodes"));
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject> NodeJson = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!NodeJson.IsValid())
		{
			OutFailure = TEXT("normalized planned node is invalid");
			return false;
		}
		const FString ClientId = NodeJson->GetStringField(TEXT("client_id"));

		UEdGraphNode* Live = nullptr;
		if (!ResolveLiveNode(Blueprint, Locators, ClientId, Live))
		{
			OutFailure = FString::Printf(TEXT("planned node '%s' did not re-resolve after apply"), *ClientId);
			return false;
		}

		if (!ComparePlannedNodeAgainstNative(Blueprint, NodeJson, Live, Faults, OutFailure)) return false;
	}

	if (!ComparePlannedPinUpdates(Blueprint, Prepared, Faults, OutFailure)) return false;
	if (!CompareEntrySymbol(Blueprint, Prepared, Locators, bCompiled, OutFailure)) return false;
	if (!CompareImplementationParentCall(Blueprint, Prepared, Locators, OutFailure)) return false;
	if (!ComparePlannedEdges(Blueprint, Prepared, Locators, Faults, OutFailure)) return false;
	return true;
}

/** Authoritative native readback of every planned locator against the applied state. */
bool VerifyAppliedState(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	const FGraphPatchJournal& Journal,
	bool bCompiled,
	FString& OutFailure)
{
	if (!ComparePlannedIntentAgainstNative(Blueprint, Prepared, Journal.Locators, bCompiled, true, OutFailure))
	{
		return false;
	}
	// A migration additionally proves the replacement terminators, the realized boundary remap and
	// the preservation capture the authoring comparator cannot express.
	if (Prepared.MigrationPlan.IsValid())
	{
		FCortexGraphMigrationPlan Plan;
		FCortexCommandResult PlanError;
		if (!FCortexGraphMigrationPlan::FromJson(Prepared.MigrationPlan, Plan, PlanError))
		{
			OutFailure = PlanError.ErrorMessage;
			return false;
		}
		return FCortexGraphMigrationOps::VerifyReplacementAgainstNative(Blueprint, Plan, bCompiled, true, OutFailure);
	}
	return true;
}

bool HandleApplyFailure(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	FGraphPatchJournal& Journal,
	FCortexGraphPatchOutcome* OutOutcome,
	FCortexCommandResult& OutError,
	const FString& Message,
	const FString& ErrorCode,
	bool bApplyPhaseFailure)
{
	if (OutOutcome)
	{
		// Durable identities of what the patch applied and reverted: report them on the failure
		// path too (apply fault, compile failure, readback mismatch, blocked unverified recovery),
		// so the residual mapping of a failed patch stays inspectable.
		OutOutcome->Locators = Journal.Locators;
	}
	if (Journal.Transaction)
	{
		Journal.Transaction->Cancel();
	}
	const bool bContentRestored = RestoreJournal(Blueprint, Journal);

	bool bRecoveryCompileSucceeded = true;
	if (bContentRestored && Journal.bCompileAttempted)
	{
		TArray<FString> RecoveryDiagnostics;
		bRecoveryCompileSucceeded =
			CompileTargetBlueprint(Blueprint, TEXT("recovery_compile"), RecoveryDiagnostics);
		if (OutOutcome)
		{
			OutOutcome->RecoveryCompileCount += 1;
			OutOutcome->Diagnostics.Append(RecoveryDiagnostics);
		}
	}

	const bool bInjectedFailure = ShouldInjectApplyFault(TEXT("verification_failure"));
	const bool bAuthoringRestored = bContentRestored && RestoredAuthoringMatches(Blueprint, Prepared.FingerprintBefore);
	bool bGeneratedRestored = true;
	if (Journal.bCompileAttempted)
	{
		bGeneratedRestored = bRecoveryCompileSucceeded
			&& FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint) == Journal.GeneratedStateBefore;
	}
	bool bPreservationRestored = true;
	if (Journal.bVerifyPreservation)
	{
		UEdGraph* PreservationGraph = nullptr;
		TArray<UEdGraph*> PreservationGraphs;
		Blueprint->GetAllGraphs(PreservationGraphs);
		for (UEdGraph* Candidate : PreservationGraphs)
		{
			if (Candidate && Candidate->GraphGuid == Journal.PreservationGraphGuid)
			{
				PreservationGraph = Candidate;
				break;
			}
		}
		bPreservationRestored = PreservationGraph != nullptr
			&& FCortexGraphMigrationOps::CapturePreservation(Blueprint, PreservationGraph, Journal.PreservationExcludedGuids)
				== Journal.PreservationBefore;
	}
	const bool bVerified = (bInjectedFailure == false) && bAuthoringRestored && bGeneratedRestored && bPreservationRestored;

	if (!bVerified)
	{
		// Unverified content must not look clean, otherwise it can be saved as if verified.
		Blueprint->GetOutermost()->SetDirtyFlag(true);
		FCortexAssetMutationGuard::Block(Blueprint, TEXT("Graph patch rollback verification failed"));
		if (OutOutcome)
		{
			if (bApplyPhaseFailure) OutOutcome->ApplyStatus = TEXT("failed");
			OutOutcome->RollbackStatus = TEXT("unverified");
			OutOutcome->bBlocked = true;
			if (!bContentRestored)
			{
				OutOutcome->Diagnostics.Add(TEXT("rollback: a recorded change could not be reversed"));
			}
			if (!bAuthoringRestored)
			{
				OutOutcome->Diagnostics.Add(TEXT("rollback: authoring fingerprint was not restored"));
			}
			if (!bGeneratedRestored)
			{
				OutOutcome->Diagnostics.Add(TEXT("rollback: generated state was not restored"));
			}
			if (!bPreservationRestored)
			{
				OutOutcome->Diagnostics.Add(TEXT("rollback: the downstream node set outside the replaced entry was not restored"));
			}
			if (!Journal.Member.Diagnostic.IsEmpty())
			{
				OutOutcome->Diagnostics.Add(FString::Printf(TEXT("rollback: %s"), *Journal.Member.Diagnostic));
			}
			if (bInjectedFailure)
			{
				OutOutcome->Diagnostics.Add(TEXT("rollback: recovery verification failed by test injection"));
			}
			FCortexGraphPatchOps::TrimDiagnostics(OutOutcome->Diagnostics);
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Graph patch recovery verification failed; asset is blocked from mutation"));
		return false;
	}

	Blueprint->GetOutermost()->SetDirtyFlag(Journal.bPackageWasDirty);
	if (Journal.StatusBefore != BS_Unknown)
	{
		Blueprint->Status = Journal.StatusBefore;
	}
	if (OutOutcome)
	{
		if (bApplyPhaseFailure) OutOutcome->ApplyStatus = TEXT("failed");
		OutOutcome->RollbackStatus = TEXT("restored");
		FCortexGraphPatchOps::TrimDiagnostics(OutOutcome->Diagnostics);
	}
	OutError = FCortexCommandRouter::Error(ErrorCode, Message);
	return false;
}

bool ApplyPrepared(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	FGraphPatchJournal& Journal,
	FCortexGraphPatchOutcome* OutOutcome,
	FCortexCommandResult& OutError)
{
	OutError = FCortexCommandResult();
	FString BlockReason;
	if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		return false;
	}
	if (!Blueprint || !Prepared.NormalizedRequest.IsValid() || Prepared.bDryRun)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A non-preview prepared graph patch is required"));
		return false;
	}
	if (!FCortexGraphPatchState::ValidatePrecondition(
		Prepared.FingerprintBefore, FCortexGraphPatchState::ComputeFingerprint(Blueprint), OutError))
	{
		return false;
	}

	Journal.bPackageWasDirty = Blueprint->GetOutermost()->IsDirty();
	Journal.StatusBefore = Blueprint->Status;
	Journal.GeneratedStateBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint);
	Journal.Transaction = MakeUnique<FScopedTransaction>(FText::FromString(TEXT("Cortex: Apply Graph Patch")));

	auto Fail = [&](const FString& Message) -> bool
	{
		return HandleApplyFailure(Blueprint, Prepared, Journal, OutOutcome, OutError, Message,
			CortexErrorCodes::InvalidOperation, true);
	};

	if (Prepared.MigrationPlan.IsValid())
	{
		// The migration shell: one journaled entry/terminator replacement (plus an optional member
		// removal) inside the same transaction, followed by the caller's single compile and the
		// shared readback.
		FCortexGraphMigrationPlan Plan;
		if (!FCortexGraphMigrationPlan::FromJson(Prepared.MigrationPlan, Plan, OutError))
		{
			return false;
		}
		FGuid GraphGuid;
		FGuid::Parse(Plan.GraphGuid, GraphGuid);
		UEdGraph* MigrationGraph = FCortexGraphMigrationOps::FindGraphByGuid(Blueprint, GraphGuid);
		if (!MigrationGraph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("the migration target graph did not re-resolve after the final guard"));
			return false;
		}
		Journal.Locators.GraphGuid = MigrationGraph->GraphGuid;
		Journal.Locators.SubgraphPath = Plan.SubgraphPath;
		Journal.Locators.EntryNodeGuid = Plan.Identity.EntryGuid;
		Journal.Locators.bHasEntryNode = true;
		Journal.Locators.NodeGuidByClientId.Add(TEXT("entry"), Plan.Identity.EntryGuid);
		if (Plan.HasResultTerminator())
		{
			Journal.Locators.NodeGuidByClientId.Add(TEXT("result"), Plan.Identity.ResultGuid);
		}

		// The preservation contract is captured before the first migration mutation, so recovery can
		// prove the downstream body came back instead of assuming it.
		Journal.bVerifyPreservation = true;
		Journal.PreservationGraphGuid = MigrationGraph->GraphGuid;
		Journal.PreservationExcludedGuids.Add(Plan.Identity.EntryGuid);
		Journal.PreservationExcludedGuids.Add(Plan.Identity.ResultGuid);
		FGuid SourceEntryGuid;
		if (FGuid::Parse(Plan.SourceEntryGuid, SourceEntryGuid)) Journal.PreservationExcludedGuids.Add(SourceEntryGuid);
		FGuid SourceResultGuid;
		if (FGuid::Parse(Plan.SourceResultGuid, SourceResultGuid)) Journal.PreservationExcludedGuids.Add(SourceResultGuid);
		for (const FString& GuidText : Plan.MemberReferenceNodeGuids)
		{
			FGuid ReferenceGuid;
			if (FGuid::Parse(GuidText, ReferenceGuid)) Journal.PreservationExcludedGuids.Add(ReferenceGuid);
		}
		Journal.PreservationBefore = FCortexGraphMigrationOps::CapturePreservation(
			Blueprint, MigrationGraph, Journal.PreservationExcludedGuids);

		if (Plan.bRemoveMember)
		{
			// The engine's self-only variable removal destroys the referencing nodes, so they are
			// detached and journaled first and come back with the member on recovery.
			for (const FString& GuidText : Plan.MemberReferenceNodeGuids)
			{
				FGuid ReferenceGuid;
				FGuid::Parse(GuidText, ReferenceGuid);
				UEdGraphNode* ReferenceNode = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, ReferenceGuid);
				if (!ReferenceNode || !ReferenceNode->GetGraph())
				{
					return Fail(TEXT("a planned shadowing-member reference no longer resolves"));
				}
				JournalNodeRemoved(ReferenceNode, Journal);
				ReferenceNode->GetGraph()->RemoveNode(ReferenceNode);
			}
			Journal.Member.Name = FName(*Plan.MemberName);
			Journal.Member.Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Journal.Member.Name);
			Journal.Member.Captured = FCortexGraphMigrationOps::CaptureShadowingMember(Blueprint, Journal.Member.Name);
			Journal.bMemberCaptured = Journal.Member.Captured.IsValid();
			if (!Journal.bMemberCaptured)
			{
				return Fail(TEXT("the planned shadowing member no longer resolves"));
			}
			FCortexCommandResult MemberError;
			if (!FCortexGraphMigrationOps::RemoveShadowingMember(Blueprint, Journal.Member.Name, MemberError))
			{
				return Fail(MemberError.ErrorMessage);
			}
		}

		UEdGraphNode* SourceEntry = nullptr;
		if (FGuid::Parse(Plan.SourceEntryGuid, SourceEntryGuid))
		{
			SourceEntry = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, SourceEntryGuid);
		}
		if (!SourceEntry || SourceEntry->GetGraph() != MigrationGraph)
		{
			return Fail(TEXT("the stale implementation entry no longer resolves in the target graph"));
		}
		JournalNodeRemoved(SourceEntry, Journal);
		MigrationGraph->Modify();
		MigrationGraph->RemoveNode(SourceEntry);
		if (!Plan.SourceResultGuid.IsEmpty())
		{
			UEdGraphNode* SourceResult = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, SourceResultGuid);
			if (!SourceResult || SourceResult->GetGraph() != MigrationGraph)
			{
				return Fail(TEXT("the stale result terminator no longer resolves in the target graph"));
			}
			JournalNodeRemoved(SourceResult, Journal);
			MigrationGraph->RemoveNode(SourceResult);
		}
		if (ShouldInjectApplyFault(TEXT("migration_entry_removed")))
		{
			return Fail(TEXT("Test fault injected after entry removal"));
		}

		UEdGraphNode* ReplacementEntry = nullptr;
		UEdGraphNode* ReplacementResult = nullptr;
		if (!FCortexGraphMigrationOps::RegisterReplacement(Blueprint, MigrationGraph, Plan, ReplacementEntry, ReplacementResult, OutError))
		{
			return Fail(OutError.ErrorMessage);
		}
		if (ReplacementEntry) Journal.AddedNodeGuids.Add(ReplacementEntry->NodeGuid);
		if (ReplacementResult) Journal.AddedNodeGuids.Add(ReplacementResult->NodeGuid);
		if (ShouldInjectApplyFault(TEXT("migration_after_registration")))
		{
			return Fail(TEXT("Test fault injected after graph registration"));
		}

		TArray<FCortexGraphMigrationLink> CreatedLinks;
		FCortexCommandResult RemapError;
		if (!FCortexGraphMigrationOps::RemapBoundary(
			Blueprint, MigrationGraph, Plan, ReplacementEntry, ReplacementResult, CreatedLinks, RemapError))
		{
			return Fail(RemapError.ErrorMessage);
		}
		for (const FCortexGraphMigrationLink& Link : CreatedLinks)
		{
			Journal.Links.Add({ Link.FromNodeGuid, Link.FromPin, Link.ToNodeGuid, Link.ToPin });
		}
		MigrationGraph->NotifyGraphChanged();
		return true;
	}

	const TSharedPtr<FJsonObject>* TargetPtr = nullptr;
	if (!Prepared.NormalizedRequest->TryGetObjectField(TEXT("target"), TargetPtr) || !TargetPtr || !TargetPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Prepared patch has no target"));
		return false;
	}
	UEdGraphNode* ImplementationEntry = nullptr;
	UEdGraph* Graph = nullptr;
	FCortexGraphPatchState ImplementationState;
	const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
	if ((*TargetPtr)->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) && GraphRefPtr && GraphRefPtr->IsValid())
	{
		FString GraphGuidString;
		FString SubgraphPath;
		FGuid GraphGuid;
		if (!(*GraphRefPtr)->TryGetStringField(TEXT("graph_guid"), GraphGuidString) || !FGuid::Parse(GraphGuidString, GraphGuid)
			|| ((*GraphRefPtr)->HasField(TEXT("subgraph_path"))
				&& !(*GraphRefPtr)->TryGetStringField(TEXT("subgraph_path"), SubgraphPath)))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Prepared graph target has an invalid graph reference"));
			return false;
		}
		Journal.Locators.GraphGuid = GraphGuid;
		Journal.Locators.SubgraphPath = SubgraphPath;
		if (!ResolveGraphByGuid(Blueprint, GraphGuid, SubgraphPath, Graph, OutError))
		{
			return false;
		}
	}
	else
	{
		const TSharedPtr<FJsonObject>* ImplementationPtr = nullptr;
		if (!(*TargetPtr)->TryGetObjectField(TEXT("implementation"), ImplementationPtr) || !ImplementationPtr || !ImplementationPtr->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Prepared patch target is invalid"));
			return false;
		}
		FCortexGraphImplementationEnsureResult Ensured =
			FCortexGraphImplementationOps::EnsureForPatch(Blueprint, *ImplementationPtr, ImplementationState);
		if (!Ensured.bSuccess || !Ensured.Graph)
		{
			OutError = Ensured;
			return false;
		}
		ImplementationEntry = Ensured.EntryNode;
		Graph = Ensured.Graph;
		Journal.Locators.GraphGuid = Graph->GraphGuid;
		Journal.Locators.SubgraphPath.Reset();
		for (UEdGraphNode* CreatedNode : ImplementationState.AddedNodes)
		{
			if (CreatedNode && CreatedNode->NodeGuid.IsValid())
			{
				Journal.AddedNodeGuids.Add(CreatedNode->NodeGuid);
			}
		}
		for (UEdGraph* CreatedGraph : ImplementationState.AddedGraphs)
		{
			if (CreatedGraph)
			{
				Journal.AddedGraphs.Add(CreatedGraph);
			}
		}
		if (ImplementationEntry && ImplementationEntry->NodeGuid.IsValid())
		{
			Journal.Locators.EntryNodeGuid = ImplementationEntry->NodeGuid;
			Journal.Locators.bHasEntryNode = true;
		}
	}

	if (Journal.AddedGraphs.Num() > 0 && ShouldInjectApplyFault(TEXT("implementation_graph")))
	{
		return Fail(TEXT("Test fault injected after implementation graph creation"));
	}

	TMap<FString, UEdGraphNode*> NodesById;
	for (UEdGraphNode* Existing : Graph->Nodes)
	{
		if (Existing) NodesById.Add(Existing->NodeGuid.ToString(), Existing);
	}
	const TArray<TSharedPtr<FJsonValue>>& Nodes = Prepared.NormalizedRequest->GetArrayField(TEXT("nodes"));
	if (ImplementationEntry) NodesById.Add(TEXT("entry"), ImplementationEntry);

	Graph->Modify();
	int32 CreatedByPatchNodes = 0;
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject> NodeJson = Value->AsObject();
		if (!NodeJson.IsValid()) return Fail(TEXT("Prepared node is invalid"));
		const FString ClientId = NodeJson->GetStringField(TEXT("client_id"));
		const FString NodeClassName = NodeJson->GetStringField(TEXT("node_class"));
		FName Family;
		UClass* NodeClass = nullptr;
		if (!FCortexGraphNodeContract::ResolveFamily(NodeClassName, Family, NodeClass) || !NodeClass)
		{
			return Fail(TEXT("Prepared node class no longer resolves"));
		}
		const FGuid* DerivedGuid = Prepared.NodeGuidByClientId.Find(ClientId);
		if (!DerivedGuid || !DerivedGuid->IsValid())
		{
			return Fail(TEXT("Prepared node has no deterministic identity"));
		}
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
		Node->NodeGuid = *DerivedGuid;
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		FString ApplyError;
		if (NodeJson->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr && ParamsPtr->IsValid()
			&& !FCortexGraphNodeContract::ApplyNodeConstructionParams(Graph, Node, Blueprint, *ParamsPtr, ApplyError))
		{
			return Fail(ApplyError);
		}
		if (Node->Pins.Num() == 0)
		{
			Node->AllocateDefaultPins();
		}
		Graph->AddNode(Node, true, false);
		Journal.AddedNodeGuids.Add(Node->NodeGuid);
		if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
		{
			CompositeNode->PostPlacedNewNode();
			if (CompositeNode->Pins.Num() == 0)
			{
				CompositeNode->AllocateDefaultPins();
			}
		}
		const TSharedPtr<FJsonObject>* PositionPtr = nullptr;
		if (NodeJson->TryGetObjectField(TEXT("position"), PositionPtr) && PositionPtr && PositionPtr->IsValid())
		{
			Node->NodePosX = (*PositionPtr)->GetIntegerField(TEXT("x"));
			Node->NodePosY = (*PositionPtr)->GetIntegerField(TEXT("y"));
			if (ShouldInjectApplyFault(TEXT("layout")))
			{
				return Fail(TEXT("Test fault injected after layout mutation"));
			}
		}
		++CreatedByPatchNodes;
		if (CreatedByPatchNodes == 2
			&& (ShouldInjectApplyFault(TEXT("second_node")) || ShouldInjectApplyFault(TEXT("verification_failure"))))
		{
			return Fail(TEXT("Test fault injected after second node"));
		}
		NodesById.Add(ClientId, Node);
		Journal.Locators.NodeGuidByClientId.Add(ClientId, Node->NodeGuid);
		const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
		if (NodeJson->TryGetObjectField(TEXT("defaults"), DefaultsPtr) && DefaultsPtr && DefaultsPtr->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DefaultsPtr)->Values)
			{
				UEdGraphPin* Pin = Node->FindPin(FName(*Pair.Key));
				const TSharedPtr<FJsonObject> Literal = Pair.Value->AsObject();
				if (!Pin || !Literal.IsValid()) return Fail(TEXT("Prepared node default no longer resolves"));
				FCortexCommandResult DefaultError;
				JournalNodeState(Node, Journal);
				JournalPinDefault(Pin, Journal);
				if (!FCortexGraphPinDefaults::ApplyDefault(Pin, Literal, DefaultError)) return Fail(DefaultError.ErrorMessage);
				if (ShouldInjectApplyFault(TEXT("first_default")))
				{
					return Fail(TEXT("Test fault injected after first default"));
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>& PinUpdates = Prepared.NormalizedRequest->GetArrayField(TEXT("pin_updates"));
	for (const TSharedPtr<FJsonValue>& Value : PinUpdates)
	{
		const TSharedPtr<FJsonObject> Update = Value->AsObject();
		if (!Update.IsValid()) return Fail(TEXT("Prepared pin update is invalid"));
		FGuid NodeGuid;
		FString PinName;
		if (!FGuid::Parse(Update->GetStringField(TEXT("node_guid")), NodeGuid)
			|| !Update->TryGetStringField(TEXT("pin"), PinName))
		{
			return Fail(TEXT("Prepared pin update no longer identifies a pin"));
		}
		UEdGraphNode* Node = NodesById.FindRef(NodeGuid.ToString());
		UEdGraphPin* Pin = Node ? Node->FindPin(FName(*PinName)) : nullptr;
		const TSharedPtr<FJsonObject>* LiteralPtr = nullptr;
		if (!Pin || !Update->TryGetObjectField(Update->HasField(TEXT("default")) ? TEXT("default") : TEXT("value"), LiteralPtr)
			|| !LiteralPtr || !LiteralPtr->IsValid())
		{
			return Fail(TEXT("Prepared pin update no longer resolves"));
		}
		FCortexCommandResult DefaultError;
		JournalNodeState(Node, Journal);
		JournalPinDefault(Pin, Journal);
		if (!FCortexGraphPinDefaults::ApplyDefault(Pin, *LiteralPtr, DefaultError)) return Fail(DefaultError.ErrorMessage);
	}

	const TArray<TSharedPtr<FJsonValue>>& Connections = Prepared.NormalizedRequest->GetArrayField(TEXT("connections"));
	for (const TSharedPtr<FJsonValue>& Value : Connections)
	{
		const TSharedPtr<FJsonObject> Connection = Value->AsObject();
		const TSharedPtr<FJsonObject>* FromPtr = nullptr;
		const TSharedPtr<FJsonObject>* ToPtr = nullptr;
		if (!Connection.IsValid() || !Connection->TryGetObjectField(TEXT("from"), FromPtr) || !Connection->TryGetObjectField(TEXT("to"), ToPtr))
		{
			return Fail(TEXT("Prepared connection is invalid"));
		}
		FString FromId;
		FString FromPinName;
		FString ToId;
		FString ToPinName;
		bool bFromEntry = false;
		bool bToEntry = false;
		FCortexCommandResult EndpointError;
		if (!ParseEndpoint(*FromPtr, FromId, FromPinName, bFromEntry, EndpointError, TEXT("connection.from"))
			|| !ParseEndpoint(*ToPtr, ToId, ToPinName, bToEntry, EndpointError, TEXT("connection.to")))
		{
			return Fail(EndpointError.ErrorMessage);
		}
		UEdGraphNode* SourceNode = NodesById.FindRef(FromId);
		UEdGraphNode* TargetNode = NodesById.FindRef(ToId);
		UEdGraphPin* SourcePin = SourceNode ? SourceNode->FindPin(FName(*FromPinName)) : nullptr;
		UEdGraphPin* TargetPin = TargetNode ? TargetNode->FindPin(FName(*ToPinName)) : nullptr;
		if (!SourcePin || !TargetPin) return Fail(TEXT("Prepared connection endpoint no longer resolves"));
		// Capture touched node state before connecting: the engine derives a disabled event's
		// display state from its links and clears it as soon as the first link exists.
		JournalNodeState(SourceNode, Journal);
		JournalNodeState(TargetNode, Journal);
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema || Schema->CanCreateConnection(SourcePin, TargetPin).Response != CONNECT_RESPONSE_MAKE
			|| !Schema->TryCreateConnection(SourcePin, TargetPin))
		{
			return Fail(TEXT("Prepared connection is no longer directly safe"));
		}
		Journal.Links.Add({ SourceNode->NodeGuid, SourcePin->PinName, TargetNode->NodeGuid, TargetPin->PinName });
		if (ShouldInjectApplyFault(TEXT("first_link")))
		{
			return Fail(TEXT("Test fault injected after first link"));
		}
	}
	return true;
}

/**
 * Post-save persistence verification of the committed target package, without reloading the asset:
 * the package's on-disk filename must resolve to an existing file and the package must be clean
 * again. The failing check is named so the caller never has to guess which boundary failed.
 */
bool VerifySavedTargetPackage(UPackage* Package, const FString& Filename, FName& OutFailedCheck)
{
	OutFailedCheck = NAME_None;
	if (Filename.IsEmpty() || !IFileManager::Get().FileExists(*Filename))
	{
		OutFailedCheck = TEXT("asset_file");
		return false;
	}
	if (ShouldInjectPostSaveFault(TEXT("asset_file")))
	{
		OutFailedCheck = TEXT("asset_file");
		return false;
	}
	if (Package->IsDirty())
	{
		OutFailedCheck = TEXT("clean_package");
		return false;
	}
	if (ShouldInjectPostSaveFault(TEXT("clean_package")))
	{
		OutFailedCheck = TEXT("clean_package");
		return false;
	}
	return true;
}

/**
 * Commits the verified in-memory result of a coordinated patch to disk: exactly one save of the
 * target package and then post-save persistence verification. The asset is never reloaded and a
 * committed file is never rolled back; a failure reports the persistence phases honestly instead.
 */
bool SaveVerifiedTargetPackage(
	UBlueprint* Blueprint,
	FCortexGraphPatchOutcome& OutOutcome,
	FCortexCommandResult& OutError)
{
	UPackage* const Package = Blueprint->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaveReported = !ShouldInjectSaveFault()
		&& UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs);
	if (!bSaveReported)
	{
		// The disk commit did not happen: keep the verified in-memory result, keep the package dirty
		// and never claim that Undo reverted the file.
		OutOutcome.SaveStatus = TEXT("failed");
		OutOutcome.PostSaveStatus = TEXT("not_requested");
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, FString::Printf(
			TEXT("Graph patch applied and verified in memory, but saving '%s' failed; the verified in-memory result is preserved, the package stays dirty and nothing was rolled back"),
			*Package->GetName()));
		return false;
	}
	OutOutcome.SaveStatus = TEXT("saved");
	OutOutcome.bSaved = true;

	FName FailedCheck = NAME_None;
	if (!VerifySavedTargetPackage(Package, Filename, FailedCheck))
	{
		// The disk commit really happened, so the save result stays honest and only the persistence
		// verification is reported as failed; nothing is rolled back and the asset is never blocked.
		OutOutcome.PostSaveStatus = TEXT("failed");
		const FString Message = FString::Printf(
			TEXT("Post-save verification of '%s' failed after the file was committed; the in-memory result was not rolled back and the saved asset was not reloaded, so the asset must be reopened before further authoring"),
			*FailedCheck.ToString());
		OutOutcome.Diagnostics.Add(Message);
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::VerificationFailed, Message);
		return false;
	}
	OutOutcome.PostSaveStatus = TEXT("verified");
	return true;
}
}

/** Bounded compiler diagnostics: the total entry count and each entry length respect the bound. */
void FCortexGraphPatchOps::CollectCompilerDiagnostics(const FCompilerResultsLog& Log, TArray<FString>& OutDiagnostics)
{
	static const FString OmissionMarker = TEXT("additional compiler diagnostics omitted");
	static const FString Elision = TEXT("...");
	for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
	{
		const EMessageSeverity::Type Severity = Message->GetSeverity();
		if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning) continue;
		if (OutDiagnostics.Num() >= 16 - 1)
		{
			// Keep the total at or below 16 entries by reserving the final slot for the marker.
			if (!OutDiagnostics.Contains(OmissionMarker))
			{
				OutDiagnostics.Add(OmissionMarker);
			}
			break;
		}
		FString Diagnostic = Message->ToText().ToString();
		if (Diagnostic.Len() + Elision.Len() > 512)
		{
			Diagnostic = Diagnostic.Left(512 - Elision.Len()) + Elision;
		}
		OutDiagnostics.Add(MoveTemp(Diagnostic));
	}
}

/**
 * Enforces one shared final diagnostics bound over a whole outcome: at most 16 entries, a single
 * omission marker inside that bound, and at most 512 characters per entry including the suffix.
 */
void FCortexGraphPatchOps::TrimDiagnostics(TArray<FString>& InOutDiagnostics)
{
	static const FString OmissionMarker = TEXT("additional compiler diagnostics omitted");
	static const FString Elision = TEXT("...");
	for (FString& Diagnostic : InOutDiagnostics)
	{
		if (Diagnostic.Len() + Elision.Len() > 512)
		{
			Diagnostic = Diagnostic.Left(512 - Elision.Len()) + Elision;
		}
	}
	// Any pre-existing marker means the set is already truncated: it must survive the trim even
	// when the aggregate then fits, otherwise a truncated set is reported as complete.
	bool bTruncated = InOutDiagnostics.Remove(OmissionMarker) > 0;
	if (InOutDiagnostics.Num() > 16 - 1)
	{
		InOutDiagnostics.SetNum(16 - 1);
		bTruncated = true;
	}
	if (bTruncated)
	{
		InOutDiagnostics.Add(OmissionMarker);
	}
}

bool FCortexGraphPatchOps::ValidateEligibility(UBlueprint* Blueprint, FCortexCommandResult& OutError)
{
	OutError = FCortexCommandResult();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (Blueprint->ParentClass == nullptr || Blueprint->GeneratedClass == nullptr || Blueprint->Status == BS_BeingCreated)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Blueprint class context is unready: missing ParentClass or GeneratedClass"));
		return false;
	}
	if (Blueprint->Status == BS_Error)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Blueprint has pre-existing compiler errors; fix the asset before applying a typed patch"));
		return false;
	}
	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress()))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Typed graph patches cannot be applied while a play or simulate session is active"));
		return false;
	}
	return true;
}

bool FCortexGraphPatchOps::Apply(
	UBlueprint* Blueprint,
	const FCortexGraphPreparedPatch& Prepared,
	FCortexCommandResult& OutError)
{
	FGraphPatchJournal Journal;
	return ApplyPrepared(Blueprint, Prepared, Journal, nullptr, OutError);
}

bool FCortexGraphPatchOps::Execute(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	FCortexGraphPatchOutcome& OutOutcome,
	FCortexCommandResult& OutError)
{
	OutOutcome = FCortexGraphPatchOutcome();
	OutError = FCortexCommandResult();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}

	// Every terminal path reports the live before/after state of the asset, so a caller that never
	// learned the outcome can still reconcile by inspection, even after a refusal or a rollback.
	OutOutcome.FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	OutOutcome.bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	auto CaptureAfterState = [&]()
	{
		OutOutcome.FingerprintAfter = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
		OutOutcome.bDirtyAfter = Blueprint->GetOutermost()->IsDirty();
	};
	auto Refuse = [&]() -> bool
	{
		CaptureAfterState();
		return false;
	};

	if (!ValidateEligibility(Blueprint, OutError)) return Refuse();

	FCortexGraphPreparedPatch Prepared;
	if (!Preflight(Blueprint, Params, Prepared, OutError))
	{
		// Validation errors never mutate, never compile and never save.
		return Refuse();
	}
	OutOutcome.PatchId = Prepared.PatchId;
	OutOutcome.bChanged = Prepared.bChanged;
	OutOutcome.ReusedClientIds = Prepared.ReusedClientIds;
	OutOutcome.bReplayedWithAbsentSource = Prepared.bReplayedWithAbsentSource;
	if (Prepared.bReplayedWithAbsentSource)
	{
		OutOutcome.Diagnostics.Add(TEXT(
			"replay accepted with an absent source locator: the migration apply already consumed the stale entry this request names"));
	}
	OutOutcome.FingerprintBefore = Prepared.FingerprintBefore;
	if (Prepared.bDryRun)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("Execute requires an apply request (dry_run=false)"));
		return Refuse();
	}
	FString BlockReason;
	if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		return Refuse();
	}

	if (!Prepared.bChanged)
	{
		// An idempotent replay or an allow_noop request never mutates, never opens a transaction,
		// never compiles and never saves; it still reports the durable identities it reconciled by.
		OutOutcome.ApplyStatus = TEXT("unchanged");
		OutOutcome.Locators = MakePreparedLocators(Prepared);
		CaptureAfterState();
		return true;
	}

	FGraphPatchJournal Journal;
	if (!ApplyPrepared(Blueprint, Prepared, Journal, &OutOutcome, OutError))
	{
		// A failed apply still reports the durable identities it reached: a blocked or partially
		// applied asset must stay inspectable instead of returning empty residual identities.
		OutOutcome.Locators = Journal.Locators;
		return Refuse();
	}
	OutOutcome.ApplyStatus = TEXT("applied");
	OutOutcome.Locators = Journal.Locators;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	if (Prepared.bCompile)
	{
		Journal.bCompileAttempted = true;
		OutOutcome.TargetCompileCount = 1;
		TArray<FString> Diagnostics;
		const bool bCompiled = CompileTargetBlueprint(Blueprint, TEXT("target_compile"), Diagnostics);
		OutOutcome.Diagnostics.Append(Diagnostics);
		if (!bCompiled)
		{
			OutOutcome.CompileStatus = TEXT("failed");
			// HandleApplyFailure always reports the failure: it completes the recovery attempt itself.
			HandleApplyFailure(Blueprint, Prepared, Journal, &OutOutcome, OutError,
				FString::Printf(TEXT("Graph patch compilation failed: %s"), *FString::Join(Diagnostics, TEXT("; "))),
				CortexErrorCodes::CompileFailed, false);
			return Refuse();
		}
		OutOutcome.CompileStatus = TEXT("compiled");
	}

	FString ReadbackFailure;
	MutateNativeStateBeforeReadback(Blueprint);
	if (!VerifyAppliedState(Blueprint, Prepared, Journal, OutOutcome.CompileStatus == TEXT("compiled"), ReadbackFailure))
	{
		OutOutcome.ReadbackStatus = TEXT("mismatched");
		// HandleApplyFailure always reports the failure: it completes the recovery attempt itself.
		HandleApplyFailure(Blueprint, Prepared, Journal, &OutOutcome, OutError, ReadbackFailure,
			CortexErrorCodes::VerificationFailed, false);
		return Refuse();
	}
	OutOutcome.ReadbackStatus = TEXT("matched");

	// Only a verified in-memory result is ever persisted, and only when the request asked for it.
	if (Prepared.bSave && !SaveVerifiedTargetPackage(Blueprint, OutOutcome, OutError))
	{
		FCortexGraphPatchOps::TrimDiagnostics(OutOutcome.Diagnostics);
		return Refuse();
	}
	FCortexGraphPatchOps::TrimDiagnostics(OutOutcome.Diagnostics);
	CaptureAfterState();
	return true;
}


#if WITH_AUTOMATION_TESTS
void FCortexGraphPatchOps::SetApplyFaultPointForTesting(const FName Point)
{
	ApplyFaultPointForTesting = Point;
}

void FCortexGraphPatchOps::ClearApplyFaultPointForTesting()
{
	ApplyFaultPointForTesting = NAME_None;
}

void FCortexGraphPatchOps::SetReadbackFaultForTesting(const FName Field)
{
	ReadbackFaultForTesting = Field;
}

void FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(TFunction<void(UBlueprint*)> Mutator)
{
	PreReadbackMutatorForTesting = MoveTemp(Mutator);
}

void FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting()
{
	PreReadbackMutatorForTesting = nullptr;
}

void FCortexGraphPatchOps::SetOperationObserverForTesting(TFunction<void(FName, UBlueprint*)> Observer)
{
	OperationObserverForTesting = MoveTemp(Observer);
}

void FCortexGraphPatchOps::ClearOperationObserverForTesting()
{
	OperationObserverForTesting = nullptr;
}

void FCortexGraphPatchOps::SetSaveFaultForTesting(const bool bFail)
{
	bSaveFaultForTesting = bFail;
}

void FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(const FName Check)
{
	PostSaveVerificationFaultForTesting = Check;
}
#endif
