#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphMigrationTestTypes.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * `migration.op="replace_entry"` coverage.
 *
 * Every case drives the real native entry points (`FCortexGraphPatchOps::Preflight` / `Execute`),
 * so the envelope, the phase shell, the journal and the native readback are exercised exactly as a
 * connected editor exercises them. The preservation oracle below is an independent test-side
 * capture of the downstream nodes the request never planned to touch.
 */
namespace CortexGraphMigrationReplaceTest
{
/** Canonical class path of the fixture declaration owner (UObject names drop the C++ prefix). */
FString FixtureActorClassPath()
{
	return ACortexGraphMigrationFixtureActor::StaticClass()->GetPathName();
}

const TCHAR* const ActorClassPath = TEXT("/Script/Engine.Actor");

/** Observations around real coordinator operations, installed per test. */
struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;

	void Begin()
	{
		*this = FOperations();
		Active = this;
		FCortexGraphPatchOps::SetOperationObserverForTesting(
			[](const FName Operation, UBlueprint* Observed)
			{
				(void)Observed;
				if (!Active) return;
				if (Operation == TEXT("target_compile")) ++Active->TargetCompiles;
				else if (Operation == TEXT("recovery_compile")) ++Active->RecoveryCompiles;
			});
		SaveHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString&, UPackage*, FObjectPostSaveContext)
			{
				if (Active) ++Active->Saves;
			});
	}

	void End()
	{
		UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

private:
	static FOperations* Active;
	FDelegateHandle SaveHandle;
};

FOperations* FOperations::Active = nullptr;

void ClearFaults()
{
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
}

void ResetTransaction()
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationReplaceTestCleanup")));
	}
}

int32 TransactionCount()
{
	return (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
}

/** Identity comparison that does not depend on the published GUID representation. */
bool SameGuid(const FString& Left, const FString& Right)
{
	FGuid LeftGuid;
	FGuid RightGuid;
	return FGuid::Parse(Left, LeftGuid) && FGuid::Parse(Right, RightGuid) && LeftGuid == RightGuid;
}

FString FingerprintHash(const TSharedPtr<FJsonObject>& Fingerprint)
{
	FString Hash;
	if (Fingerprint.IsValid()) Fingerprint->TryGetStringField(TEXT("graph_authoring_hash"), Hash);
	return Hash;
}

FString LiveGraphHash(UBlueprint* Blueprint)
{
	return FingerprintHash(FCortexGraphPatchState::ComputeFingerprint(Blueprint));
}

UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	return FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, NodeGuid);
}

int32 CountNativeNodes(UBlueprint* Blueprint)
{
	int32 Count = 0;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		Count += Graph ? Graph->Nodes.Num() : 0;
	}
	return Count;
}

/**
 * Independent preservation oracle: canonical native state of the selected downstream nodes plus the
 * links whose two ends are both inside that selection. The entry the request replaces and every
 * deliberately remapped boundary link stay outside it.
 */
FString CaptureSelectedNativeNodes(UBlueprint* Blueprint, const TArray<FGuid>& Guids)
{
	TSet<FGuid> InSet;
	for (const FGuid& Guid : Guids) InSet.Add(Guid);
	TArray<FString> Captures;
	for (const FGuid& Guid : Guids)
	{
		UEdGraphNode* Node = FindNodeByGuid(Blueprint, Guid);
		if (!Node)
		{
			Captures.Add(FString::Printf(TEXT("%s=<missing>"), *Guid.ToString()));
			continue;
		}
		FString Capture = FString::Printf(TEXT("guid=%s class=%s pos=(%d,%d) comment=\"%s\" bubble=%d/%d"),
			*Node->NodeGuid.ToString(), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY,
			*Node->NodeComment, Node->bCommentBubblePinned ? 1 : 0, Node->bCommentBubbleVisible ? 1 : 0);
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UClass* Owner = Call->FunctionReference.GetMemberParentClass();
			Capture += FString::Printf(TEXT(" symbol=%s@%s pure=%d"),
				*Call->FunctionReference.GetMemberName().ToString(),
				Owner ? *Owner->GetPathName() : TEXT("none"), Call->IsNodePure() ? 1 : 0);
		}
		else if (const UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
		{
			Capture += FString::Printf(TEXT(" var=%s self=%d"),
				*VarGet->VariableReference.GetMemberName().ToString(),
				VarGet->VariableReference.IsSelfContext() ? 1 : 0);
		}
		TArray<FString> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin != nullptr) continue;
			TArray<FString> Links;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!LinkedNode || !InSet.Contains(LinkedNode->NodeGuid)) continue;
				Links.Add(FString::Printf(TEXT("%s.%s"), *LinkedNode->NodeGuid.ToString(), *LinkedPin->PinName.ToString()));
			}
			Links.Sort();
			Pins.Add(FString::Printf(TEXT("%s|dir=%d|cat=%s|sub=%s|subobj=%s|container=%d|def=%s|linked=[%s]"),
				*Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction),
				*Pin->PinType.PinCategory.ToString(),
				*Pin->PinType.PinSubCategory.ToString(),
				Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("none"),
				static_cast<int32>(Pin->PinType.ContainerType),
				*Pin->DefaultValue,
				*FString::Join(Links, TEXT(","))));
		}
		Pins.Sort();
		Capture += FString::Printf(TEXT(" pins=[%s]"), *FString::Join(Pins, TEXT(";")));
		Captures.Add(MoveTemp(Capture));
	}
	Captures.Sort();
	return FString::Join(Captures, TEXT("\n"));
}

bool PreviewForApply(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexGraphPreparedPatch& OutPrepared,
	FCortexCommandResult& OutError)
{
	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
	{
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	OutPrepared = Preview;
	return true;
}

// ---------------------------------------------------------------------------
// Generic native fixtures
// ---------------------------------------------------------------------------

struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;

	bool Create(const TCHAR* Name)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ACortexGraphMigrationFixtureActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Blueprint != nullptr;
	}

	void Cleanup()
	{
		ResetTransaction();
		if (Blueprint)
		{
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Blueprint = nullptr;
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
			Package = nullptr;
		}
	}
};

/** A third-party asset that keeps referencing a member of the migrated asset. */
struct FExternalReferenceFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;

	bool Create(UBlueprint* Parent, const TCHAR* Name, const TCHAR* MemberName)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		UClass* const ParentClass = Parent && Parent->GeneratedClass
			? Parent->GeneratedClass.Get()
			: ACortexGraphMigrationFixtureActor::StaticClass();
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint) return false;
		UEdGraph* Graph = EnsureEventGraph(Blueprint);
		UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
		Get->VariableReference.SetSelfMember(FName(MemberName));
		Get->CreateNewGuid();
		Get->NodePosX = 0;
		Get->NodePosY = 0;
		Graph->AddNode(Get, true, false);
		return true;
	}

	void Cleanup()
	{
		if (Blueprint)
		{
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Blueprint = nullptr;
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
			Package = nullptr;
		}
	}

	static UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
	{
		if (Blueprint->UbergraphPages.Num() > 0) return Blueprint->UbergraphPages[0];
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		return Graph;
	}
};

UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
{
	if (Blueprint->UbergraphPages.Num() > 0) return Blueprint->UbergraphPages[0];
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	return Graph;
}

