#include "Operations/CortexGraphInterfaceMigration.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "IO/IoHash.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace
{
constexpr int32 MaxMappedFunctions = 64;
constexpr int32 MaxDerivedBlueprints = 512;
constexpr int32 MaxReportedSites = 8;
const TCHAR* const InterfaceOp = TEXT("replace_interface");
const TCHAR* const FunctionGraphKind = TEXT("function_graph");
const TCHAR* const EventKind = TEXT("event");
const TCHAR* const DetachedSuffix = TEXT("__CortexDetached");

bool IsSameInterface(const UClass* Candidate, const UClass* Interface)
{
	return Candidate != nullptr && Interface != nullptr
		&& (Candidate == Interface || Candidate->GetAuthoritativeClass() == Interface->GetAuthoritativeClass());
}

int32 FindDescriptionIndex(const UBlueprint* Blueprint, const UClass* Interface)
{
	for (int32 Index = 0; Index < Blueprint->ImplementedInterfaces.Num(); ++Index)
	{
		if (IsSameInterface(Blueprint->ImplementedInterfaces[Index].Interface, Interface))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

/** The functions an interface declares: the same set the reflect inventory reports. */
TArray<UFunction*> InterfaceFunctions(const UClass* Interface)
{
	TArray<UFunction*> Functions;
	for (TFieldIterator<UFunction> It(Interface, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* const Function = *It;
		const UClass* const Owner = Function->GetOwnerClass();
		if (!Owner || Owner == UInterface::StaticClass() || Owner == UObject::StaticClass()
			|| Function->GetName().StartsWith(TEXT("ExecuteUbergraph")))
		{
			continue;
		}
		Functions.Add(Function);
	}
	Functions.Sort([](const UFunction& A, const UFunction& B) { return A.GetName() < B.GetName(); });
	return Functions;
}

/** The engine's own rule (`ImplementNewInterface`): a function that cannot be placed as an event owns a graph. */
bool IsFunctionStyle(const UFunction* Function)
{
	return UEdGraphSchema_K2::CanKismetOverrideFunction(Function) && !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function);
}

bool IsEventNode(const UEdGraphNode* Node, const FName Function, const UClass* Interface)
{
	const UK2Node_Event* const Event = Cast<UK2Node_Event>(Node);
	return Event != nullptr && !Event->IsA<UK2Node_CustomEvent>() && Event->EventReference.GetMemberName() == Function
		&& IsSameInterface(Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode()), Interface);
}

FString Guid(const FGuid& Value)
{
	return Value.ToString();
}

TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

bool ReadStrings(const TSharedPtr<FJsonObject>& Source, const TCHAR* Field, TArray<FString>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Source->TryGetArrayField(Field, Values) || !Values)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString String;
		if (!Value.IsValid() || !Value->TryGetString(String))
		{
			return false;
		}
		Out.Add(MoveTemp(String));
	}
	return true;
}

/** A pin value that is the type's plain default: nothing an author set. */
bool IsPlainDefault(const UEdGraphPin* Pin)
{
	if (Pin->DefaultObject != nullptr || !Pin->DefaultTextValue.IsEmpty())
	{
		return false;
	}
	const FString& Value = Pin->DefaultValue;
	if (Value.IsEmpty() || Value == TEXT("false") || Value == TEXT("None"))
	{
		return true;
	}
	for (const TCHAR Char : Value)
	{
		if (!FChar::IsWhitespace(Char) && Char != TEXT('0') && Char != TEXT('.') && Char != TEXT(',') && Char != TEXT('(') && Char != TEXT(')') && Char != TEXT('+') && Char != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

/** True when the function declares an output, so its graph owns a result terminator. */
bool FunctionHasResult(const UFunction* Function)
{
	for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_OutParm))
		{
			return true;
		}
	}
	return false;
}

/**
 * A function graph carries an implementation when it owns a node besides its terminators, or when the terminators themselves
 * carry authored behavior: a data link on either of them (a parameter passthrough) or a non-default result literal. An engine-made
 * empty stub has none of these and stays droppable.
 */
bool GraphHasBody(const UEdGraph* Graph)
{
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		const bool bEntry = Cast<UK2Node_FunctionEntry>(Node) != nullptr;
		const bool bResult = Cast<UK2Node_FunctionResult>(Node) != nullptr;
		if (!bEntry && !bResult)
		{
			return true;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->ParentPin != nullptr || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (Pin->LinkedTo.Num() > 0 || (bResult && Pin->Direction == EGPD_Input && !IsPlainDefault(Pin)))
			{
				return true;
			}
		}
	}
	return false;
}

/** An event carries an implementation when anything is linked to it. */
bool EventHasLinks(const UK2Node_Event* Event)
{
	for (const UEdGraphPin* Pin : Event->Pins)
	{
		if (Pin && Pin->LinkedTo.Num() > 0)
		{
			return true;
		}
	}
	return false;
}

struct FOldFunction
{
	UFunction* Function = nullptr;
	bool bEventStyle = false;
	UEdGraph* Graph = nullptr;
	UK2Node_Event* Event = nullptr;
	bool bHasImplementation = false;
	bool bImplemented = false;
};

FString NodeLabel(const UEdGraphNode* Node)
{
	return FString::Printf(TEXT("%s%s %s in graph '%s'"), Node->GetClass()->GetPrefixCPP(), *Node->GetClass()->GetName(), *Node->NodeGuid.ToString(),
		Node->GetGraph() ? *Node->GetGraph()->GetName() : TEXT("<none>"));
}

/** In-asset references to the old interface: message and typed calls of its functions and casts to it. */
void CollectOldInterfaceSites(UBlueprint* Blueprint, const UClass* OldInterface, TArray<FString>& OutSites)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node)) continue;
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				if (IsSameInterface(Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode()), OldInterface))
				{
					OutSites.Add(FString::Printf(TEXT("%s calls '%s'"), *NodeLabel(Node), *Call->FunctionReference.GetMemberName().ToString()));
				}
			}
			else if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				if (IsSameInterface(CastNode->TargetType, OldInterface))
				{
					OutSites.Add(FString::Printf(TEXT("%s casts to the old interface"), *NodeLabel(Node)));
				}
			}
		}
	}
}

/** Derived Blueprints (loaded ones and the registry's unloaded ones) that own an override of a mapped function. */
bool CollectDerivedOverrides(
	UBlueprint* Blueprint,
	const UClass* OldInterface,
	const TSet<FName>& MappedFunctions,
	TArray<FString>& OutOverrides,
	FCortexCommandResult& OutError)
{
	TMap<FString, UBlueprint*> Derived;
	UClass* const BlueprintClass = Blueprint->GeneratedClass;
	if (!BlueprintClass)
	{
		return true;
	}
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* const Candidate = *It;
		// A derived Blueprint owns a strict subclass; transient planning Blueprints share this asset's class and are not assets.
		if (Candidate && Candidate != Blueprint && IsValid(Candidate) && !Candidate->HasAnyFlags(RF_Transient) && Candidate->GeneratedClass
			&& Candidate->GeneratedClass != BlueprintClass && Candidate->GeneratedClass->IsChildOf(BlueprintClass))
		{
			Derived.Add(Candidate->GetPathName(), Candidate);
		}
	}
	if (IAssetRegistry* const Registry = IAssetRegistry::Get())
	{
		TSet<FTopLevelAssetPath> DerivedClassNames;
		Registry->GetDerivedClassNames({ BlueprintClass->GetClassPathName() }, TSet<FTopLevelAssetPath>(), DerivedClassNames);
		TArray<FTopLevelAssetPath> Sorted = DerivedClassNames.Array();
		Sorted.Sort([](const FTopLevelAssetPath& A, const FTopLevelAssetPath& B) { return A.ToString() < B.ToString(); });
		for (const FTopLevelAssetPath& ClassPath : Sorted)
		{
			FString AssetName = ClassPath.GetAssetName().ToString();
			if (!AssetName.RemoveFromEnd(TEXT("_C")))
			{
				continue;
			}
			const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *ClassPath.GetPackageName().ToString(), *AssetName);
			if (Derived.Contains(ObjectPath))
			{
				continue;
			}
			UBlueprint* Loaded = FindObject<UBlueprint>(nullptr, *ObjectPath);
			if (!Loaded)
			{
				const FString PackageName = ClassPath.GetPackageName().ToString();
				if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
				{
					continue;
				}
				Loaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
			}
			if (Loaded && Loaded != Blueprint)
			{
				Derived.Add(ObjectPath, Loaded);
			}
		}
	}
	if (Derived.Num() > MaxDerivedBlueprints)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
			FString::Printf(TEXT("replace_interface found %d Blueprints derived from this asset, over the bounded budget of %d; migrate the derived Blueprints first"),
				Derived.Num(), MaxDerivedBlueprints));
		return false;
	}
	TArray<FString> Paths;
	Derived.GenerateKeyArray(Paths);
	Paths.Sort();
	for (const FString& Path : Paths)
	{
		UBlueprint* const Child = Derived[Path];
		if (FindDescriptionIndex(Child, OldInterface) != INDEX_NONE)
		{
			OutOverrides.Add(FString::Printf(TEXT("%s re-implements the old interface"), *Path));
		}
		for (const UEdGraph* Graph : Child->FunctionGraphs)
		{
			if (Graph && MappedFunctions.Contains(Graph->GetFName()))
			{
				OutOverrides.Add(FString::Printf(TEXT("%s overrides '%s' with a function graph"), *Path, *Graph->GetName()));
			}
		}
		for (const UEdGraph* Graph : Child->UbergraphPages)
		{
			if (!Graph) continue;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Event* const Event = Cast<UK2Node_Event>(Node);
				if (Event && !Event->IsA<UK2Node_CustomEvent>() && Event->bOverrideFunction
					&& MappedFunctions.Contains(Event->EventReference.GetMemberName()))
				{
					OutOverrides.Add(FString::Printf(TEXT("%s overrides '%s' with an event"), *Path, *Event->EventReference.GetMemberName().ToString()));
				}
			}
		}
	}
	return true;
}

