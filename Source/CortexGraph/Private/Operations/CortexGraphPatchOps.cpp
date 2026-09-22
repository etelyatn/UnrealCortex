#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphAuthoringContext.h"
#include "Operations/CortexGraphImplementationOps.h"
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
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Char.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr int32 MaxNodes = 64;
constexpr int32 MaxEdges = 256;
constexpr int32 MaxClientIdLength = 32;
constexpr int32 MaxRequestSize = 64 * 1024;
constexpr int32 MaxScannedNodes = 2048;

bool IsJsonType(const TSharedPtr<FJsonValue>& Value, EJson Expected)
{
	return Value.IsValid() && Value->Type == Expected;
}

bool HasOnlyFields(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Allowed, FCortexCommandResult& OutError, const FString& Context)
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

bool ReadStrictBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool DefaultValue, bool& OutValue, FCortexCommandResult& OutError)
{
	OutValue = DefaultValue;
	if (!Object->HasField(Field)) return true;
	if (!Object->TryGetBoolField(Field, OutValue))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a boolean"), *Field));
		return false;
	}
	return true;
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Object, const FString& Field, FString& OutValue, FCortexCommandResult& OutError)
{
	if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a non-empty string"), *Field));
		return false;
	}
	return true;
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


bool ParseGuidField(const TSharedPtr<FJsonObject>& Object, const FString& Field, FGuid& OutGuid, FCortexCommandResult& OutError)
{
	FString Value;
	if (!ReadRequiredString(Object, Field, Value, OutError) || !FGuid::Parse(Value, OutGuid))
	{
		if (OutError.bSuccess || OutError.ErrorCode.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be a valid GUID"), *Field));
		}
		return false;
	}
	return true;
}