UK2Node_Event* AddEventNode(
	UEdGraph* Graph,
	const TCHAR* FunctionName,
	const TCHAR* OwnerClassPath,
	const int32 X,
	const int32 Y)
{
	UClass* OwnerClass = FindObject<UClass>(nullptr, OwnerClassPath);
	UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
	Event->EventReference.SetExternalMember(FName(FunctionName), OwnerClass);
	Event->bOverrideFunction = true;
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();
	Event->NodePosX = X;
	Event->NodePosY = Y;
	Event->NodeComment = FString::Printf(TEXT("stale %s entry"), FunctionName);
	Event->bCommentBubblePinned = true;
	Event->bCommentBubbleVisible = true;
	Graph->AddNode(Event, true, false);
	return Event;
}

UK2Node_CallFunction* AddPrintNode(UEdGraph* Graph, const TCHAR* Text, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	if (UEdGraphPin* InString = Call->FindPin(TEXT("InString")))
	{
		InString->DefaultValue = Text;
	}
	Graph->AddNode(Call, true, false);
	return Call;
}

UEdGraphNode_Comment* AddCommentNode(UEdGraph* Graph, const TCHAR* Text, const int32 X, const int32 Y)
{
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
	Comment->CreateNewGuid();
	Comment->NodeComment = Text;
	Comment->NodePosX = X;
	Comment->NodePosY = Y;
	Comment->NodeWidth = 400;
	Comment->NodeHeight = 200;
	Graph->AddNode(Comment, true, false);
	return Comment;
}

bool LinkPins(UEdGraph* Graph, UEdGraphPin* From, UEdGraphPin* To)
{
	const UEdGraphSchema* Schema = Graph->GetSchema();
	return From && To && Schema && Schema->TryCreateConnection(From, To);
}

bool LinkNodes(UEdGraph* Graph, UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
{
	UEdGraphPin* Source = From ? From->FindPin(FName(FromPin)) : nullptr;
	UEdGraphPin* Target = To ? To->FindPin(FName(ToPin)) : nullptr;
	return LinkPins(Graph, Source, Target);
}

/**
 * Removes every node the engine pre-created in a fresh ubergraph, so the fixture graph contains
 * exactly the stale entry and the downstream body the case authors (a new EventGraph otherwise
 * already implements BeginPlay, which is itself one of the migration targets).
 */
int32 ClearGraphNodes(UEdGraph* Graph)
{
	int32 Removed = 0;
	const TArray<UEdGraphNode*> Nodes = Graph->Nodes;
	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node) continue;
		Node->DestroyNode();
		++Removed;
	}
	return Removed;
}

/**
 * Sorted far endpoints of one pin that live outside the replaced set: the boundary links the
 * replacement must realize, expressed independently of the fixture pointers.
 */
TArray<FString> BoundaryFarEndpoints(UEdGraphNode* Node, const TCHAR* PinName, const TArray<FGuid>& ReplacedGuids)
{
	TArray<FString> Endpoints;
	UEdGraphNode* Owning = Node;
	UEdGraphPin* Pin = Owning ? Owning->FindPin(FName(PinName)) : nullptr;
	if (!Pin) return Endpoints;
	for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		const UEdGraphNode* FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
		if (!FarNode || ReplacedGuids.Contains(FarNode->NodeGuid)) continue;
		Endpoints.Add(FString::Printf(TEXT("%s.%s"), *FarNode->NodeGuid.ToString(), *LinkedPin->PinName.ToString()));
	}
	Endpoints.Sort();
	return Endpoints;
}

/** A stale function terminator pair inside a function graph named after the declaration. */
struct FStaleFunctionGraph
{
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
};

FStaleFunctionGraph AddStaleFunctionGraph(UBlueprint* Blueprint, const TCHAR* FunctionName, const TCHAR* OwnerClassPath)
{
	FStaleFunctionGraph Stale;
	UClass* OwnerClass = FindObject<UClass>(nullptr, OwnerClassPath);
	Stale.Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(FunctionName),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	Blueprint->FunctionGraphs.Add(Stale.Graph);

	Stale.Entry = NewObject<UK2Node_FunctionEntry>(Stale.Graph);
	Stale.Entry->FunctionReference.SetExternalMember(FName(FunctionName), OwnerClass);
	Stale.Entry->CreateNewGuid();
	Stale.Entry->AllocateDefaultPins();
	Stale.Entry->NodePosX = 0;
	Stale.Entry->NodePosY = 0;
	Stale.Entry->NodeComment = FString::Printf(TEXT("stale %s entry"), FunctionName);
	Stale.Graph->AddNode(Stale.Entry, true, false);

	Stale.Result = NewObject<UK2Node_FunctionResult>(Stale.Graph);
	Stale.Result->FunctionReference = Stale.Entry->FunctionReference;
	Stale.Result->CreateNewGuid();
	Stale.Result->AllocateDefaultPins();
	Stale.Result->NodePosX = 700;
	Stale.Result->NodePosY = 0;
	Stale.Graph->AddNode(Stale.Result, true, false);
	return Stale;
}

UK2Node_VariableGet* AddVariableGetNode(UEdGraph* Graph, const TCHAR* MemberName, const int32 X, const int32 Y)
{
	UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
	Get->VariableReference.SetSelfMember(FName(MemberName));
	Get->CreateNewGuid();
	Get->AllocateDefaultPins();
	Get->NodePosX = X;
	Get->NodePosY = Y;
	Graph->AddNode(Get, true, false);
	return Get;
}

/**
 * Adds an app-declared Blueprint variable with the given name.
 *
 * The authored description is inserted directly: a name that collides with an inherited declaration
 * cannot be created through the engine variable API without an internal class-generation error, and
 * the migration only needs the asset's authored member state to be adversarial.
 */
bool AddShadowingVariable(UBlueprint* Blueprint, const TCHAR* MemberName)
{
	FBPVariableDescription Description;
	Description.VarName = FName(MemberName);
	Description.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Description.DefaultValue = TEXT("7");
	Description.VarGuid = FGuid::NewGuid();
	Blueprint->Modify();
	Blueprint->NewVariables.Add(Description);
	return FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(MemberName)) != INDEX_NONE;
}

UK2Node_CustomEvent* AddCustomEventNode(UEdGraph* Graph, const TCHAR* MemberName)
{
	UK2Node_CustomEvent* Custom = NewObject<UK2Node_CustomEvent>(Graph);
	Custom->CustomFunctionName = FName(MemberName);
	Custom->CreateNewGuid();
	Custom->AllocateDefaultPins();
	Custom->NodePosX = 0;
	Custom->NodePosY = 600;
	Graph->AddNode(Custom, true, false);
	return Custom;
}

// ---------------------------------------------------------------------------
// Request builders
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> PinMapEntry(const TCHAR* Entry, const TCHAR* FromPin, const TCHAR* ToPin)
{
	TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
	EntryJson->SetStringField(TEXT("entry"), Entry);
	EntryJson->SetStringField(TEXT("from_pin"), FromPin);
	EntryJson->SetStringField(TEXT("to_pin"), ToPin);
	return EntryJson;
}

TSharedPtr<FJsonObject> MakeMigration(
	UEdGraph* SourceGraph,
	UEdGraphNode* SourceEntry,
	const TArray<TSharedPtr<FJsonValue>>& PinMap,
	const bool bHasRemoveShadowingMember = false,
	const bool bRemoveShadowingMember = false)
{
	TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
	Migration->SetStringField(TEXT("op"), TEXT("replace_entry"));
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), SourceGraph->GraphGuid.ToString());
	Source->SetObjectField(TEXT("graph_ref"), GraphRef);
	Source->SetStringField(TEXT("entry_node_guid"), SourceEntry->NodeGuid.ToString());
	Migration->SetObjectField(TEXT("source"), Source);
	Migration->SetArrayField(TEXT("pin_map"), PinMap);
	if (bHasRemoveShadowingMember)
	{
		Migration->SetBoolField(TEXT("remove_shadowing_member"), bRemoveShadowingMember);
	}
	return Migration;
}