FString JoinBounded(const TArray<FString>& Items)
{
	TArray<FString> Shown;
	for (int32 Index = 0; Index < FMath::Min(Items.Num(), MaxReportedSites); ++Index)
	{
		Shown.Add(Items[Index]);
	}
	FString Text = FString::Join(Shown, TEXT("; "));
	if (Items.Num() > MaxReportedSites)
	{
		Text += FString::Printf(TEXT("; and %d more"), Items.Num() - MaxReportedSites);
	}
	return Text;
}

UEdGraph* FindGraphInDescriptionByGuid(FBPInterfaceDescription& Description, const FGuid& GraphGuid)
{
	for (UEdGraph* Graph : Description.Graphs)
	{
		if (Graph && Graph->GraphGuid == GraphGuid)
		{
			return Graph;
		}
	}
	return nullptr;
}

UObject* FindObjectNamed(UObject* Outer, const FName Name)
{
	return StaticFindObject(UObject::StaticClass(), Outer, *Name.ToString(), EFindObjectFlags::None);
}
}

TSharedPtr<FJsonObject> FCortexGraphInterfaceFunctionPlan::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("from"), From);
	Json->SetStringField(TEXT("to"), To);
	Json->SetStringField(TEXT("implementation"), Implementation);
	Json->SetBoolField(TEXT("to_is_event"), bToIsEvent);
	Json->SetBoolField(TEXT("to_has_result"), bToHasResult);
	Json->SetStringField(TEXT("source_graph_guid"), SourceGraphGuid);
	Json->SetStringField(TEXT("source_graph_name"), SourceGraphName);
	Json->SetStringField(TEXT("source_entry_guid"), SourceEntryGuid);
	Json->SetStringField(TEXT("source_result_guid"), SourceResultGuid);
	Json->SetNumberField(TEXT("node_count"), NodeCount);
	Json->SetStringField(TEXT("entry_identity"), EntryIdentity);
	Json->SetStringField(TEXT("result_identity"), ResultIdentity);
	Json->SetArrayField(TEXT("pin_map"), PinMap);
	TArray<TSharedPtr<FJsonValue>> DefaultValues;
	for (const FCortexGraphInterfaceResultDefault& Default : ResultDefaults)
	{
		TSharedPtr<FJsonObject> DefaultJson = MakeShared<FJsonObject>();
		DefaultJson->SetStringField(TEXT("to_pin"), Default.ToPin);
		DefaultJson->SetStringField(TEXT("value"), Default.Value);
		DefaultJson->SetStringField(TEXT("object"), Default.ObjectPath);
		DefaultJson->SetStringField(TEXT("text"), Default.Text);
		DefaultValues.Add(MakeShared<FJsonValueObject>(DefaultJson));
	}
	Json->SetArrayField(TEXT("result_defaults"), DefaultValues);
	return Json;
}

bool FCortexGraphInterfaceFunctionPlan::FromJson(
	const TSharedPtr<FJsonObject>& Source, FCortexGraphInterfaceFunctionPlan& OutPlan, FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphInterfaceFunctionPlan();
	const TArray<TSharedPtr<FJsonValue>>* PinMapValues = nullptr;
	double NodeCountValue = 0;
	if (!Source.IsValid()
		|| !Source->TryGetStringField(TEXT("from"), OutPlan.From)
		|| !Source->TryGetStringField(TEXT("to"), OutPlan.To)
		|| !Source->TryGetStringField(TEXT("implementation"), OutPlan.Implementation)
		|| !Source->TryGetBoolField(TEXT("to_is_event"), OutPlan.bToIsEvent)
		|| !Source->TryGetBoolField(TEXT("to_has_result"), OutPlan.bToHasResult)
		|| !Source->TryGetStringField(TEXT("source_graph_guid"), OutPlan.SourceGraphGuid)
		|| !Source->TryGetStringField(TEXT("source_graph_name"), OutPlan.SourceGraphName)
		|| !Source->TryGetStringField(TEXT("source_entry_guid"), OutPlan.SourceEntryGuid)
		|| !Source->TryGetStringField(TEXT("source_result_guid"), OutPlan.SourceResultGuid)
		|| !Source->TryGetNumberField(TEXT("node_count"), NodeCountValue)
		|| !Source->TryGetStringField(TEXT("entry_identity"), OutPlan.EntryIdentity)
		|| !Source->TryGetStringField(TEXT("result_identity"), OutPlan.ResultIdentity)
		|| !Source->TryGetArrayField(TEXT("pin_map"), PinMapValues) || !PinMapValues)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared replace_interface function plan is incomplete"));
		return false;
	}
	OutPlan.NodeCount = static_cast<int32>(NodeCountValue);
	OutPlan.PinMap = *PinMapValues;
	const TArray<TSharedPtr<FJsonValue>>* DefaultValues = nullptr;
	if (Source->TryGetArrayField(TEXT("result_defaults"), DefaultValues) && DefaultValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *DefaultValues)
		{
			const TSharedPtr<FJsonObject> Json = Value.IsValid() ? Value->AsObject() : nullptr;
			FCortexGraphInterfaceResultDefault Default;
			if (!Json.IsValid() || !Json->TryGetStringField(TEXT("to_pin"), Default.ToPin))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared replace_interface function plan has an invalid result default"));
				return false;
			}
			Json->TryGetStringField(TEXT("value"), Default.Value);
			Json->TryGetStringField(TEXT("object"), Default.ObjectPath);
			Json->TryGetStringField(TEXT("text"), Default.Text);
			OutPlan.ResultDefaults.Add(MoveTemp(Default));
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCortexGraphMigrationInterfacePlan::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("op"), Op);
	Json->SetStringField(TEXT("old_interface"), OldInterface);
	Json->SetStringField(TEXT("new_interface"), NewInterface);
	Json->SetBoolField(TEXT("reused"), bReused);
	Json->SetNumberField(TEXT("scanned_nodes"), ScannedNodes);
	TArray<TSharedPtr<FJsonValue>> FunctionValues;
	for (const FCortexGraphInterfaceFunctionPlan& Function : Functions)
	{
		FunctionValues.Add(MakeShared<FJsonValueObject>(Function.ToJson()));
	}
	Json->SetArrayField(TEXT("functions"), FunctionValues);
	TArray<TSharedPtr<FJsonValue>> StubValues;
	for (const FCortexGraphInterfaceDroppedStub& Stub : DroppedStubs)
	{
		TSharedPtr<FJsonObject> StubJson = MakeShared<FJsonObject>();
		StubJson->SetStringField(TEXT("function"), Stub.Function);
		StubJson->SetStringField(TEXT("kind"), Stub.Kind);
		StubJson->SetStringField(TEXT("graph_guid"), Stub.GraphGuid);
		StubJson->SetStringField(TEXT("graph_name"), Stub.GraphName);
		StubJson->SetStringField(TEXT("node_guid"), Stub.NodeGuid);
		StubValues.Add(MakeShared<FJsonValueObject>(StubJson));
	}
	Json->SetArrayField(TEXT("dropped_stubs"), StubValues);
	Json->SetArrayField(TEXT("new_stubs"), StringValues(NewStubFunctions));
	TArray<TSharedPtr<FJsonValue>> PreservationValues;
	for (const FCortexGraphTransferPreservation& Preservation : Preservations)
	{
		TSharedPtr<FJsonObject> PreservationJson = MakeShared<FJsonObject>();
		PreservationJson->SetStringField(TEXT("label"), Preservation.Label);
		PreservationJson->SetStringField(TEXT("graph_guid"), Preservation.GraphGuid);
		PreservationJson->SetStringField(TEXT("capture"), Preservation.Capture);
		PreservationJson->SetArrayField(TEXT("excluded_guids"), StringValues(Preservation.ExcludedGuids));
		PreservationValues.Add(MakeShared<FJsonValueObject>(PreservationJson));
	}
	Json->SetArrayField(TEXT("preservations"), PreservationValues);
	return Json;
}