bool ResolveGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid, const FString& SubgraphPath, UEdGraph*& OutGraph, FCortexCommandResult& OutError)
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
		if (OutDefaultPins.Contains(PinName))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Duplicate default pin"));
			return false;
		}
		OutDefaultPins.Add(PinName);
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
		if (!FCortexGraphPinDefaults::Validate(Pin, Literal, OutError)) return false;
	}
	return true;
}
void AddPlannedPinSignature(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NormalizedNode)
{
	if (!Node || !NormalizedNode.IsValid()) return;
	TArray<UEdGraphPin*> Pins;
	for (UEdGraphPin* Pin : Node->Pins) if (Pin) Pins.Add(Pin);
	Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) { return A.PinName.LexicalLess(B.PinName); });
	TArray<TSharedPtr<FJsonValue>> Serialized;
	for (const UEdGraphPin* Pin : Pins)
	{
		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Descriptor->SetNumberField(TEXT("direction"), static_cast<int32>(Pin->Direction));
		Descriptor->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		Descriptor->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
		Descriptor->SetStringField(TEXT("subobject"), Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : FString());
		Descriptor->SetBoolField(TEXT("reference"), Pin->PinType.bIsReference);
		Descriptor->SetBoolField(TEXT("const"), Pin->PinType.bIsConst);
		if (Pin->PinType.ContainerType == EPinContainerType::Map || !Pin->PinType.PinValueType.TerminalCategory.IsNone())
		{
			TSharedPtr<FJsonObject> Terminal = MakeShared<FJsonObject>();
			Terminal->SetStringField(TEXT("category"), Pin->PinType.PinValueType.TerminalCategory.ToString());
			Terminal->SetStringField(TEXT("subcategory"), Pin->PinType.PinValueType.TerminalSubCategory.ToString());
			Terminal->SetStringField(TEXT("subobject"), Pin->PinType.PinValueType.TerminalSubCategoryObject.IsValid() ? Pin->PinType.PinValueType.TerminalSubCategoryObject->GetPathName() : FString());
			Terminal->SetBoolField(TEXT("const"), Pin->PinType.PinValueType.bTerminalIsConst);
			Descriptor->SetObjectField(TEXT("map_terminal_type"), Terminal);
		}
		Serialized.Add(MakeShared<FJsonValueObject>(Descriptor));
	}
	NormalizedNode->SetArrayField(TEXT("resolved_pins"), Serialized);
}
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
	if (!HasOnlyFields(Params, { TEXT("asset_path"), TEXT("target"), TEXT("patch_id"), TEXT("expected_fingerprint"), TEXT("nodes"), TEXT("connections"), TEXT("pin_updates"), TEXT("dry_run"), TEXT("compile"), TEXT("save"), TEXT("allow_noop"), TEXT("expected_validation_hash") }, OutError, TEXT("patch request"))) return false;
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
	TMap<FString, UEdGraphNode*> PlannedNodes;
	TMap<FString, TSet<FString>> DefaultPins;
	UEdGraph* PlanningGraph = NewObject<UEdGraph>(Blueprint, NAME_None, RF_Transient);
	PlanningGraph->Schema = UEdGraphSchema_K2::StaticClass();
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
			PlannedNodes.Add(TEXT("entry"), ImplementationPlan.ExistingEntryNode);
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
		UEdGraphNode* PlannedNode = NewObject<UEdGraphNode>(PlanningGraph, ResolvedNodeClass, NAME_None, RF_Transient);
		PlannedNode->CreateNewGuid();
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
		if (TargetGraph && (!PlannedNode->IsCompatibleWithGraph(TargetGraph) || !PlannedNode->CanPasteHere(TargetGraph)))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Node class '%s' is incompatible with the selected graph"), *NodeClass));
			return false;
		}
		AddPlannedPinSignature(PlannedNode, NormalizedNode);
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
			UEdGraphPin* Pin = FindPlannedPin(PlannedNodes, NodeGuid.ToString(), PinName, TargetGraph, OutError);
			if (!Pin) return false;
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
			const FString InputKey = NodeGuid.ToString() + TEXT(".") + PinName;
			if (ExistingDefaultInputs.Contains(InputKey))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Duplicate pin_update target"));
				return false;
			}
			ExistingDefaultInputs.Add(InputKey);
			NormalizedPinUpdates.Add(MakeShared<FJsonValueObject>(Update));
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
		UEdGraphPin* SourcePin = FindPlannedPin(PlannedNodes, FromId, FromPinName, TargetGraph, OutError);
		UEdGraphPin* TargetPin = FindPlannedPin(PlannedNodes, ToId, ToPinName, TargetGraph, OutError);
		if (!SourcePin || !TargetPin) return false;
		if (SourcePin->Direction != EGPD_Output || TargetPin->Direction != EGPD_Input)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Connections must be output-to-input"));
			return false;
		}
		const FString InputKey = ToId + TEXT(".") + ToPinName;
		bool bHasPlannedDefault = false;
		for (const FString& DefaultPin : DefaultPins.FindRef(ToId))
		{
			if (DefaultPin.Equals(ToPinName, ESearchCase::IgnoreCase))
			{
				bHasPlannedDefault = true;
				break;
			}
		}
		if (ConnectedInputs.Contains(InputKey) || ExistingDefaultInputs.Contains(InputKey) || bHasPlannedDefault || TargetPin->LinkedTo.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Input '%s' has competing connection/default"), *InputKey));
			return false;
		}
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
			if (SourcePin->GetOwningNode()->GetTypedOuter<UEdGraph>() == PlanningGraph && TargetPin->GetOwningNode()->GetTypedOuter<UEdGraph>() == PlanningGraph
				&& !Schema->TryCreateConnection(SourcePin, TargetPin))
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
	OutPrepared.bChanged = Nodes->Num() > 0 || Connections->Num() > 0 || (PinUpdates && PinUpdates->Num() > 0) || bImplementationWouldCreate;
	if (TargetGraph && !OutPrepared.bChanged && !bAllowNoop)
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