TSharedPtr<FJsonObject> ReplacementRequest(
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	const TSharedPtr<FJsonObject>& Migration,
	const TCHAR* FunctionName,
	const TCHAR* OwnerClassPath = nullptr)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"),
		OwnerClassPath ? FString(OwnerClassPath) : FixtureActorClassPath());
	Implementation->SetStringField(TEXT("function_name"), FunctionName);
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	Request->SetObjectField(TEXT("migration"), Migration);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

/** Identity map over the fixture declaration's event pins. */
TArray<TSharedPtr<FJsonValue>> OnPayloadPinMap()
{
	TArray<TSharedPtr<FJsonValue>> Map;
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("OutputDelegate"), TEXT("OutputDelegate"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Payload"), TEXT("Payload"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Ids"), TEXT("Ids"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Tag"), TEXT("Tag"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Source"), TEXT("Source"))));
	return Map;
}

TArray<TSharedPtr<FJsonValue>> ReceiveBeginPlayPinMap()
{
	TArray<TSharedPtr<FJsonValue>> Map;
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("OutputDelegate"), TEXT("OutputDelegate"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then"))));
	return Map;
}

TArray<TSharedPtr<FJsonValue>> ComputeScorePinMap()
{
	TArray<TSharedPtr<FJsonValue>> Map;
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Tag"), TEXT("Tag"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("input"), TEXT("execute"), TEXT("execute"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("input"), TEXT("OutIds"), TEXT("OutIds"))));
	Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("input"), TEXT("ReturnValue"), TEXT("ReturnValue"))));
	return Map;
}

/** Replaces one mapped entry of a pin map, keeping coverage complete. */
TArray<TSharedPtr<FJsonValue>> WithEntry(TArray<TSharedPtr<FJsonValue>> Map, const TCHAR* ToPin, const TCHAR* Entry, const TCHAR* FromPin)
{
	for (TSharedPtr<FJsonValue>& Value : Map)
	{
		TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid() && Object->GetStringField(TEXT("to_pin")) == ToPin)
		{
			Object->SetStringField(TEXT("entry"), Entry);
			Object->SetStringField(TEXT("from_pin"), FromPin);
			break;
		}
	}
	return Map;
}

UEdGraphPin* FindMappablePin(UEdGraphNode* Node, const TCHAR* PinName)
{
	if (!Node) return nullptr;
	UEdGraphPin* Pin = Node->FindPin(FName(PinName));
	return Pin && Pin->ParentPin == nullptr && !Pin->bHidden ? Pin : nullptr;
}
}