bool FCortexGraphMigrationInterfacePlan::FromJson(
	const TSharedPtr<FJsonObject>& Source, FCortexGraphMigrationInterfacePlan& OutPlan, FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationInterfacePlan();
	OutError = FCortexCommandResult();
	const TArray<TSharedPtr<FJsonValue>>* FunctionValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* StubValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PreservationValues = nullptr;
	double ScannedValue = 0;
	if (!Source.IsValid()
		|| !Source->TryGetStringField(TEXT("op"), OutPlan.Op) || OutPlan.Op != InterfaceOp
		|| !Source->TryGetStringField(TEXT("old_interface"), OutPlan.OldInterface)
		|| !Source->TryGetStringField(TEXT("new_interface"), OutPlan.NewInterface)
		|| !Source->TryGetBoolField(TEXT("reused"), OutPlan.bReused)
		|| !Source->TryGetNumberField(TEXT("scanned_nodes"), ScannedValue)
		|| !Source->TryGetArrayField(TEXT("functions"), FunctionValues) || !FunctionValues
		|| !Source->TryGetArrayField(TEXT("dropped_stubs"), StubValues) || !StubValues
		|| !ReadStrings(Source, TEXT("new_stubs"), OutPlan.NewStubFunctions)
		|| !Source->TryGetArrayField(TEXT("preservations"), PreservationValues) || !PreservationValues)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared replace_interface plan is incomplete"));
		return false;
	}
	OutPlan.ScannedNodes = static_cast<int32>(ScannedValue);
	for (const TSharedPtr<FJsonValue>& Value : *FunctionValues)
	{
		FCortexGraphInterfaceFunctionPlan Function;
		if (!FCortexGraphInterfaceFunctionPlan::FromJson(Value.IsValid() ? Value->AsObject() : nullptr, Function, OutError))
		{
			return false;
		}
		OutPlan.Functions.Add(MoveTemp(Function));
	}
	for (const TSharedPtr<FJsonValue>& Value : *StubValues)
	{
		const TSharedPtr<FJsonObject> StubJson = Value.IsValid() ? Value->AsObject() : nullptr;
		FCortexGraphInterfaceDroppedStub Stub;
		if (!StubJson.IsValid()
			|| !StubJson->TryGetStringField(TEXT("function"), Stub.Function)
			|| !StubJson->TryGetStringField(TEXT("kind"), Stub.Kind)
			|| !StubJson->TryGetStringField(TEXT("graph_guid"), Stub.GraphGuid)
			|| !StubJson->TryGetStringField(TEXT("graph_name"), Stub.GraphName)
			|| !StubJson->TryGetStringField(TEXT("node_guid"), Stub.NodeGuid))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared replace_interface plan has an invalid dropped stub"));
			return false;
		}
		OutPlan.DroppedStubs.Add(MoveTemp(Stub));
	}
	for (const TSharedPtr<FJsonValue>& Value : *PreservationValues)
	{
		const TSharedPtr<FJsonObject> PreservationJson = Value.IsValid() ? Value->AsObject() : nullptr;
		FCortexGraphTransferPreservation Preservation;
		if (!PreservationJson.IsValid()
			|| !PreservationJson->TryGetStringField(TEXT("label"), Preservation.Label)
			|| !PreservationJson->TryGetStringField(TEXT("graph_guid"), Preservation.GraphGuid)
			|| !PreservationJson->TryGetStringField(TEXT("capture"), Preservation.Capture)
			|| !ReadStrings(PreservationJson, TEXT("excluded_guids"), Preservation.ExcludedGuids))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared replace_interface plan has an invalid preservation contract"));
			return false;
		}
		OutPlan.Preservations.Add(MoveTemp(Preservation));
	}
	return true;
}

void FCortexGraphInterfaceStateCapture::Capture(UBlueprint* Blueprint)
{
	Descriptions.Reset();
	AddedGraphs.Reset();
	for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
	{
		FDescriptionState State;
		State.Interface = Description.Interface.Get();
		for (UEdGraph* Graph : Description.Graphs)
		{
			FGraphState GraphState;
			GraphState.Graph = TStrongObjectPtr<UEdGraph>(Graph);
			if (Graph)
			{
				GraphState.Name = Graph->GetFName();
				GraphState.GraphGuid = Graph->GraphGuid;
				GraphState.InterfaceGuid = Graph->InterfaceGuid;
			}
			State.Graphs.Add(MoveTemp(GraphState));
		}
		Descriptions.Add(MoveTemp(State));
	}
	bCaptured = true;
}

bool FCortexGraphInterfaceStateCapture::Restore(UBlueprint* Blueprint, FString& OutFailure)
{
	if (!bCaptured)
	{
		return true;
	}
	// The graphs the swap created leave the Blueprint first: their names may be the ones a restored graph owns again.
	for (const TStrongObjectPtr<UEdGraph>& Added : AddedGraphs)
	{
		UEdGraph* const Graph = Added.Get();
		if (!Graph || Graph->GetOuter() == GetTransientPackage())
		{
			continue;
		}
		const FName Unique = MakeUniqueObjectName(GetTransientPackage(), UEdGraph::StaticClass(), Graph->GetFName());
		Graph->Rename(*Unique.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	}
	// Two phases: every graph whose name changed first steps aside to a unique name, then takes its captured name. Restoring one
	// by one could find a captured name still held by another captured graph (a swap, or a detached stub and its adopter).
	const ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
	for (const FDescriptionState& Description : Descriptions)
	{
		for (const FGraphState& GraphState : Description.Graphs)
		{
			UEdGraph* const Graph = GraphState.Graph.Get();
			if (!Graph)
			{
				OutFailure = TEXT("a graph of the interface list no longer exists");
				return false;
			}
			if (Graph->GetFName() != GraphState.Name)
			{
				const FName Aside = MakeUniqueObjectName(Graph->GetOuter(), UEdGraph::StaticClass(), FName(*(GraphState.Name.ToString() + TEXT("__CortexRestore"))));
				Graph->Rename(*Aside.ToString(), Graph->GetOuter(), RenameFlags);
			}
		}
	}
	for (const FDescriptionState& Description : Descriptions)
	{
		for (const FGraphState& GraphState : Description.Graphs)
		{
			UEdGraph* const Graph = GraphState.Graph.Get();
			if (Graph->GetFName() != GraphState.Name)
			{
				if (UObject* const Occupant = FindObjectNamed(Graph->GetOuter(), GraphState.Name); Occupant && Occupant != Graph)
				{
					OutFailure = FString::Printf(TEXT("the graph name '%s' is held by another object, so the interface graph cannot be renamed back"), *GraphState.Name.ToString());
					return false;
				}
				Graph->Rename(*GraphState.Name.ToString(), Graph->GetOuter(), RenameFlags);
				if (Graph->GetFName() != GraphState.Name)
				{
					OutFailure = FString::Printf(TEXT("the interface graph '%s' could not be renamed back"), *GraphState.Name.ToString());
					return false;
				}
			}
			Graph->InterfaceGuid = GraphState.InterfaceGuid;
		}
	}
	Blueprint->ImplementedInterfaces.Reset();
	for (const FDescriptionState& Description : Descriptions)
	{
		FBPInterfaceDescription Restored;
		Restored.Interface = Description.Interface.Get();
		if (!Restored.Interface)
		{
			OutFailure = TEXT("an interface class of the captured list no longer exists");
			return false;
		}
		for (const FGraphState& GraphState : Description.Graphs)
		{
			Restored.Graphs.Add(GraphState.Graph.Get());
		}
		Blueprint->ImplementedInterfaces.Add(MoveTemp(Restored));
	}
	return true;
}

bool FCortexGraphInterfaceStateCapture::Matches(const UBlueprint* Blueprint, FString& OutFailure) const
{
	if (!bCaptured)
	{
		return true;
	}
	if (Blueprint->ImplementedInterfaces.Num() != Descriptions.Num())
	{
		OutFailure = FString::Printf(TEXT("the interface list has %d entries, expected %d"), Blueprint->ImplementedInterfaces.Num(), Descriptions.Num());
		return false;
	}
	for (int32 Index = 0; Index < Descriptions.Num(); ++Index)
	{
		const FBPInterfaceDescription& Live = Blueprint->ImplementedInterfaces[Index];
		const FDescriptionState& Expected = Descriptions[Index];
		if (Live.Interface.Get() != Expected.Interface.Get() || Live.Graphs.Num() != Expected.Graphs.Num())
		{
			OutFailure = FString::Printf(TEXT("interface entry %d does not match the captured interface or graph count"), Index);
			return false;
		}
		for (int32 GraphIndex = 0; GraphIndex < Expected.Graphs.Num(); ++GraphIndex)
		{
			const UEdGraph* const Graph = Live.Graphs[GraphIndex];
			const FGraphState& GraphState = Expected.Graphs[GraphIndex];
			if (Graph != GraphState.Graph.Get() || !Graph || Graph->GetFName() != GraphState.Name
				|| Graph->GraphGuid != GraphState.GraphGuid || Graph->InterfaceGuid != GraphState.InterfaceGuid)
			{
				OutFailure = FString::Printf(TEXT("interface graph %d of entry %d does not match its captured name, GUID or InterfaceGuid"), GraphIndex, Index);
				return false;
			}
		}
	}
	return true;
}

FString FCortexGraphInterfaceMigration::ClassPath(const UClass* Interface)
{
	return Interface ? Interface->GetAuthoritativeClass()->GetPathName() : FString();
}

bool FCortexGraphInterfaceMigration::ResolveClassIdentifier(const FString& Identifier, UClass*& OutClass, FCortexCommandResult& OutError)
{
	OutClass = nullptr;
	UClass* Resolved = nullptr;
	FCortexCommandResult ResolveError;
	bool bResolved = FCortexGraphSymbolResolver::ResolveClass(Identifier, Resolved, ResolveError);
	if (!bResolved && Identifier.StartsWith(TEXT("/")) && !Identifier.Contains(TEXT(".")))
	{
		// A bare asset path names the Blueprint class by its package: try its asset object.
		const FString Asset = Identifier + TEXT(".") + FPackageName::GetShortName(Identifier);
		FCortexCommandResult RetryError;
		bResolved = FCortexGraphSymbolResolver::ResolveClass(Asset, Resolved, RetryError);
	}
	if (!bResolved || !Resolved)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' does not resolve to a class: %s"), *Identifier, *ResolveError.ErrorMessage));
		return false;
	}
	OutClass = const_cast<UClass*>(Resolved->GetAuthoritativeClass());
	return true;
}

bool FCortexGraphInterfaceMigration::ResolveInterfaceClass(const FString& Identifier, UClass*& OutClass, FCortexCommandResult& OutError)
{
	OutClass = nullptr;
	UClass* Resolved = nullptr;
	FCortexCommandResult ResolveError;
	if (!ResolveClassIdentifier(Identifier, Resolved, ResolveError))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("interface ") + ResolveError.ErrorMessage);
		return false;
	}
	if (!Resolved->HasAnyClassFlags(CLASS_Interface))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is not an interface class"), *Identifier));
		return false;
	}
	OutClass = Resolved;
	return true;
}

void FCortexGraphInterfaceMigration::BuildEntryMigration(
	const FCortexGraphInterfaceFunctionPlan& Function,
	const FCortexGraphMigrationInterfacePlan& Plan,
	TSharedPtr<FJsonObject>& OutMigration,
	TSharedPtr<FJsonObject>& OutSelector)
{
	OutMigration = MakeShared<FJsonObject>();
	OutMigration->SetStringField(TEXT("op"), TEXT("replace_entry"));
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Function.SourceGraphGuid);
	Source->SetObjectField(TEXT("graph_ref"), GraphRef);
	Source->SetStringField(TEXT("entry_node_guid"), Function.SourceEntryGuid);
	OutMigration->SetObjectField(TEXT("source"), Source);
	OutMigration->SetArrayField(TEXT("pin_map"), Function.PinMap);
	OutSelector = MakeShared<FJsonObject>();
	OutSelector->SetStringField(TEXT("owner_class"), Plan.NewInterface);
	OutSelector->SetStringField(TEXT("function_name"), Function.To);
}