// ---------------------------------------------------------------------------
// 1. CompatibleReplacement
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationCompatibleReplacementTest,
	"Cortex.Graph.Authoring.Migration.Replace.CompatibleReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationCompatibleReplacementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationCompatible_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("cortex migration"), 400, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Graph, TEXT("tail"), 800, 0);
	UEdGraphNode_Comment* Comment = AddCommentNode(Graph, TEXT("body comment"), -100, -200);
	TestTrue(TEXT("stale then link wired"), LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute")));
	TestTrue(TEXT("downstream internal link wired"), LinkNodes(Graph, Print, TEXT("then"), Tail, TEXT("execute")));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Tail->NodeGuid);
	DownstreamGuids.Add(Comment->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110001"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("compatible replacement previews: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	TestTrue(TEXT("preview reports a prospective change"), Prepared.bChanged);
	const FGuid* ReplacementGuid = Prepared.NodeGuidByClientId.Find(TEXT("entry"));
	TestTrue(TEXT("preview publishes the replacement identity"), ReplacementGuid != nullptr && ReplacementGuid->IsValid());

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("compatible replacement succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("compatible replacement reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("compatible replacement compiles once"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("compatible replacement target compile count"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("compatible replacement recovery compile count"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("compatible replacement readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("compatible replacement does not block"), Outcome.bBlocked, false);
	TestEqual(TEXT("compatible replacement saves nothing"), Operations.Saves, 0);
	TestNotEqual(TEXT("compatible replacement changed the fingerprint"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("downstream presentation preserved"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);

	UEdGraphNode* Replacement = ReplacementGuid ? FindNodeByGuid(Fixture.Blueprint, *ReplacementGuid) : nullptr;
	TestNotNull(TEXT("replacement entry resolves at its deterministic identity"), Replacement);
	TestNull(TEXT("stale entry was replaced"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	if (Replacement)
	{
		TestEqual(TEXT("replacement is the target declaration entry"),
			Replacement->GetClass()->GetPathName(), FString(UK2Node_Event::StaticClass()->GetPathName()));
		TestEqual(TEXT("replacement inherits the stale layout"), Replacement->NodePosX, StaleEntry->NodePosX);
		TestEqual(TEXT("replacement inherits the stale comment"), Replacement->NodeComment, StaleEntry->NodeComment);
		UEdGraphPin* Then = Replacement->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* PrintExec = Print->FindPin(TEXT("execute"));
		TestTrue(TEXT("mapped boundary link is realized on the replacement"),
			Then && PrintExec && Then->LinkedTo.Contains(PrintExec));
		TestEqual(TEXT("realized boundary link is single-linked"), PrintExec ? PrintExec->LinkedTo.Num() : 0, 1);
	}

	// The published command path reports the replacement mapping for the same request shape.
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
	TSharedPtr<FJsonObject> CommandRequest = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110001"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));
	const FCortexCommandResult CommandResult = Router.Execute(TEXT("graph.apply_patch"), CommandRequest);
	TestTrue(FString::Printf(TEXT("replay preview through the command path succeeds: %s"), *CommandResult.ErrorMessage),
		CommandResult.bSuccess);
	if (CommandResult.bSuccess && CommandResult.Data.IsValid())
	{
		TestFalse(TEXT("replay preview reports no prospective change"), CommandResult.Data->GetBoolField(TEXT("changed")));
		TestTrue(TEXT("replay preview publishes the validation token"), CommandResult.Data->HasField(TEXT("validation_hash")));
		const TSharedPtr<FJsonObject>* Mappings = nullptr;
		TestTrue(TEXT("replay preview publishes the replacement mapping"),
			CommandResult.Data->TryGetObjectField(TEXT("node_mappings"), Mappings) && Mappings && (*Mappings)->HasField(TEXT("entry")));
		if (Mappings && ReplacementGuid)
		{
			TestTrue(TEXT("the published mapping is the deterministic replacement identity"),
				SameGuid((*Mappings)->GetStringField(TEXT("entry")), ReplacementGuid->ToString()));
		}
	}

	// F1: a user custom event derives from the event node class, so it must never be treated as the
	// entry of the replaced declaration, and it must never be detached or deleted by the apply.
	{
		UK2Node_CustomEvent* StrayEvent = AddCustomEventNode(Graph, TEXT("CortexStrayEvent"));
		TestNotNull(TEXT("stray custom event created"), StrayEvent);
		const FGuid StrayGuid = StrayEvent->NodeGuid;
		TSharedPtr<FJsonObject> StrayRequest = ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000110001"),
			MakeMigration(Graph, StrayEvent, OnPayloadPinMap()), TEXT("OnPayload"));
		const FString HashBeforeStray = LiveGraphHash(Fixture.Blueprint);
		FCortexGraphPatchOutcome StrayOutcome;
		FCortexCommandResult StrayError;
		TestFalse(TEXT("an unrelated user custom event is not accepted as the source entry"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, StrayRequest, StrayOutcome, StrayError));
		TestEqual(TEXT("unrelated custom event refusal is INVALID_OPERATION"),
			StrayError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("refusal names the custom event [%s]"), *StrayError.ErrorMessage),
			StrayError.ErrorMessage.Contains(TEXT("custom event")));
		TestEqual(TEXT("unrelated custom event refusal mutates nothing"),
			LiveGraphHash(Fixture.Blueprint), HashBeforeStray);
		TestNotNull(TEXT("the user custom event still exists"), FindNodeByGuid(Fixture.Blueprint, StrayGuid));
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 2. IncompatibleSignatureRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationIncompatibleSignatureTest,
	"Cortex.Graph.Authoring.Migration.Replace.IncompatibleSignatureRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace CortexGraphMigrationReplaceTest
{
/** True when a TYPE_MISMATCH refusal names the given compatibility dimension. */
bool NamesDimension(const FString& Message, const TCHAR* Dimension)
{
	return Message.Contains(Dimension);
}

/**
 * Mutates one stale pin of the fixture and proves the request is refused with TYPE_MISMATCH naming
 * the pin and dimension, with zero mutation and an unchanged fingerprint.
 */
void CheckIncompatibleMapping(
	FAutomationTestBase& Test,
	const TCHAR* FixtureName,
	const TCHAR* Context,
	const TCHAR* Dimension,
	TFunctionRef<void(UK2Node_Event*)> MutateStale,
	TFunctionRef<TArray<TSharedPtr<FJsonValue>>()> BuildPinMap)
{
	FFixture Fixture;
	Test.TestTrue(FString::Printf(TEXT("%s: fixture created"), Context),
		Fixture.Create(FixtureName));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("mismatch body"), 400, 0);
	Test.TestTrue(FString::Printf(TEXT("%s: body wired"), Context),
		LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute")));
	// The asset is compiled while it is still consistent, then the stale pin is mutated, so the
	// refusal can only come from the compatibility diff and never from a pre-existing error state.
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FGuid StaleGuid = StaleEntry->NodeGuid;
	MutateStale(StaleEntry);

	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110002"),
		MakeMigration(Graph, StaleEntry, BuildPinMap()), TEXT("OnPayload"));

	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	Test.TestFalse(FString::Printf(TEXT("%s: incompatible mapping is refused"), Context),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Test.TestEqual(FString::Printf(TEXT("%s: refusal is TYPE_MISMATCH [%s]"), Context, *Error.ErrorMessage),
		Error.ErrorCode, FString(CortexErrorCodes::TypeMismatch));
	Test.TestTrue(FString::Printf(TEXT("%s: refusal names the %s dimension [%s]"), Context, Dimension, *Error.ErrorMessage),
		NamesDimension(Error.ErrorMessage, Dimension));
	Test.TestEqual(FString::Printf(TEXT("%s: refusal mutates nothing"), Context),
		LiveGraphHash(Fixture.Blueprint), HashBefore);
	Test.TestEqual(FString::Printf(TEXT("%s: refusal leaves the node count"), Context),
		CountNativeNodes(Fixture.Blueprint), NodesBefore);
	Test.TestNotNull(FString::Printf(TEXT("%s: stale entry is untouched"), Context),
		FindNodeByGuid(Fixture.Blueprint, StaleGuid));
	Fixture.Cleanup();
}
}

bool FCortexGraphMigrationIncompatibleSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	// Wrong direction: the request declares an output pin as an input.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationDirection_T11"), TEXT("direction"), TEXT("direction"),
		[](UK2Node_Event*) {},
		[]()
		{
			TArray<TSharedPtr<FJsonValue>> Map = OnPayloadPinMap();
			return WithEntry(Map, TEXT("then"), TEXT("input"), TEXT("then"));
		});

	// Reference mismatch: the stale parameter is by reference while the declaration is a const ref.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationReference_T11"), TEXT("reference"), TEXT("reference"),
		[](UK2Node_Event* Stale) { FindMappablePin(Stale, TEXT("Tag"))->PinType.bIsReference = true; },
		[]() { return OnPayloadPinMap(); });

	// Const mismatch: the declaration parameter is mutable while the stale pin claims const.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationConst_T11"), TEXT("const"), TEXT("const"),
		[](UK2Node_Event* Stale) { FindMappablePin(Stale, TEXT("Source"))->PinType.bIsConst = true; },
		[]() { return OnPayloadPinMap(); });

	// Container-kind mismatch: the stale array parameter is a set.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationContainer_T11"), TEXT("container"), TEXT("container"),
		[](UK2Node_Event* Stale) { FindMappablePin(Stale, TEXT("Ids"))->PinType.ContainerType = EPinContainerType::Set; },
		[]() { return OnPayloadPinMap(); });

	// Map-terminal mismatch: both sides are maps, the stale value type differs.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationMapTerminal_T11"), TEXT("map terminal"), TEXT("map_terminal"),
		[](UK2Node_Event* Stale)
		{
			UEdGraphPin* Payload = FindMappablePin(Stale, TEXT("Payload"));
			Payload->PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
			Payload->PinType.PinValueType.TerminalSubCategory = NAME_None;
			Payload->PinType.PinValueType.TerminalSubCategoryObject = nullptr;
		},
		[]() { return OnPayloadPinMap(); });

	// Object-class mismatch: the stale object pin points at an unrelated class.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationObjectClass_T11"), TEXT("object class"), TEXT("object_class"),
		[](UK2Node_Event* Stale)
		{
			FindMappablePin(Stale, TEXT("Source"))->PinType.PinSubCategoryObject = UActorComponent::StaticClass();
		},
		[]() { return OnPayloadPinMap(); });

	// Category mismatch: the stale exec pin is mapped onto a data pin of a different category.
	CheckIncompatibleMapping(*this, TEXT("BP_MigrationCategory_T11"), TEXT("category"), TEXT("category"),
		[](UK2Node_Event* Stale)
		{
			UEdGraphPin* Payload = FindMappablePin(Stale, TEXT("Payload"));
			Payload->PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			Payload->PinType.ContainerType = EPinContainerType::None;
		},
		[]() { return OnPayloadPinMap(); });

	return true;
}

// ---------------------------------------------------------------------------
// 3. SameNameConflictRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationSameNameConflictTest,
	"Cortex.Graph.Authoring.Migration.Replace.SameNameConflictRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationSameNameConflictTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationSameName_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestNotNull(TEXT("same-named custom event created"), AddCustomEventNode(Graph, TEXT("OnPayload")));

	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110003"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));

	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("same-name conflict blocks the replacement"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("same-name conflict is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("same-name conflict names the member [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("OnPayload")));
	TestEqual(TEXT("same-name conflict mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("same-name conflict adds no node"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestNotNull(TEXT("stale entry is untouched"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4. ExternalReferenceBlocks
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationExternalReferenceTest,
	"Cortex.Graph.Authoring.Migration.Replace.ExternalReferenceBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationExternalReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationExternal_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("external body"), 400, 0);
	TestTrue(TEXT("body wired"), LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute")));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("shadowing member added"), AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));

	FExternalReferenceFixture External;
	TestTrue(TEXT("external asset created"), External.Create(Fixture.Blueprint, TEXT("BP_MigrationExternalChild_T11"), TEXT("OnPayload")));
	const int32 ExternalNodes = CountNativeNodes(External.Blueprint);

	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110004"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));

	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("external reference blocks the replacement"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("external reference is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("external reference is named [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("outside this asset")));
	TestTrue(TEXT("external reference names the package"), Error.ErrorMessage.Contains(TEXT("BP_MigrationExternalChild_T11")));
	TestEqual(TEXT("external reference mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("external reference triggers no repair in this asset"),
		CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestEqual(TEXT("external reference leaves the other asset untouched"),
		CountNativeNodes(External.Blueprint), ExternalNodes);
	TestNotNull(TEXT("stale entry is untouched"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	TestTrue(TEXT("shadowing member survives"),
		FBlueprintEditorUtils::FindNewVariableIndex(Fixture.Blueprint, FName(TEXT("OnPayload"))) != INDEX_NONE);

	External.Cleanup();
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 5. ShadowingMemberRefusalByName
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationShadowingRefusalTest,
	"Cortex.Graph.Authoring.Migration.Replace.ShadowingMemberRefusalByName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationShadowingRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	// (a) The default refuses and names the parameter that would authorize the removal.
	{
		FFixture Fixture;
		TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationShadowing_T11")));
		if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
		UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
		ClearGraphNodes(Graph);
	ClearGraphNodes(Graph);
		UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
		UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("shadow body"), 400, 0);
		LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		TestTrue(TEXT("shadowing member added"), AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));
		UK2Node_VariableGet* Reference = AddVariableGetNode(Graph, TEXT("OnPayload"), 0, 400);

		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000110005"),
			MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));

		FCortexCommandResult Error;
		FCortexGraphPatchOutcome Outcome;
		TestFalse(TEXT("shadowing member refuses by default"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(TEXT("shadowing refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("shadowing refusal names the parameter [%s]"), *Error.ErrorMessage),
			Error.ErrorMessage.Contains(TEXT("remove_shadowing_member")));
		TestTrue(TEXT("shadowing refusal says removal was not attempted"),
			Error.ErrorMessage.Contains(TEXT("was not removed")));
		TestEqual(TEXT("shadowing refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestTrue(TEXT("user member is untouched"),
			FBlueprintEditorUtils::FindNewVariableIndex(Fixture.Blueprint, FName(TEXT("OnPayload"))) != INDEX_NONE);
		TestNotNull(TEXT("user member reference node is untouched"), FindNodeByGuid(Fixture.Blueprint, Reference->NodeGuid));
		Fixture.Cleanup();
	}

	// (b) Even with the flag, an unsafe (external) reference set still blocks.
	{
		FFixture Fixture;
		TestTrue(TEXT("second fixture created"), Fixture.Create(TEXT("BP_MigrationShadowingUnsafe_T11")));
		if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
		UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
		ClearGraphNodes(Graph);
	ClearGraphNodes(Graph);
		UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		TestTrue(TEXT("second shadowing member added"), AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));

		FExternalReferenceFixture External;
		TestTrue(TEXT("unsafe external asset created"),
			External.Create(Fixture.Blueprint, TEXT("BP_MigrationShadowingChild_T11"), TEXT("OnPayload")));

		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000110006"),
			MakeMigration(Graph, StaleEntry, OnPayloadPinMap(), true, true), TEXT("OnPayload"));

		FCortexCommandResult Error;
		FCortexGraphPatchOutcome Outcome;
		TestFalse(TEXT("unsafe reference set still blocks with the flag"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(TEXT("unsafe refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("unsafe refusal names the reference [%s]"), *Error.ErrorMessage),
			Error.ErrorMessage.Contains(TEXT("BP_MigrationShadowingChild_T11")));
		TestEqual(TEXT("unsafe refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestTrue(TEXT("user member survives the unsafe refusal"),
			FBlueprintEditorUtils::FindNewVariableIndex(Fixture.Blueprint, FName(TEXT("OnPayload"))) != INDEX_NONE);

		External.Cleanup();
		Fixture.Cleanup();
	}
	return true;
}

// ---------------------------------------------------------------------------
// 6. ShadowingMemberRemovalProvenSafe
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationShadowingRemovalTest,
	"Cortex.Graph.Authoring.Migration.Replace.ShadowingMemberRemovalProvenSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationShadowingRemovalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationShadowingSafe_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("safe removal body"), 400, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Graph, TEXT("safe removal tail"), 800, 0);
	LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
	LinkNodes(Graph, Print, TEXT("then"), Tail, TEXT("execute"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("shadowing member added"), AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));
	UK2Node_VariableGet* Reference = AddVariableGetNode(Graph, TEXT("OnPayload"), 0, 400);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Tail->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110007"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap(), true, true), TEXT("OnPayload"));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("proven-safe removal previews: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("proven-safe removal succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("proven-safe removal applies"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("proven-safe removal compiles"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("proven-safe removal target compile count"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("proven-safe removal readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("proven-safe removal does not block"), Outcome.bBlocked, false);
	TestTrue(TEXT("shadowing member was removed"),
		FBlueprintEditorUtils::FindNewVariableIndex(Fixture.Blueprint, FName(TEXT("OnPayload"))) == INDEX_NONE);
	TestNull(TEXT("the resolved in-asset reference node was removed with the member"),
		FindNodeByGuid(Fixture.Blueprint, Reference->NodeGuid));
	TestNull(TEXT("stale entry was replaced"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	TestEqual(TEXT("downstream presentation preserved"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 7. FailureAfterEntryRemovalRestores
// ---------------------------------------------------------------------------
namespace CortexGraphMigrationReplaceTest
{
/** Shared assertions of the two injected-failure restoration cases. */
void CheckInjectedFailureRestores(
	FAutomationTestBase& Test,
	const TCHAR* FixtureName,
	const TCHAR* FaultPoint,
	const TCHAR* Context)
{
	FFixture Fixture;
	Test.TestTrue(FString::Printf(TEXT("%s: fixture created"), Context), Fixture.Create(FixtureName));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("restore body"), 400, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Graph, TEXT("restore tail"), 800, 0);
	UEdGraphNode_Comment* Comment = AddCommentNode(Graph, TEXT("restore comment"), -100, -200);
	LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
	LinkNodes(Graph, Print, TEXT("then"), Tail, TEXT("execute"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	Test.TestTrue(FString::Printf(TEXT("%s: shadowing member added"), Context),
		AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));
	UK2Node_VariableGet* Reference = AddVariableGetNode(Graph, TEXT("OnPayload"), 0, 400);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Tail->NodeGuid);
	DownstreamGuids.Add(Comment->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const FString DigestBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Blueprint->GetOutermost()->IsDirty();
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110008"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap(), true, true), TEXT("OnPayload"));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	Test.TestTrue(FString::Printf(TEXT("%s: preview succeeds: %s"), Context, *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));

	FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(FaultPoint));
	const int32 TransactionsBefore = TransactionCount();
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
	Operations.End();
	ClearFaults();

	Test.TestFalse(FString::Printf(TEXT("%s: injected failure fails the apply [%s]"), Context, *Error.ErrorMessage), bApplied);
	Test.TestEqual(FString::Printf(TEXT("%s: apply reports the fault"), Context),
		Outcome.ApplyStatus, FString(TEXT("failed")));
	Test.TestEqual(FString::Printf(TEXT("%s: recovery restores"), Context),
		Outcome.RollbackStatus, FString(TEXT("restored")));
	Test.TestFalse(FString::Printf(TEXT("%s: recovery does not block the asset"), Context), Outcome.bBlocked);
	Test.TestEqual(FString::Printf(TEXT("%s: authoring fingerprint restored"), Context),
		LiveGraphHash(Fixture.Blueprint), HashBefore);
	Test.TestEqual(FString::Printf(TEXT("%s: generated state digest restored"), Context),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Fixture.Blueprint), DigestBefore);
	Test.TestEqual(FString::Printf(TEXT("%s: dirty state restored"), Context),
		Fixture.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	Test.TestEqual(FString::Printf(TEXT("%s: node count restored"), Context),
		CountNativeNodes(Fixture.Blueprint), NodesBefore);
	Test.TestEqual(FString::Printf(TEXT("%s: downstream capture restored"), Context),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);
	Test.TestTrue(FString::Printf(TEXT("%s: member state restored"), Context),
		FBlueprintEditorUtils::FindNewVariableIndex(Fixture.Blueprint, FName(TEXT("OnPayload"))) != INDEX_NONE);
	Test.TestNotNull(FString::Printf(TEXT("%s: member reference node restored"), Context),
		FindNodeByGuid(Fixture.Blueprint, Reference->NodeGuid));
	Test.TestNotNull(FString::Printf(TEXT("%s: stale entry restored"), Context),
		FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	Test.TestEqual(FString::Printf(TEXT("%s: no transaction is left open"), Context),
		TransactionCount(), TransactionsBefore);
	// The deterministic replacement identity of the refused apply is not present after recovery.
	const FGuid* ReplacementGuid = Prepared.NodeGuidByClientId.Find(TEXT("entry"));
	Test.TestTrue(FString::Printf(TEXT("%s: the plan published a replacement identity"), Context), ReplacementGuid != nullptr);
	if (ReplacementGuid)
	{
		Test.TestNull(FString::Printf(TEXT("%s: replacement identity is gone"), Context),
			FindNodeByGuid(Fixture.Blueprint, *ReplacementGuid));
	}
	Fixture.Cleanup();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationFailureAfterRemovalTest,
	"Cortex.Graph.Authoring.Migration.Replace.FailureAfterEntryRemovalRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationFailureAfterRemovalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();
	CheckInjectedFailureRestores(*this, TEXT("BP_MigrationFaultRemoval_T11"), TEXT("migration_entry_removed"),
		TEXT("failure after entry removal"));
	return true;
}

// ---------------------------------------------------------------------------
// 8. FailureAfterGraphRegistrationRestores
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationFailureAfterRegistrationTest,
	"Cortex.Graph.Authoring.Migration.Replace.FailureAfterGraphRegistrationRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationFailureAfterRegistrationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();
	CheckInjectedFailureRestores(*this, TEXT("BP_MigrationFaultRegistration_T11"), TEXT("migration_after_registration"),
		TEXT("failure after graph registration"));
	return true;
}

// ---------------------------------------------------------------------------
// 9. ReplacementReplayIsUnchanged
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplayTest,
	"Cortex.Graph.Authoring.Migration.Replace.ReplacementReplayIsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationReplay_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("replay body"), 400, 0);
	UEdGraphNode_Comment* Comment = AddCommentNode(Graph, TEXT("replay comment"), -100, -200);
	LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Comment->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);
	const FGuid StaleGuid = StaleEntry->NodeGuid;

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110009"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("replay first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("replay first apply succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, FirstOutcome, Error));
	TestEqual(TEXT("replay first apply reports the apply"), FirstOutcome.ApplyStatus, FString(TEXT("applied")));
	const FGuid* ReplacementGuid = FirstOutcome.Locators.NodeGuidByClientId.Find(TEXT("entry"));
	TestTrue(TEXT("replay first apply publishes the replacement identity"), ReplacementGuid != nullptr);
	const FString HashAfterFirst = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);

	// The identical request with the live fingerprint must reconcile as a complete reuse.
	TSharedPtr<FJsonObject> Replay = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000110009"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));
	const int32 TransactionsBefore = TransactionCount();
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch ReplayPrepared;
	TestTrue(FString::Printf(TEXT("replay preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Replay, ReplayPrepared, Error));
	TestFalse(TEXT("replay preview reports no prospective change"), ReplayPrepared.bChanged);
	TestTrue(TEXT("replay preview reports a complete reuse match"), ReplayPrepared.bFullyReused);
	TestEqual(TEXT("replay preview reports the reused deterministic identity"),
		FString::Join(ReplayPrepared.ReusedClientIds, TEXT(",")), FString(TEXT("entry")));
	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("replay applies as an idempotent no-op: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, Error));
	Operations.End();

	TestEqual(TEXT("replay reports unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("replay compiles nothing"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("replay performs no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("replay saves nothing"), Operations.Saves, 0);
	TestEqual(TEXT("replay leaves the fingerprint untouched"), LiveGraphHash(Fixture.Blueprint), HashAfterFirst);
	TestEqual(TEXT("replay does not duplicate nodes"), CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);
	TestEqual(TEXT("replay opens no transaction"), TransactionCount(), TransactionsBefore);
	TestEqual(TEXT("replay preserves the downstream presentation"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);
	TestNull(TEXT("replay keeps the stale entry gone"), FindNodeByGuid(Fixture.Blueprint, StaleGuid));

	// A different patch id for the same declaration is a conflicting identity state, not a replay.
	TSharedPtr<FJsonObject> Conflicting = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-00000011000a"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));
	FCortexGraphPatchOutcome ConflictingOutcome;
	TestFalse(TEXT("a conflicting identity state is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Conflicting, ConflictingOutcome, Error));
	TestEqual(TEXT("conflicting identity is INVALID_OPERATION"),
		Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestEqual(TEXT("conflicting identity mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashAfterFirst);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 10. MixedRequestRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationMixedRequestTest,
	"Cortex.Graph.Authoring.Migration.Replace.MixedRequestRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationMixedRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationMixed_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("ReceiveEndPlay"), ActorClassPath, 0, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);

	auto ExpectRefusal = [&](const FString& Context, const FString& ExpectedCode, const TSharedPtr<FJsonObject>& Request)
	{
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestFalse(FString::Printf(TEXT("%s: request is refused"), Context),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(FString::Printf(TEXT("%s: refusal code [%s]"), Context, *Error.ErrorMessage),
			Error.ErrorCode, ExpectedCode);
		TestEqual(FString::Printf(TEXT("%s: refusal mutates nothing"), Context),
			LiveGraphHash(Fixture.Blueprint), HashBefore);
	};

	auto FreshRequest = [&]()
	{
		return ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-00000011000b"),
			MakeMigration(Graph, StaleEntry, ReceiveBeginPlayPinMap()), TEXT("ReceiveBeginPlay"), ActorClassPath);
	};

	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), TEXT("note"));
		Node->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
		Request->SetArrayField(TEXT("nodes"), Nodes);
		ExpectRefusal(TEXT("migration with authoring nodes"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		TArray<TSharedPtr<FJsonValue>> Connections;
		TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
		From->SetBoolField(TEXT("entry"), true);
		From->SetStringField(TEXT("pin"), TEXT("then"));
		TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
		To->SetStringField(TEXT("client_id"), TEXT("note"));
		To->SetStringField(TEXT("pin"), TEXT("execute"));
		Connection->SetObjectField(TEXT("from"), From);
		Connection->SetObjectField(TEXT("to"), To);
		Connections.Add(MakeShared<FJsonValueObject>(Connection));
		Request->SetArrayField(TEXT("connections"), Connections);
		ExpectRefusal(TEXT("migration with authoring connections"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		TArray<TSharedPtr<FJsonValue>> Updates;
		Updates.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
		Request->SetArrayField(TEXT("pin_updates"), Updates);
		ExpectRefusal(TEXT("migration with authoring pin updates"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->SetStringField(TEXT("op"), TEXT("copy_subgraph"));
		ExpectRefusal(TEXT("unknown migration operation"), CortexErrorCodes::UnsupportedOperation, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->SetStringField(TEXT("sneaky"), TEXT("field"));
		ExpectRefusal(TEXT("unknown migration field"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->GetObjectField(TEXT("source"))->SetStringField(TEXT("sneaky"), TEXT("field"));
		ExpectRefusal(TEXT("unknown source field"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		TArray<TSharedPtr<FJsonValue>> Map = ReceiveBeginPlayPinMap();
		Map[0]->AsObject()->SetStringField(TEXT("sneaky"), TEXT("field"));
		Request->GetObjectField(TEXT("migration"))->SetArrayField(TEXT("pin_map"), Map);
		ExpectRefusal(TEXT("unknown pin_map field"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->SetStringField(TEXT("remove_shadowing_member"), TEXT("true"));
		ExpectRefusal(TEXT("non-boolean remove_shadowing_member"), CortexErrorCodes::InvalidField, Request);
	}
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->SetArrayField(TEXT("pin_map"), TArray<TSharedPtr<FJsonValue>>());
		ExpectRefusal(TEXT("empty pin map"), CortexErrorCodes::InvalidField, Request);
	}
	// Every published flag is a strict JSON boolean: a numeric or string value is refused, never
	// coerced (design 4.4: "malformed flag types fail; no silent flag coercion").
	auto ExpectFlagRefusal = [&](const TCHAR* Field, const TCHAR* Value, const int32 TrackToken)
	{
		{
			TSharedPtr<FJsonObject> Request = FreshRequest();
			Request->SetNumberField(Field, TrackToken);
			ExpectRefusal(FString::Printf(TEXT("%s as a number"), Field), CortexErrorCodes::InvalidField, Request);
		}
		{
			TSharedPtr<FJsonObject> Request = FreshRequest();
			Request->SetStringField(Field, Value);
			ExpectRefusal(FString::Printf(TEXT("%s as a string"), Field), CortexErrorCodes::InvalidField, Request);
		}
	};
	ExpectFlagRefusal(TEXT("dry_run"), TEXT("false"), 0);
	ExpectFlagRefusal(TEXT("compile"), TEXT("true"), 1);
	ExpectFlagRefusal(TEXT("save"), TEXT("false"), 0);
	ExpectFlagRefusal(TEXT("allow_noop"), TEXT("true"), 1);
	{
		TSharedPtr<FJsonObject> Request = FreshRequest();
		Request->GetObjectField(TEXT("migration"))->SetNumberField(TEXT("remove_shadowing_member"), 1);
		ExpectRefusal(TEXT("remove_shadowing_member as a number"), CortexErrorCodes::InvalidField, Request);
	}
	{
		// The target of a migration must be the implementation selector.
		TSharedPtr<FJsonObject> Request = FreshRequest();
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		Target->SetObjectField(TEXT("graph_ref"), GraphRef);
		Request->SetObjectField(TEXT("target"), Target);
		ExpectRefusal(TEXT("migration with a graph target"), CortexErrorCodes::InvalidField, Request);
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 11. StaleTokenRejected
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationStaleTokenTest,
	"Cortex.Graph.Authoring.Migration.Replace.StaleTokenRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationStaleTokenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationStale_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("stale body"), 400, 0);
	LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-00000011000c"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap()), TEXT("OnPayload"));
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("stale token preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));

	// The same mapping set in a different order is a valid request on its own, but the token was
	// minted for the previous intent bytes, so the changed migration intent invalidates it without
	// any mutation.
	{
		TArray<TSharedPtr<FJsonValue>> Reordered = OnPayloadPinMap();
		Algo::Reverse(Reordered);
		TSharedPtr<FJsonObject> Changed = ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-00000011000c"),
			MakeMigration(Graph, StaleEntry, Reordered),
			TEXT("OnPayload"));
		FCortexGraphPreparedPatch ChangedPreview;
		FCortexCommandResult PreviewError;
		TestTrue(FString::Printf(TEXT("the changed migration intent is valid on its own: %s"), *PreviewError.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Changed, ChangedPreview, PreviewError));
		TestNotEqual(TEXT("the changed migration intent has a different validation hash"),
			ChangedPreview.ValidationHash, Prepared.ValidationHash);
		Changed->SetBoolField(TEXT("dry_run"), false);
		Changed->SetStringField(TEXT("expected_validation_hash"), Prepared.ValidationHash);
		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		FCortexGraphPatchOutcome Outcome;
		TestFalse(TEXT("a changed migration intent invalidates the preview token"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Changed, Outcome, Error));
		TestEqual(TEXT("changed migration intent is STALE_PRECONDITION"),
			Error.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
		TestEqual(TEXT("changed migration intent mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestNotNull(TEXT("stale entry is untouched"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	}

	// A source-graph edit invalidates the fingerprint the token was computed over.
	{
		AddPrintNode(Graph, TEXT("late edit"), 1200, 0);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Fixture.Blueprint);
		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		FCortexGraphPatchOutcome Outcome;
		TestFalse(TEXT("a source-graph edit invalidates the preview"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(TEXT("edited source graph is STALE_PRECONDITION"),
			Error.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
		TestEqual(TEXT("stale refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestNotNull(TEXT("stale entry is untouched"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 12. PreviewNeverMutates
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPreviewTest,
	"Cortex.Graph.Authoring.Migration.Replace.PreviewNeverMutates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationPreview_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	ClearGraphNodes(Graph);
	UK2Node_Event* StaleEntry = AddEventNode(Graph, TEXT("OnPayload"), *FixtureActorClassPath(), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("preview body"), 400, 0);
	UEdGraphNode_Comment* Comment = AddCommentNode(Graph, TEXT("preview comment"), -100, -200);
	LinkNodes(Graph, StaleEntry, TEXT("then"), Print, TEXT("execute"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("shadowing member added"), AddShadowingVariable(Fixture.Blueprint, TEXT("OnPayload")));
	UK2Node_VariableGet* Reference = AddVariableGetNode(Graph, TEXT("OnPayload"), 0, 400);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Comment->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Blueprint->GetOutermost()->IsDirty();
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const int32 VariablesBefore = Fixture.Blueprint->NewVariables.Num();

	// A preview that would remove the shadowing member and replace the entry.
	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-00000011000d"),
		MakeMigration(Graph, StaleEntry, OnPayloadPinMap(), true, true), TEXT("OnPayload"));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
	Operations.End();

	TestTrue(TEXT("preview reports a prospective change"), Prepared.bChanged);
	TestFalse(TEXT("preview reports no reuse"), Prepared.bFullyReused);
	TestEqual(TEXT("preview compiles nothing"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("preview performs no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("preview saves nothing"), Operations.Saves, 0);
	TestEqual(TEXT("preview leaves the fingerprint untouched"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("preview leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestEqual(TEXT("preview leaves the dirty state"), Fixture.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("preview keeps the user member"), Fixture.Blueprint->NewVariables.Num(), VariablesBefore);
	TestNotNull(TEXT("preview keeps the stale entry"), FindNodeByGuid(Fixture.Blueprint, StaleEntry->NodeGuid));
	TestNotNull(TEXT("preview keeps the member reference node"), FindNodeByGuid(Fixture.Blueprint, Reference->NodeGuid));
	TestEqual(TEXT("preview preserves the downstream presentation"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 13. Function-graph terminator replacement (entry plus result)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationFunctionGraphTest,
	"Cortex.Graph.Authoring.Migration.Replace.FunctionGraphTerminators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationFunctionGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationReplaceTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_MigrationFunction_T11")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }

	FStaleFunctionGraph Stale = AddStaleFunctionGraph(Fixture.Blueprint, TEXT("ComputeScore"), *FixtureActorClassPath());
	UK2Node_CallFunction* Print = AddPrintNode(Stale.Graph, TEXT("function body"), 300, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Stale.Graph, TEXT("function tail"), 600, 0);
	UEdGraphNode_Comment* Comment = AddCommentNode(Stale.Graph, TEXT("function comment"), -100, -200);
	TestTrue(TEXT("entry to body link wired"), LinkNodes(Stale.Graph, Stale.Entry, TEXT("then"), Print, TEXT("execute")));
	TestTrue(TEXT("entry data link wired"), LinkNodes(Stale.Graph, Stale.Entry, TEXT("Tag"), Print, TEXT("InString")));
	TestTrue(TEXT("body to result link wired"), LinkNodes(Stale.Graph, Print, TEXT("then"), Stale.Result, TEXT("execute")));
	TestTrue(TEXT("tail to result link wired"), LinkNodes(Stale.Graph, Tail, TEXT("then"), Stale.Result, TEXT("execute")));
	TestTrue(TEXT("entry to result internal link wired"), LinkNodes(Stale.Graph, Stale.Entry, TEXT("then"), Stale.Result, TEXT("execute")));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	TArray<FGuid> DownstreamGuids;
	DownstreamGuids.Add(Print->NodeGuid);
	DownstreamGuids.Add(Tail->NodeGuid);
	DownstreamGuids.Add(Comment->NodeGuid);
	const FString DownstreamBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids);

	// The replaced set: what the stale terminators carried outside it is the boundary contract.
	TArray<FGuid> ReplacedGuids;
	ReplacedGuids.Add(Stale.Entry->NodeGuid);
	ReplacedGuids.Add(Stale.Result->NodeGuid);
	const TArray<FString> EntryBoundaryBefore = BoundaryFarEndpoints(Stale.Entry, TEXT("then"), ReplacedGuids);
	const TArray<FString> ResultBoundaryBefore = BoundaryFarEndpoints(Stale.Result, TEXT("execute"), ReplacedGuids);

	TSharedPtr<FJsonObject> Request = ReplacementRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-00000011000e"),
		MakeMigration(Stale.Graph, Stale.Entry, ComputeScorePinMap()), TEXT("ComputeScore"));

	// F7/F2: an out-parameter/result-terminator incompatibility (an array result mapped onto the
	// scalar return input) must refuse in preflight with nothing mutated.
	{
		TSharedPtr<FJsonObject> BadRequest = ReplacementRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-00000011000e"),
			MakeMigration(Stale.Graph, Stale.Entry, WithEntry(ComputeScorePinMap(), TEXT("ReturnValue"), TEXT("input"), TEXT("OutIds"))),
			TEXT("ComputeScore"));
		const FString HashBeforeBad = LiveGraphHash(Fixture.Blueprint);
		const int32 NodesBeforeBad = CountNativeNodes(Fixture.Blueprint);
		FCortexGraphPatchOutcome BadOutcome;
		FCortexCommandResult BadError;
		TestFalse(TEXT("an incompatible result-terminator mapping is refused"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, BadRequest, BadOutcome, BadError));
		TestEqual(TEXT("result-terminator incompatibility is TYPE_MISMATCH"),
			BadError.ErrorCode, FString(CortexErrorCodes::TypeMismatch));
		TestTrue(FString::Printf(TEXT("refusal names the container dimension [%s]"), *BadError.ErrorMessage),
			BadError.ErrorMessage.Contains(TEXT("container")));
		TestEqual(TEXT("result-terminator refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBeforeBad);
		TestEqual(TEXT("result-terminator refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBeforeBad);
	}

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("function-graph replacement previews: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	TestTrue(TEXT("function-graph preview publishes the entry identity"),
		Prepared.NodeGuidByClientId.Contains(TEXT("entry")));
	TestTrue(TEXT("function-graph preview publishes the result identity"),
		Prepared.NodeGuidByClientId.Contains(TEXT("result")));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("function-graph replacement succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("function-graph apply reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("function-graph compile succeeds"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("function-graph target compile count"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("function-graph readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("function-graph does not block"), Outcome.bBlocked, false);
	TestEqual(TEXT("function-graph downstream presentation preserved"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, DownstreamGuids), DownstreamBefore);
	TestNull(TEXT("stale function entry replaced"), FindNodeByGuid(Fixture.Blueprint, Stale.Entry->NodeGuid));
	TestNull(TEXT("stale function result replaced"), FindNodeByGuid(Fixture.Blueprint, Stale.Result->NodeGuid));

	const FGuid* EntryGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("entry"));
	const FGuid* ResultGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("result"));
	UEdGraphNode* ReplacementEntry = EntryGuid ? FindNodeByGuid(Fixture.Blueprint, *EntryGuid) : nullptr;
	UEdGraphNode* ReplacementResult = ResultGuid ? FindNodeByGuid(Fixture.Blueprint, *ResultGuid) : nullptr;
	TestNotNull(TEXT("replacement function entry resolves"), ReplacementEntry);
	TestNotNull(TEXT("replacement function result resolves"), ReplacementResult);
	if (ReplacementEntry && ReplacementResult)
	{
		UEdGraphPin* EntryThen = ReplacementEntry->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* ResultExec = ReplacementResult->FindPin(UEdGraphSchema_K2::PN_Execute);
		UEdGraphPin* PrintExec = Print->FindPin(TEXT("execute"));
		UEdGraphPin* PrintThen = Print->FindPin(UEdGraphSchema_K2::PN_Then);
		TArray<FGuid> ReplacementGuids;
		ReplacementGuids.Add(ReplacementEntry->NodeGuid);
		ReplacementGuids.Add(ReplacementResult->NodeGuid);
		TestEqual(TEXT("the replacement entry realizes every mapped boundary link of the stale entry"),
			FString::Join(BoundaryFarEndpoints(ReplacementEntry, TEXT("then"), ReplacementGuids), TEXT(",")),
			FString::Join(EntryBoundaryBefore, TEXT(",")));
		TestEqual(TEXT("the replacement result realizes every mapped boundary link of the stale result"),
			FString::Join(BoundaryFarEndpoints(ReplacementResult, TEXT("execute"), ReplacementGuids), TEXT(",")),
			FString::Join(ResultBoundaryBefore, TEXT(",")));
		(void)PrintExec;
		TestTrue(TEXT("body feeds the replacement result"),
			PrintThen && ResultExec && PrintThen->LinkedTo.Contains(ResultExec));
		TestTrue(TEXT("the internal entry/result link was remapped"),
			EntryThen && ResultExec && EntryThen->LinkedTo.Contains(ResultExec));
		UEdGraphPin* EntryTag = ReplacementEntry->FindPin(TEXT("Tag"));
		UEdGraphPin* PrintInString = Print->FindPin(TEXT("InString"));
		TestTrue(TEXT("the mapped data link was remapped"),
			EntryTag && PrintInString && EntryTag->LinkedTo.Contains(PrintInString));
	}

	Fixture.Cleanup();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