bool FCortexGraphInterfaceMigration::PlanInterfaceReplacement(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Migration,
	const FString& PatchId,
	FCortexGraphMigrationInterfacePlan& OutPlan,
	bool& bOutReused,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationInterfacePlan();
	OutError = FCortexCommandResult();
	bOutReused = false;
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Migration.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(Migration,
		{ TEXT("op"), TEXT("old_interface"), TEXT("new_interface"), TEXT("function_map") }, OutError, TEXT("migration")))
	{
		return false;
	}
	FString Op, OldIdentifier, NewIdentifier;
	if (!FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("op"), Op, OutError)
		|| !FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("old_interface"), OldIdentifier, OutError)
		|| !FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("new_interface"), NewIdentifier, OutError))
	{
		return false;
	}
	if (Op != InterfaceOp)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation, TEXT("migration.op must be replace_interface"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* MapValues = nullptr;
	if (!Migration->TryGetArrayField(TEXT("function_map"), MapValues) || !MapValues)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.function_map must be an array"));
		return false;
	}
	if (MapValues->Num() > MaxMappedFunctions)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
			FString::Printf(TEXT("migration.function_map exceeds the bounded function limit of %d"), MaxMappedFunctions));
		return false;
	}
	UClass* OldInterface = nullptr;
	UClass* NewInterface = nullptr;
	if (!ResolveInterfaceClass(OldIdentifier, OldInterface, OutError) || !ResolveInterfaceClass(NewIdentifier, NewInterface, OutError))
	{
		return false;
	}
	if (OldInterface == NewInterface)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("old_interface and new_interface name the same interface"));
		return false;
	}
	OutPlan.Op = InterfaceOp;
	OutPlan.OldInterface = ClassPath(OldInterface);
	OutPlan.NewInterface = ClassPath(NewInterface);

	// The request entries: {from, to?, pin_map}.
	struct FRequestEntry
	{
		FName From;
		FName To;
		TArray<TSharedPtr<FJsonValue>> PinMap;
	};
	TArray<FRequestEntry> Requested;
	TSet<FName> Froms;
	TSet<FName> Tos;
	for (const TSharedPtr<FJsonValue>& Value : *MapValues)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.function_map entries must be objects"));
			return false;
		}
		if (!FCortexGraphPatchOps::HasOnlyFields(Entry, { TEXT("from"), TEXT("to"), TEXT("pin_map") }, OutError, TEXT("migration.function_map entry")))
		{
			return false;
		}
		FString From, To;
		if (!FCortexGraphPatchOps::ReadRequiredString(Entry, TEXT("from"), From, OutError))
		{
			return false;
		}
		To = From;
		if (Entry->HasField(TEXT("to")) && !FCortexGraphPatchOps::ReadRequiredString(Entry, TEXT("to"), To, OutError))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* PinMapValues = nullptr;
		if (!Entry->TryGetArrayField(TEXT("pin_map"), PinMapValues) || !PinMapValues)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("migration.function_map entry '%s' needs a pin_map array"), *From));
			return false;
		}
		FRequestEntry Parsed;
		Parsed.From = FName(*From);
		Parsed.To = FName(*To);
		Parsed.PinMap = *PinMapValues;
		if (Froms.Contains(Parsed.From) || Tos.Contains(Parsed.To))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("migration.function_map names '%s' -> '%s' twice (every from and every to may appear once)"), *From, *To));
			return false;
		}
		Froms.Add(Parsed.From);
		Tos.Add(Parsed.To);
		Requested.Add(MoveTemp(Parsed));
	}

	const int32 OldIndex = FindDescriptionIndex(Blueprint, OldInterface);
	const int32 NewIndex = FindDescriptionIndex(Blueprint, NewInterface);

	// Replay: the swap is already applied; prove it exactly instead of re-planning.
	if (OldIndex == INDEX_NONE && NewIndex != INDEX_NONE)
	{
		TArray<FCortexGraphMigrationPlan> EntryPlans;
		for (const FRequestEntry& Entry : Requested)
		{
			UFunction* const NewFunction = NewInterface->FindFunctionByName(Entry.To);
			if (!NewFunction)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("'%s' is not a function of the new interface '%s'"), *Entry.To.ToString(), *OutPlan.NewInterface));
				return false;
			}
			FCortexGraphInterfaceFunctionPlan Function;
			Function.From = Entry.From.ToString();
			Function.To = Entry.To.ToString();
			Function.bToIsEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(NewFunction);
			Function.bToHasResult = !Function.bToIsEvent && FunctionHasResult(NewFunction);
			Function.Implementation = Function.bToIsEvent ? EventKind : FunctionGraphKind;
			Function.EntryIdentity = Guid(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("entry:") + Function.To));
			Function.ResultIdentity = Guid(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("result:") + Function.To));
			Function.SourceEntryGuid = Guid(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("replay_source:") + Function.To));
			Function.PinMap = Entry.PinMap;
			FGuid EntryGuid;
			FGuid::Parse(Function.EntryIdentity, EntryGuid);
			UEdGraphNode* const Replacement = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, EntryGuid);
			if (!Replacement || !Replacement->GetGraph())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the Blueprint implements the new interface but the deterministic replacement identity of '%s' does not exist; the old interface is not implemented, so this is not a replay of this request"), *Function.To));
				return false;
			}
			Function.SourceGraphGuid = Guid(Replacement->GetGraph()->GraphGuid);
			Function.SourceGraphName = Replacement->GetGraph()->GetName();
			OutPlan.Functions.Add(Function);

			TSharedPtr<FJsonObject> EntryMigration;
			TSharedPtr<FJsonObject> Selector;
			BuildEntryMigration(Function, OutPlan, EntryMigration, Selector);
			FCortexGraphMigrationIdentity Identity;
			FGuid::Parse(Function.EntryIdentity, Identity.EntryGuid);
			FGuid::Parse(Function.ResultIdentity, Identity.ResultGuid);
			FCortexGraphMigrationPlanOptions Options;
			Options.bSkipLoadedPackageInventory = true;
			Options.SourceDeclarationName = Entry.From;
			Options.SourceDeclarationOwner = OldInterface;
			FCortexGraphMigrationPlan EntryPlan;
			bool bEntryReused = false;
			if (!FCortexGraphMigrationOps::Plan(Blueprint, EntryMigration, Selector, Identity, EntryPlan, bEntryReused, OutError, &Options))
			{
				return false;
			}
			FString Failure;
			if (!bEntryReused || !FCortexGraphMigrationOps::VerifyReplacementAgainstNative(Blueprint, EntryPlan, false, false, Failure))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the existing deterministic replacement of '%s' does not match the planned replace_entry: %s"), *Function.To, *Failure));
				return false;
			}
			EntryPlans.Add(MoveTemp(EntryPlan));
		}
		FString Failure;
		if (!VerifyAgainstNative(Blueprint, OutPlan, OldInterface, NewInterface, EntryPlans, false, [](const TCHAR*) { return false; }, Failure))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the Blueprint implements the new interface but not as this request would leave it: %s"), *Failure));
			return false;
		}
		OutPlan.bReused = true;
		bOutReused = true;
		return true;
	}
	if (OldIndex == INDEX_NONE)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the Blueprint does not implement the old interface '%s'"), *OutPlan.OldInterface));
		return false;
	}
	if (NewIndex != INDEX_NONE)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the Blueprint already implements the new interface '%s'"), *OutPlan.NewInterface));
		return false;
	}
	FBPInterfaceDescription& OldDescription = Blueprint->ImplementedInterfaces[OldIndex];

	// What the old interface declares and what the Blueprint implements of it.
	TArray<FOldFunction> OldFunctions;
	for (UFunction* Function : InterfaceFunctions(OldInterface))
	{
		FOldFunction Old;
		Old.Function = Function;
		Old.bEventStyle = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function);
		if (!Old.bEventStyle)
		{
			for (UEdGraph* Graph : OldDescription.Graphs)
			{
				if (Graph && Graph->GetFName() == Function->GetFName())
				{
					Old.Graph = Graph;
					Old.bHasImplementation = true;
					Old.bImplemented = GraphHasBody(Graph);
					break;
				}
			}
		}
		else
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!IsEventNode(Node, Function->GetFName(), OldInterface))
					{
						continue;
					}
					if (Old.Event)
					{
						OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("the event '%s' of the old interface is implemented by more than one event node; remove the duplicate first"), *Function->GetName()));
						return false;
					}
					if (!Blueprint->UbergraphPages.Contains(Graph))
					{
						OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("the event '%s' of the old interface lives in the composite graph '%s'; replace_interface moves only top-level event graph events"), *Function->GetName(), *Graph->GetName()));
						return false;
					}
					Old.Event = Cast<UK2Node_Event>(Node);
					Old.bHasImplementation = true;
					Old.bImplemented = EventHasLinks(Old.Event);
				}
			}
		}
		OldFunctions.Add(Old);
	}
	for (UEdGraph* Graph : OldDescription.Graphs)
	{
		if (Graph && !OldFunctions.ContainsByPredicate([Graph](const FOldFunction& Old) { return Old.Function->GetFName() == Graph->GetFName(); }))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the graph '%s' of the old interface description names no function of the old interface; the interface state is inconsistent and cannot be migrated"), *Graph->GetName()));
			return false;
		}
	}

	// Mapping: every mapped function has an implementation, every implemented function is mapped.
	TArray<FString> Unmapped;
	for (const FOldFunction& Old : OldFunctions)
	{
		if (Old.bImplemented && !Froms.Contains(Old.Function->GetFName()))
		{
			Unmapped.Add(Old.Function->GetName());
		}
	}
	for (const FRequestEntry& Entry : Requested)
	{
		if (!OldFunctions.ContainsByPredicate([&Entry](const FOldFunction& Old) { return Old.Function->GetFName() == Entry.From; }))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("function_map names '%s', which is no function of the old interface '%s'"), *Entry.From.ToString(), *OutPlan.OldInterface));
			return false;
		}
	}
	if (Unmapped.Num() > 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the implemented function(s) %s of the old interface are missing from function_map; map every implemented function (an unimplemented stub may be left out and is dropped)"),
				*FString::Join(Unmapped, TEXT(", "))));
		return false;
	}

	TMap<UEdGraph*, TArray<FGuid>> ExcludedByGraph;
	for (const FRequestEntry& Entry : Requested)
	{
		const FOldFunction* const Old = OldFunctions.FindByPredicate([&Entry](const FOldFunction& Candidate) { return Candidate.Function->GetFName() == Entry.From; });
		if (!Old->bHasImplementation)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("function_map names '%s', but the Blueprint has no implementation of it (no graph or event) to migrate; remove it from function_map"), *Entry.From.ToString()));
			return false;
		}
		UFunction* const NewFunction = NewInterface->FindFunctionByName(Entry.To);
		const UClass* const NewOwner = NewFunction ? NewFunction->GetOwnerClass() : nullptr;
		if (!NewFunction || !NewOwner || NewOwner == UInterface::StaticClass() || NewOwner == UObject::StaticClass())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' is not a function of the new interface '%s'"), *Entry.To.ToString(), *OutPlan.NewInterface));
			return false;
		}
		const bool bToEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(NewFunction);
		if (bToEvent != Old->bEventStyle)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' is %s-style but '%s' is %s-style; an implementation moves only between functions of the same style"),
					*Entry.From.ToString(), Old->bEventStyle ? TEXT("event") : TEXT("function"), *Entry.To.ToString(), bToEvent ? TEXT("event") : TEXT("function")));
			return false;
		}
		if (!bToEvent && !IsFunctionStyle(NewFunction))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' cannot be overridden by a Blueprint graph"), *Entry.To.ToString()));
			return false;
		}
		if (NewFunction->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' is an animation Blueprint function, which replace_interface does not migrate"), *Entry.To.ToString()));
			return false;
		}

		UEdGraph* const SourceGraph = Old->bEventStyle ? Old->Event->GetGraph() : Old->Graph;
		UEdGraphNode* StaleEntry = Old->bEventStyle ? Cast<UEdGraphNode>(Old->Event) : nullptr;
		UEdGraphNode* StaleResult = nullptr;
		if (!Old->bEventStyle)
		{
			int32 ResultCount = 0;
			for (UEdGraphNode* Node : SourceGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* const FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					StaleEntry = FunctionEntry;
				}
				else if (Cast<UK2Node_FunctionResult>(Node))
				{
					StaleResult = Node;
					++ResultCount;
				}
			}
			if (!StaleEntry || ResultCount > 1)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the graph '%s' has %s; replace_interface owns exactly one entry and at most one result terminator"),
						*SourceGraph->GetName(), StaleEntry ? TEXT("several result terminators") : TEXT("no function entry")));
				return false;
			}
		}

		FCortexGraphInterfaceFunctionPlan Function;
		Function.From = Entry.From.ToString();
		Function.To = Entry.To.ToString();
		Function.Implementation = Old->bEventStyle ? EventKind : FunctionGraphKind;
		Function.bToIsEvent = bToEvent;
		Function.bToHasResult = !bToEvent && FunctionHasResult(NewFunction);
		Function.SourceGraphGuid = Guid(SourceGraph->GraphGuid);
		Function.SourceGraphName = SourceGraph->GetName();
		Function.SourceEntryGuid = Guid(StaleEntry->NodeGuid);
		Function.SourceResultGuid = StaleResult ? Guid(StaleResult->NodeGuid) : FString();
		Function.NodeCount = SourceGraph->Nodes.Num();
		Function.EntryIdentity = Guid(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("entry:") + Function.To));
		Function.ResultIdentity = Guid(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("result:") + Function.To));
		Function.PinMap = Entry.PinMap;

		TArray<FCortexGraphMigrationPinMapping> PinMap;
		FCortexCommandResult PinMapError;
		if (!FCortexGraphMigrationOps::ValidateReplacementPinMap(Blueprint, NewFunction, NewInterface, StaleEntry, StaleResult, Entry.PinMap, PinMap, PinMapError))
		{
			OutError = PinMapError;
			OutError.ErrorMessage = FString::Printf(TEXT("function_map '%s' -> '%s': %s"), *Function.From, *Function.To, *PinMapError.ErrorMessage);
			return false;
		}
		if (StaleResult)
		{
			// A mapped, unlinked result input keeps the typed default state its author set; the replacement is created with the
			// declaration's plain defaults, so the planned state is applied to it and read back.
			for (const FCortexGraphMigrationPinMapping& Mapping : PinMap)
			{
				const UEdGraphPin* const Stale = Mapping.TargetRole == TEXT("result") ? StaleResult->FindPin(FName(*Mapping.FromPin)) : nullptr;
				if (!Stale || Stale->Direction != EGPD_Input || Stale->LinkedTo.Num() > 0 || Stale->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				FCortexGraphInterfaceResultDefault Default;
				Default.ToPin = Mapping.ToPin;
				Default.Value = Stale->DefaultValue;
				Default.ObjectPath = Stale->DefaultObject ? Stale->DefaultObject->GetPathName() : FString();
				if (!Stale->DefaultTextValue.IsEmpty())
				{
					FTextStringHelper::WriteToBuffer(Default.Text, Stale->DefaultTextValue);
				}
				Function.ResultDefaults.Add(MoveTemp(Default));
			}
		}

		TArray<FGuid>& Excluded = ExcludedByGraph.FindOrAdd(SourceGraph);
		Excluded.Add(StaleEntry->NodeGuid);
		if (StaleResult) Excluded.Add(StaleResult->NodeGuid);
		Excluded.Add(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("entry:") + Function.To));
		Excluded.Add(FCortexGraphPatchOps::DeriveNodeGuid(PatchId, TEXT("result:") + Function.To));
		OutPlan.Functions.Add(MoveTemp(Function));
	}

	// Rename targets of adopted graphs: a taken name refuses, and so does a swap among the mapped graphs.
	for (const FCortexGraphInterfaceFunctionPlan& Function : OutPlan.Functions)
	{
		if (Function.Implementation != FunctionGraphKind || Function.From == Function.To)
		{
			continue;
		}
		const UObject* const Occupant = FindObjectNamed(Blueprint, FName(*Function.To));
		// A dropped stub graph is detached (and renamed away) before the rename, so its name is free.
		const bool bOccupiedByDroppedStub = Occupant != nullptr && OldFunctions.ContainsByPredicate([&](const FOldFunction& Old)
			{ return !Old.bEventStyle && Old.Graph == Occupant && !Froms.Contains(Old.Function->GetFName()) && Old.bHasImplementation; });
		if (Occupant && !bOccupiedByDroppedStub)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("cannot rename the graph '%s' to '%s': the name is already taken by '%s' in this Blueprint"),
					*Function.From, *Function.To, *Occupant->GetPathName()));
			return false;
		}
	}

	// Dropped stubs and the stubs of the new interface.
	TSet<FName> DroppedGraphNames;
	for (const FOldFunction& Old : OldFunctions)
	{
		if (Froms.Contains(Old.Function->GetFName()) || !Old.bHasImplementation)
		{
			continue;
		}
		FCortexGraphInterfaceDroppedStub Stub;
		Stub.Function = Old.Function->GetName();
		if (Old.bEventStyle)
		{
			Stub.Kind = EventKind;
			Stub.GraphGuid = Guid(Old.Event->GetGraph()->GraphGuid);
			Stub.GraphName = Old.Event->GetGraph()->GetName();
			Stub.NodeGuid = Guid(Old.Event->NodeGuid);
			ExcludedByGraph.FindOrAdd(Old.Event->GetGraph()).Add(Old.Event->NodeGuid);
		}
		else
		{
			Stub.Kind = FunctionGraphKind;
			Stub.GraphGuid = Guid(Old.Graph->GraphGuid);
			Stub.GraphName = Old.Graph->GetName();
			DroppedGraphNames.Add(Old.Graph->GetFName());
		}
		OutPlan.DroppedStubs.Add(MoveTemp(Stub));
	}
	for (TFieldIterator<UFunction> It(NewInterface, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* const Function = *It;
		if (!IsFunctionStyle(Function))
		{
			continue;
		}
		if (OutPlan.Functions.ContainsByPredicate([Function](const FCortexGraphInterfaceFunctionPlan& Mapped) { return Mapped.To == Function->GetName(); }))
		{
			continue;
		}
		if (Function->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the new interface declares the animation Blueprint function '%s', which replace_interface does not create"), *Function->GetName()));
			return false;
		}
		const UObject* const Occupant = FindObjectNamed(Blueprint, Function->GetFName());
		const bool bAllowed = Occupant == nullptr
			|| DroppedGraphNames.Contains(Function->GetFName())
			|| OutPlan.Functions.ContainsByPredicate([Function](const FCortexGraphInterfaceFunctionPlan& Mapped)
				{ return Mapped.Implementation == FunctionGraphKind && Mapped.From == Function->GetName() && Mapped.To != Mapped.From; });
		if (!bAllowed)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the new interface function '%s' conflicts with the existing object '%s' of this Blueprint; rename or remove it first"),
					*Function->GetName(), *Occupant->GetPathName()));
			return false;
		}
		OutPlan.NewStubFunctions.Add(Function->GetName());
	}
	OutPlan.NewStubFunctions.Sort();

	// In-asset references to the old interface would dangle once it is gone.
	TArray<FString> Sites;
	CollectOldInterfaceSites(Blueprint, OldInterface, Sites);
	if (Sites.Num() > 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%d in-asset reference(s) to the old interface would dangle: %s. Retarget them first (retarget_calls)."),
				Sites.Num(), *JoinBounded(Sites)));
		return false;
	}

	// Derived Blueprints whose override still describes the old signature.
	TSet<FName> MappedFunctions = Froms;
	TArray<FString> Overrides;
	if (!CollectDerivedOverrides(Blueprint, OldInterface, MappedFunctions, Overrides, OutError))
	{
		return false;
	}
	if (Overrides.Num() > 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%d derived Blueprint override(s) of a mapped function would keep describing the old signature: %s. Migrate the derived Blueprints in the same wave first."),
				Overrides.Num(), *JoinBounded(Overrides)));
		return false;
	}

	// Preservation contracts, one per touched graph, with every replaced and dropped node excluded.
	TArray<UEdGraph*> ContractGraphs;
	ExcludedByGraph.GenerateKeyArray(ContractGraphs);
	ContractGraphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GraphGuid.ToString() < B.GraphGuid.ToString(); });
	for (UEdGraph* const Graph : ContractGraphs)
	{
		TArray<FGuid> Excluded = ExcludedByGraph[Graph];
		FCortexGraphTransferPreservation Preservation;
		Preservation.Label = FString::Printf(TEXT("replace_interface:%s"), *Graph->GetName());
		Preservation.GraphGuid = Guid(Graph->GraphGuid);
		for (const FGuid& ExcludedGuid : Excluded)
		{
			Preservation.ExcludedGuids.Add(Guid(ExcludedGuid));
		}
		Preservation.ExcludedGuids.Sort();
		Preservation.Capture = FCortexGraphMigrationOps::CapturePreservation(Blueprint, Graph, Excluded);
		OutPlan.Preservations.Add(MoveTemp(Preservation));
	}
	TArray<UEdGraph*> Scanned;
	Blueprint->GetAllGraphs(Scanned);
	for (const UEdGraph* Graph : Scanned)
	{
		OutPlan.ScannedNodes += Graph ? Graph->Nodes.Num() : 0;
	}
	return true;
}

bool FCortexGraphInterfaceMigration::SwapInterfaces(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationInterfacePlan& Plan,
	UClass* OldInterface,
	UClass* NewInterface,
	FCortexGraphInterfaceStateCapture& Capture,
	FString& OutFailure)
{
	const int32 OldIndex = FindDescriptionIndex(Blueprint, OldInterface);
	if (OldIndex == INDEX_NONE || FindDescriptionIndex(Blueprint, NewInterface) != INDEX_NONE)
	{
		OutFailure = TEXT("the interface list no longer matches the reviewed plan");
		return false;
	}
	Blueprint->Modify();
	FBPInterfaceDescription& OldDescription = Blueprint->ImplementedInterfaces[OldIndex];

	// Dropped stub graphs leave the old description and the graph namespace before any stub of the new interface is created:
	// CreateNewGraph would otherwise silently rename a new stub next to a same-named graph that is still there.
	for (const FCortexGraphInterfaceDroppedStub& Stub : Plan.DroppedStubs)
	{
		if (Stub.Kind != FunctionGraphKind)
		{
			continue;
		}
		FGuid GraphGuid;
		FGuid::Parse(Stub.GraphGuid, GraphGuid);
		UEdGraph* const Graph = FindGraphInDescriptionByGuid(OldDescription, GraphGuid);
		if (!Graph)
		{
			OutFailure = FString::Printf(TEXT("the dropped stub graph '%s' no longer resolves"), *Stub.GraphName);
			return false;
		}
		OldDescription.Graphs.Remove(Graph);
		Graph->Modify();
		const FName Detached = MakeUniqueObjectName(Graph->GetOuter(), UEdGraph::StaticClass(), FName(*(Graph->GetName() + DetachedSuffix)));
		Graph->Rename(*Detached.ToString(), Graph->GetOuter(), REN_DontCreateRedirectors);
	}

	TMap<FName, UEdGraph*> Adopted;
	for (const FCortexGraphInterfaceFunctionPlan& Function : Plan.Functions)
	{
		if (Function.Implementation != FunctionGraphKind)
		{
			continue;
		}
		FGuid GraphGuid;
		FGuid::Parse(Function.SourceGraphGuid, GraphGuid);
		UEdGraph* const Graph = FindGraphInDescriptionByGuid(OldDescription, GraphGuid);
		if (!Graph)
		{
			OutFailure = FString::Printf(TEXT("the graph of '%s' no longer resolves in the old interface description"), *Function.From);
			return false;
		}
		OldDescription.Graphs.Remove(Graph);
		if (Function.From != Function.To)
		{
			// Graph->Rename, never FBlueprintEditorUtils::RenameGraph: that no-ops silently on a collision and edits other assets.
			if (FindObjectNamed(Graph->GetOuter(), FName(*Function.To)) != nullptr
				|| !Graph->Rename(*Function.To, Graph->GetOuter(), REN_Test))
			{
				OutFailure = FString::Printf(TEXT("the graph '%s' cannot be renamed to '%s': the name is taken"), *Function.From, *Function.To);
				return false;
			}
			Graph->Modify();
			Graph->Rename(*Function.To, Graph->GetOuter(), REN_DontCreateRedirectors);
			if (Graph->GetName() != Function.To)
			{
				OutFailure = FString::Printf(TEXT("the graph '%s' was not renamed to '%s'"), *Function.From, *Function.To);
				return false;
			}
		}
		Adopted.Add(FName(*Function.To), Graph);
	}

	FBPInterfaceDescription NewDescription;
	NewDescription.Interface = NewInterface;
	for (TFieldIterator<UFunction> It(NewInterface, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		UFunction* const Function = *It;
		if (!IsFunctionStyle(Function))
		{
			continue;
		}
		const FName FunctionName = Function->GetFName();
		UEdGraph* Graph = nullptr;
		bool bCreated = false;
		if (UEdGraph** const Found = Adopted.Find(FunctionName))
		{
			Graph = *Found;
			Adopted.Remove(FunctionName);
		}
		else
		{
			if (FindObjectNamed(Blueprint, FunctionName) != nullptr)
			{
				OutFailure = FString::Printf(TEXT("the new interface function '%s' conflicts with an existing object of this Blueprint"), *FunctionName.ToString());
				return false;
			}
			Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!Graph)
			{
				OutFailure = FString::Printf(TEXT("the stub graph of '%s' could not be created"), *FunctionName.ToString());
				return false;
			}
			Capture.AddedGraphs.Add(TStrongObjectPtr<UEdGraph>(Graph));
			if (Graph->GetFName() != FunctionName)
			{
				OutFailure = FString::Printf(TEXT("the stub graph of '%s' was created under the name '%s'"), *FunctionName.ToString(), *Graph->GetName());
				return false;
			}
			Graph->bAllowDeletion = false;
			bCreated = true;
		}
		Graph->Modify();
		Graph->InterfaceGuid = FBlueprintEditorUtils::FindInterfaceFunctionGuid(Function, NewInterface);
		NewDescription.Graphs.Add(Graph);
		if (bCreated)
		{
			FBlueprintEditorUtils::AddInterfaceGraph(Blueprint, Graph, NewInterface);
		}
	}
	if (Adopted.Num() > 0)
	{
		OutFailure = FString::Printf(TEXT("%d adopted graph(s) match no function-style function of the new interface"), Adopted.Num());
		return false;
	}
	Blueprint->ImplementedInterfaces.Add(MoveTemp(NewDescription));
	return true;
}

bool FCortexGraphInterfaceMigration::RemoveOldInterface(UBlueprint* Blueprint, UClass* OldInterface, FString& OutFailure)
{
	const int32 OldIndex = FindDescriptionIndex(Blueprint, OldInterface);
	if (OldIndex == INDEX_NONE)
	{
		OutFailure = TEXT("the old interface description is already gone");
		return false;
	}
	if (Blueprint->ImplementedInterfaces[OldIndex].Graphs.Num() != 0)
	{
		OutFailure = FString::Printf(TEXT("the old interface description still owns %d graph(s) that the plan neither adopted nor dropped"),
			Blueprint->ImplementedInterfaces[OldIndex].Graphs.Num());
		return false;
	}
	Blueprint->Modify();
	Blueprint->ImplementedInterfaces.RemoveAt(OldIndex);
	return true;
}

namespace
{
/** The planned result-literal state of one mapped function against its replacement result terminator. */
bool VerifyResultDefaults(UBlueprint* Blueprint, const FCortexGraphInterfaceFunctionPlan& Function, FString& OutFailure)
{
	if (Function.ResultDefaults.Num() == 0)
	{
		return true;
	}
	FGuid ResultGuid;
	FGuid::Parse(Function.ResultIdentity, ResultGuid);
	UEdGraphNode* const Result = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, ResultGuid);
	for (const FCortexGraphInterfaceResultDefault& Default : Function.ResultDefaults)
	{
		const UEdGraphPin* const Pin = Result ? Result->FindPin(FName(*Default.ToPin), EGPD_Input) : nullptr;
		FString LiveText;
		if (Pin && !Pin->DefaultTextValue.IsEmpty())
		{
			FTextStringHelper::WriteToBuffer(LiveText, Pin->DefaultTextValue);
		}
		if (!Pin || Pin->DefaultValue != Default.Value || LiveText != Default.Text
			|| (Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString()) != Default.ObjectPath)
		{
			OutFailure = FString::Printf(TEXT("the replacement result input '%s' of '%s' lost its authored default (expected '%s')"),
				*Default.ToPin, *Function.To, *Default.Value);
			return false;
		}
	}
	return true;
}

/**
 * The complete boundary of the replacement terminators: the live endpoint pairs of every visible replacement pin against the
 * planned per-function edge set. A missing, an extra and a non-reciprocal or dangling link are all refused.
 */
bool VerifyBoundaryEdges(UBlueprint* Blueprint, const TArray<FCortexGraphMigrationPlan>& EntryPlans, FString& OutFailure)
{
	for (const FCortexGraphMigrationPlan& Plan : EntryPlans)
	{
		FGuid GraphGuid;
		FGuid::Parse(Plan.GraphGuid, GraphGuid);
		UEdGraph* const Graph = FCortexGraphMigrationOps::FindGraphByGuid(Blueprint, GraphGuid);
		UEdGraphNode* const Entry = FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, Plan.Identity.EntryGuid);
		UEdGraphNode* const Result = Plan.HasResultTerminator() ? FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, Plan.Identity.ResultGuid) : nullptr;
		if (!Graph || !Entry)
		{
			OutFailure = FString::Printf(TEXT("'%s': the replacement terminators did not re-resolve for the boundary comparison"), *Plan.FunctionName);
			return false;
		}
		TMap<FString, TSet<FString>> Planned;
		for (const FCortexGraphMigrationEdge& Edge : Plan.Edges)
		{
			const FString Near = Edge.ReplacementRole + TEXT(".") + Edge.ReplacementPin;
			if (Edge.FarRole.IsEmpty())
			{
				Planned.FindOrAdd(Near).Add(FString::Printf(TEXT("node:%s.%s"), *Edge.FarGuid, *Edge.FarPin));
			}
			else
			{
				const FString Far = Edge.FarRole + TEXT(".") + Edge.FarPin;
				Planned.FindOrAdd(Near).Add(Far);
				Planned.FindOrAdd(Far).Add(Near);
			}
		}
		TMap<FString, TSet<FString>> Live;
		const TArray<TPair<UEdGraphNode*, const TCHAR*>> Terminators = { { Entry, TEXT("entry") }, { Result, TEXT("result") } };
		for (const TPair<UEdGraphNode*, const TCHAR*>& Terminator : Terminators)
		{
			if (!Terminator.Key) continue;
			for (const UEdGraphPin* Pin : Terminator.Key->Pins)
			{
				if (!Pin || Pin->bHidden || Pin->ParentPin != nullptr) continue;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* const Far = Linked ? Linked->GetOwningNode() : nullptr;
					if (!Far || Far->GetGraph() != Graph || !Linked->LinkedTo.Contains(Pin))
					{
						OutFailure = FString::Printf(TEXT("'%s': the %s pin '%s' carries a dangling or non-reciprocal link"), *Plan.FunctionName, Terminator.Value, *Pin->PinName.ToString());
						return false;
					}
					const FString FarKey = Far == Entry ? TEXT("entry.") + Linked->PinName.ToString()
						: (Far == Result ? TEXT("result.") + Linked->PinName.ToString()
							: FString::Printf(TEXT("node:%s.%s"), *Far->NodeGuid.ToString(), *Linked->PinName.ToString()));
					Live.FindOrAdd(FString(Terminator.Value) + TEXT(".") + Pin->PinName.ToString()).Add(FarKey);
				}
			}
		}
		TSet<FString> Keys;
		for (const TPair<FString, TSet<FString>>& Pair : Planned) { Keys.Add(Pair.Key); }
		for (const TPair<FString, TSet<FString>>& Pair : Live) { Keys.Add(Pair.Key); }
		for (const FString& Key : Keys)
		{
			const TSet<FString>* const P = Planned.Find(Key);
			const TSet<FString>* const L = Live.Find(Key);
			const bool bSame = (P ? P->Num() : 0) == (L ? L->Num() : 0) && (!P || (L && P->Difference(*L).Num() == 0));
			if (!bSame)
			{
				OutFailure = FString::Printf(TEXT("'%s': the links of the replacement pin '%s' are not exactly the planned boundary (planned %d, live %d)"),
					*Plan.FunctionName, *Key, P ? P->Num() : 0, L ? L->Num() : 0);
				return false;
			}
		}
	}
	return true;
}
}

bool FCortexGraphInterfaceMigration::ApplyResultDefaults(UBlueprint* Blueprint, const FCortexGraphInterfaceFunctionPlan& Function, FString& OutFailure)
{
	if (Function.ResultDefaults.Num() == 0)
	{
		return true;
	}
	FGuid ResultGuid;
	FGuid::Parse(Function.ResultIdentity, ResultGuid);
	UEdGraphNode* const Result = FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, ResultGuid);
	if (!Result)
	{
		OutFailure = FString::Printf(TEXT("the replacement result of '%s' did not re-resolve"), *Function.To);
		return false;
	}
	for (const FCortexGraphInterfaceResultDefault& Default : Function.ResultDefaults)
	{
		UEdGraphPin* const Pin = Result->FindPin(FName(*Default.ToPin), EGPD_Input);
		UObject* Object = nullptr;
		if (!Default.ObjectPath.IsEmpty())
		{
			Object = FindObject<UObject>(nullptr, *Default.ObjectPath);
		}
		if (!Pin || (!Default.ObjectPath.IsEmpty() && !Object))
		{
			OutFailure = FString::Printf(TEXT("the replacement result input '%s' of '%s' cannot take its authored default"), *Default.ToPin, *Function.To);
			return false;
		}
		Result->Modify();
		Pin->DefaultValue = Default.Value;
		Pin->DefaultObject = Object;
		if (!Default.Text.IsEmpty())
		{
			FText Text;
			FTextStringHelper::ReadFromBuffer(*Default.Text, Text);
			Pin->DefaultTextValue = Text;
		}
	}
	return true;
}

bool FCortexGraphInterfaceMigration::VerifyAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationInterfacePlan& Plan,
	UClass* OldInterface,
	UClass* NewInterface,
	const TArray<FCortexGraphMigrationPlan>& EntryPlans,
	const bool bCompiled,
	TFunctionRef<bool(const TCHAR*)> FaultProbe,
	FString& OutFailure)
{
	OutFailure.Reset();
	if (FindDescriptionIndex(Blueprint, OldInterface) != INDEX_NONE)
	{
		OutFailure = TEXT("the old interface is still implemented");
		return false;
	}
	int32 NewCount = 0;
	for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
	{
		NewCount += IsSameInterface(Description.Interface, NewInterface) ? 1 : 0;
	}
	if (NewCount != 1)
	{
		OutFailure = FString::Printf(TEXT("the new interface is implemented %d time(s), expected exactly once"), NewCount);
		return false;
	}
	const FBPInterfaceDescription& Description = Blueprint->ImplementedInterfaces[FindDescriptionIndex(Blueprint, NewInterface)];
	if (FaultProbe(TEXT("interface_after_swap")))
	{
		OutFailure = TEXT("replace_interface readback divergence injected after the interface swap");
		return false;
	}

	// The new description holds exactly the function-style graphs, each under its function name with the new InterfaceGuid.
	TSet<FName> Expected;
	for (TFieldIterator<UFunction> It(NewInterface, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (IsFunctionStyle(*It))
		{
			Expected.Add((*It)->GetFName());
		}
	}
	TSet<FName> Seen;
	for (const UEdGraph* Graph : Description.Graphs)
	{
		if (!Graph || !Expected.Contains(Graph->GetFName()) || Seen.Contains(Graph->GetFName()))
		{
			OutFailure = FString::Printf(TEXT("the new interface description holds the graph '%s', which is not a unique function-style function of the new interface"),
				Graph ? *Graph->GetName() : TEXT("<null>"));
			return false;
		}
		Seen.Add(Graph->GetFName());
		const UFunction* const Function = NewInterface->FindFunctionByName(Graph->GetFName());
		const FGuid ExpectedGuid = FBlueprintEditorUtils::FindInterfaceFunctionGuid(Function, NewInterface);
		if (Graph->InterfaceGuid != ExpectedGuid)
		{
			OutFailure = FString::Printf(TEXT("the graph '%s' carries the InterfaceGuid %s, expected %s"),
				*Graph->GetName(), *Graph->InterfaceGuid.ToString(), *ExpectedGuid.ToString());
			return false;
		}
	}
	if (Seen.Num() != Expected.Num())
	{
		OutFailure = FString::Printf(TEXT("the new interface description holds %d graph(s), expected %d"), Seen.Num(), Expected.Num());
		return false;
	}
	for (const FCortexGraphInterfaceFunctionPlan& Function : Plan.Functions)
	{
		if (Function.Implementation != FunctionGraphKind)
		{
			continue;
		}
		FGuid GraphGuid;
		FGuid::Parse(Function.SourceGraphGuid, GraphGuid);
		const bool bOwned = Description.Graphs.ContainsByPredicate([&](const UEdGraph* Graph)
			{ return Graph && Graph->GraphGuid == GraphGuid && Graph->GetName() == Function.To; });
		if (!bOwned)
		{
			OutFailure = FString::Printf(TEXT("the adopted graph of '%s' is not owned by the new interface description under the name '%s'"), *Function.From, *Function.To);
			return false;
		}
	}

	// Nothing may still reference the old interface, and no detached stub may remain reachable.
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		if (Graph->GetName().EndsWith(DetachedSuffix) || Graph->GetName().Contains(DetachedSuffix))
		{
			OutFailure = FString::Printf(TEXT("the detached stub graph '%s' is still reachable"), *Graph->GetName());
			return false;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node)) continue;
			const UK2Node_Event* const Event = Cast<UK2Node_Event>(Node);
			if (Event && !Event->IsA<UK2Node_CustomEvent>()
				&& IsSameInterface(Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode()), OldInterface))
			{
				OutFailure = FString::Printf(TEXT("the event node %s still belongs to the old interface"), *Node->NodeGuid.ToString());
				return false;
			}
			const UK2Node_FunctionEntry* const Entry = Cast<UK2Node_FunctionEntry>(Node);
			if (Entry && IsSameInterface(Entry->FunctionReference.GetMemberParentClass(), OldInterface))
			{
				OutFailure = FString::Printf(TEXT("the function entry %s still belongs to the old interface"), *Node->NodeGuid.ToString());
				return false;
			}
		}
	}

	for (const FCortexGraphMigrationPlan& EntryPlan : EntryPlans)
	{
		if (!FCortexGraphMigrationOps::VerifyReplacementAgainstNative(Blueprint, EntryPlan, bCompiled, false, OutFailure))
		{
			OutFailure = FString::Printf(TEXT("'%s': %s"), *EntryPlan.FunctionName, *OutFailure);
			return false;
		}
	}
	for (const FCortexGraphInterfaceFunctionPlan& Function : Plan.Functions)
	{
		if (!VerifyResultDefaults(Blueprint, Function, OutFailure))
		{
			return false;
		}
	}
	if (!VerifyBoundaryEdges(Blueprint, EntryPlans, OutFailure))
	{
		return false;
	}
	return FCortexGraphMigrationOps::VerifyPreservationContracts(Blueprint, Plan.Preservations, OutFailure);
}

TSharedPtr<FJsonObject> FCortexGraphInterfaceMigration::MakeInventory(const TSharedPtr<FJsonObject>& PlanJson)
{
	if (!PlanJson.IsValid()) return nullptr;
	FCortexGraphMigrationInterfacePlan Plan;
	FCortexCommandResult PlanError;
	if (!FCortexGraphMigrationInterfacePlan::FromJson(PlanJson, Plan, PlanError)) return nullptr;

	TSharedPtr<FJsonObject> Inventory = MakeShared<FJsonObject>();
	Inventory->SetStringField(TEXT("operation"), Plan.Op);
	Inventory->SetStringField(TEXT("old_interface"), Plan.OldInterface);
	Inventory->SetStringField(TEXT("new_interface"), Plan.NewInterface);
	Inventory->SetBoolField(TEXT("replay"), Plan.bReused);
	Inventory->SetNumberField(TEXT("scanned_nodes"), Plan.ScannedNodes);
	TArray<TSharedPtr<FJsonValue>> Functions;
	for (const FCortexGraphInterfaceFunctionPlan& Function : Plan.Functions)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("from"), Function.From);
		Json->SetStringField(TEXT("to"), Function.To);
		Json->SetStringField(TEXT("implementation"), Function.Implementation);
		Json->SetNumberField(TEXT("node_count"), Function.NodeCount);
		Json->SetStringField(TEXT("source_graph_guid"), Function.SourceGraphGuid);
		Json->SetStringField(TEXT("source_entry_guid"), Function.SourceEntryGuid);
		Json->SetStringField(TEXT("entry_identity"), Function.EntryIdentity);
		Json->SetNumberField(TEXT("pin_map_entries"), Function.PinMap.Num());
		Json->SetBoolField(TEXT("pin_map_complete"), true);
		Functions.Add(MakeShared<FJsonValueObject>(Json));
	}
	Inventory->SetArrayField(TEXT("functions"), Functions);
	TArray<TSharedPtr<FJsonValue>> Dropped;
	for (const FCortexGraphInterfaceDroppedStub& Stub : Plan.DroppedStubs)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("function"), Stub.Function);
		Json->SetStringField(TEXT("kind"), Stub.Kind);
		Dropped.Add(MakeShared<FJsonValueObject>(Json));
	}
	Inventory->SetArrayField(TEXT("dropped_stubs"), Dropped);
	Inventory->SetArrayField(TEXT("new_stubs"), StringValues(Plan.NewStubFunctions));
	// A refusing caller or derived override never reaches a plan, so an accepted plan has none of either.
	Inventory->SetArrayField(TEXT("in_asset_callers"), TArray<TSharedPtr<FJsonValue>>());
	Inventory->SetArrayField(TEXT("derived_overrides"), TArray<TSharedPtr<FJsonValue>>());
	Inventory->SetNumberField(TEXT("preserved_graphs"), Plan.Preservations.Num());
	TArray<TSharedPtr<FJsonValue>> Preservation;
	for (const FCortexGraphTransferPreservation& Contract : Plan.Preservations)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("label"), Contract.Label);
		Json->SetStringField(TEXT("graph_guid"), Contract.GraphGuid);
		Json->SetArrayField(TEXT("excluded_guids"), StringValues(Contract.ExcludedGuids));
		// The capture itself is a bulky canonical dump: the review surface is the surviving node count (every node of the graph
		// outside the replaced and dropped set, which must come back byte-identical) bound to its digest.
		int32 Surviving = 0;
		const int32 Header = Contract.Capture.Find(TEXT("Nodes: "));
		if (Header != INDEX_NONE)
		{
			Surviving = FCString::Atoi(*Contract.Capture.Mid(Header + 7, 12));
		}
		FTCHARToUTF8 Utf8(*Contract.Capture);
		Json->SetNumberField(TEXT("surviving_nodes"), Surviving);
		Json->SetNumberField(TEXT("capture_chars"), Contract.Capture.Len());
		Json->SetStringField(TEXT("capture_digest"), LexToString(FIoHash::HashBuffer(Utf8.Get(), Utf8.Length())));
		Preservation.Add(MakeShared<FJsonValueObject>(Json));
	}
	Inventory->SetArrayField(TEXT("preservation"), Preservation);
	return Inventory;
}
