#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphMigrationTestTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
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
 * `migration.op="prune_island"` coverage.
 *
 * Every case drives the real native entry points (`FCortexGraphPatchOps::Preflight` / `Execute`), so
 * the envelope, the durable plan, the bounded graph-wide scan, the journal and the native readback
 * are exercised exactly as a connected editor exercises them. The preservation oracles below are
 * independent test-side captures of native state, so a shared node is only proven preserved when an
 * oracle the operation never planned against says so.
 */
namespace CortexGraphMigrationPruneTest
{
/** Observations around real coordinator operations, installed per test. */
struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;

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
	}

	void End()
	{
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

private:
	static FOperations* Active;
};

FOperations* FOperations::Active = nullptr;

void ClearFaults()
{
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
	FCortexGraphMigrationOps::ClearPruneReadbackFaultForTesting();
}

void ResetTransaction()
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationPruneTestCleanup")));
	}
}

int32 TransactionCount()
{
	return (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
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

UEdGraphNode* FindNodeByGuidInGraph(UEdGraph* Graph, const FGuid& NodeGuid)
{
	return FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, NodeGuid);
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

int32 CountLinkedPins(UEdGraphNode* Node, const TCHAR* PinName)
{
	UEdGraphPin* Pin = Node ? Node->FindPin(FName(PinName)) : nullptr;
	return Pin ? Pin->LinkedTo.Num() : -1;
}

bool NodesLinked(UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
{
	UEdGraphPin* Source = From ? From->FindPin(FName(FromPin)) : nullptr;
	UEdGraphPin* Target = To ? To->FindPin(FName(ToPin)) : nullptr;
	return Source != nullptr && Target != nullptr && Source->LinkedTo.Contains(Target);
}

/**
 * Independent single-node preservation oracle: canonical native state of the selected nodes plus the
 * links whose two ends are both inside that selection. It compares intrinsic state and internal
 * selected-set edges only, so retained-consumer and removed boundary links stay separate assertions.
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
		FString Capture = FString::Printf(TEXT("guid=%s class=%s pos=(%d,%d) comment=\"%s\" bubble=%d/%d enabled=%d/%d/%d"),
			*Node->NodeGuid.ToString(), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY,
			*Node->NodeComment, Node->bCommentBubblePinned ? 1 : 0, Node->bCommentBubbleVisible ? 1 : 0,
			static_cast<int32>(Node->GetDesiredEnabledState()),
			Node->HasUserSetTheEnabledState() ? 1 : 0, Node->IsDisplayAsDisabledForced() ? 1 : 0);
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UClass* Owner = Call->FunctionReference.GetMemberParentClass();
			Capture += FString::Printf(TEXT(" symbol=%s@%s pure=%d"),
				*Call->FunctionReference.GetMemberName().ToString(),
				Owner ? *Owner->GetPathName() : TEXT("none"), Call->IsNodePure() ? 1 : 0);
		}
		else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			Capture += FString::Printf(TEXT(" var=%s self=%d set=%d"),
				*Variable->VariableReference.GetMemberName().ToString(),
				Variable->VariableReference.IsSelfContext() ? 1 : 0,
				Variable->IsA<UK2Node_VariableSet>() ? 1 : 0);
		}
		else if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Capture += FString::Printf(TEXT(" customEvent=%s"), *CustomEvent->CustomFunctionName.ToString());
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
			Pins.Add(FString::Printf(TEXT("%s|dir=%d|cat=%s|sub=%s|container=%d|def=%s|links=[%s]"),
				*Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction),
				*Pin->PinType.PinCategory.ToString(),
				*Pin->PinType.PinSubCategory.ToString(),
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

/** Independent whole-graph canonical capture, used to prove an exact restoration. */
FString CaptureGraphNative(UBlueprint* Blueprint, UEdGraph* Graph)
{
	if (!Blueprint || !Graph) return FString();
	TSet<FGuid> InGraph;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node) InGraph.Add(Node->NodeGuid);
	}
	TArray<FString> Captures;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FString Capture = FString::Printf(TEXT("guid=%s name=%s class=%s pos=(%d,%d) comment=\"%s\" bubble=%d/%d enabled=%d/%d/%d"),
			*Node->NodeGuid.ToString(), *Node->GetName(), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY,
			*Node->NodeComment, Node->bCommentBubblePinned ? 1 : 0, Node->bCommentBubbleVisible ? 1 : 0,
			static_cast<int32>(Node->GetDesiredEnabledState()),
			Node->HasUserSetTheEnabledState() ? 1 : 0, Node->IsDisplayAsDisabledForced() ? 1 : 0);
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UClass* Owner = Call->FunctionReference.GetMemberParentClass();
			Capture += FString::Printf(TEXT(" call=%s@%s self=%d"),
				*Call->FunctionReference.GetMemberName().ToString(),
				Owner ? *Owner->GetPathName() : TEXT("none"),
				Call->FunctionReference.IsSelfContext() ? 1 : 0);
		}
		else if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
		{
			Capture += FString::Printf(TEXT(" customEvent=%s"), *CustomEvent->CustomFunctionName.ToString());
		}
		else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			Capture += FString::Printf(TEXT(" var=%s self=%d set=%d"),
				*Variable->VariableReference.GetMemberName().ToString(),
				Variable->VariableReference.IsSelfContext() ? 1 : 0,
				Variable->IsA<UK2Node_VariableSet>() ? 1 : 0);
		}
		TArray<FString> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin != nullptr) continue;
			TArray<FString> Links;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!LinkedNode || !InGraph.Contains(LinkedNode->NodeGuid)) continue;
				Links.Add(FString::Printf(TEXT("%s.%s"), *LinkedNode->NodeGuid.ToString(), *LinkedPin->PinName.ToString()));
			}
			Links.Sort();
			Pins.Add(FString::Printf(TEXT("%s|dir=%d|cat=%s|sub=%s|container=%d|def=%s|links=[%s]"),
				*Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction),
				*Pin->PinType.PinCategory.ToString(),
				*Pin->PinType.PinSubCategory.ToString(),
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

/** A short description of the first differing line of two canonical captures. */
FString FirstCaptureDifference(const FString& Left, const FString& Right)
{
	TArray<FString> LeftLines;
	TArray<FString> RightLines;
	Left.ParseIntoArrayLines(LeftLines, false);
	Right.ParseIntoArrayLines(RightLines, false);
	const int32 Count = FMath::Max(LeftLines.Num(), RightLines.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FString LeftLine = LeftLines.IsValidIndex(Index) ? LeftLines[Index] : FString(TEXT("<missing>"));
		const FString RightLine = RightLines.IsValidIndex(Index) ? RightLines[Index] : FString(TEXT("<missing>"));
		if (LeftLine != RightLine)
		{
			return FString::Printf(TEXT("line %d: before='%s' after='%s'"), Index, *LeftLine, *RightLine);
		}
	}
	return FString();
}

/**
 * Canonical link set between the nodes of one graph whose identity is inside Filter. A retained
 * node's link set is the oracle for "every link to a retained consumer survives exactly".
 */
TArray<FString> CollectLinkSet(UEdGraph* Graph, const TSet<FGuid>& Filter)
{
	TArray<FString> Links;
	if (!Graph) return Links;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || !Filter.Contains(Node->NodeGuid)) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin != nullptr) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!FarNode || !Filter.Contains(FarNode->NodeGuid)) continue;
				Links.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Node->NodeGuid.ToString(),
					*Pin->PinName.ToString(), *FarNode->NodeGuid.ToString(), *LinkedPin->PinName.ToString()));
			}
		}
	}
	Links.Sort();
	return Links;
}

/** Everything a request must be able to prove unchanged after a refusal. */
struct FFixtureState
{
	FString Hash;
	int32 Nodes = 0;
	bool bDirty = false;
};

FFixtureState Observe(UBlueprint* Blueprint)
{
	FFixtureState State;
	State.Hash = LiveGraphHash(Blueprint);
	State.Nodes = CountNativeNodes(Blueprint);
	State.bDirty = Blueprint->GetOutermost()->IsDirty();
	return State;
}

/** Fixture Blueprint in a transient package. */
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;

	bool Create(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexGraphMigrationFixtureActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
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

UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
{
	if (Blueprint->UbergraphPages.Num() > 0) return Blueprint->UbergraphPages[0];
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	return Graph;
}

/** One entry terminator of the fixture: a custom event with a real exec output. */
UK2Node_CustomEvent* AddEntryNode(UEdGraph* Graph, const TCHAR* Name, const int32 X, const int32 Y)
{
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = FName(Name);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();
	Event->NodePosX = X;
	Event->NodePosY = Y;
	Event->NodeComment = FString::Printf(TEXT("entry %s"), Name);
	Event->bCommentBubblePinned = true;
	Graph->AddNode(Event, true, false);
	return Event;
}

UK2Node_CallFunction* AddCallFunctionNode(UEdGraph* Graph, const FName FunctionName, UClass* OwnerClass, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FunctionName, OwnerClass);
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

UK2Node_CallFunction* AddPrintNode(UEdGraph* Graph, const TCHAR* Text, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = AddCallFunctionNode(Graph, FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass(), X, Y);
	UEdGraphPin* TextPin = Call->FindPin(FName(TEXT("InString")));
	if (TextPin) TextPin->DefaultValue = Text;
	return Call;
}

/** A pure producer of the fixtures: `Conv_IntToString`, so shared pure data has a real symbol. */
UK2Node_CallFunction* AddPureProducerNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = AddCallFunctionNode(Graph, FName(TEXT("Conv_IntToString")), UKismetStringLibrary::StaticClass(), X, Y);
	UEdGraphPin* InputPin = Call->FindPin(FName(TEXT("InInt")));
	if (InputPin) InputPin->DefaultValue = TEXT("7");
	return Call;
}

UK2Node_CallFunction* AddIntAddNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = AddCallFunctionNode(Graph, FName(TEXT("Add_IntInt")), UKismetMathLibrary::StaticClass(), X, Y);
	UEdGraphPin* BPin = Call->FindPin(FName(TEXT("B")));
	if (BPin) BPin->DefaultValue = TEXT("1");
	return Call;
}

/** Declares an int32 member before the fixture is compiled, so a setter node can address it. */
void DeclareFixtureMember(UBlueprint* Blueprint, const TCHAR* VariableName)
{
	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(VariableName), IntType);
}

/** A setter of a declared member: it carries exec pins plus the value it writes and its read back. */
UK2Node_VariableSet* AddVariableSetNode(UEdGraph* Graph, const TCHAR* VariableName, const int32 X, const int32 Y)
{
	UK2Node_VariableSet* Set = NewObject<UK2Node_VariableSet>(Graph);
	Set->VariableReference.SetSelfMember(FName(VariableName));
	Set->CreateNewGuid();
	Set->AllocateDefaultPins();
	Set->NodePosX = X;
	Set->NodePosY = Y;
	Graph->AddNode(Set, true, false);
	return Set;
}

bool LinkNodes(UEdGraph* Graph, UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
{
	UEdGraphPin* Source = From ? From->FindPin(FName(FromPin)) : nullptr;
	UEdGraphPin* Target = To ? To->FindPin(FName(ToPin)) : nullptr;
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	return Source != nullptr && Target != nullptr && Schema != nullptr && Schema->TryCreateConnection(Source, Target);
}

// ---------------------------------------------------------------------------
// Request builders
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> PruneRequestForEntryGuid(
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	UEdGraph* Graph,
	const FString& EntryGuid,
	const TArray<FGuid>& Approved,
	const bool bIncludeApproved = true,
	const bool bCompile = true)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetArrayField(TEXT("pin_updates"), Empty);

	TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
	Migration->SetStringField(TEXT("op"), TEXT("prune_island"));
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
	Source->SetObjectField(TEXT("graph_ref"), GraphRef);
	Source->SetStringField(TEXT("entry_node_guid"), EntryGuid);
	Migration->SetObjectField(TEXT("source"), Source);
	if (bIncludeApproved)
	{
		TArray<TSharedPtr<FJsonValue>> ApprovedValues;
		for (const FGuid& Guid : Approved)
		{
			ApprovedValues.Add(MakeShared<FJsonValueString>(Guid.ToString()));
		}
		Migration->SetArrayField(TEXT("approved_node_guids"), ApprovedValues);
	}
	Request->SetObjectField(TEXT("migration"), Migration);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), bCompile);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

TSharedPtr<FJsonObject> PruneRequest(
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	UEdGraph* Graph,
	UEdGraphNode* Entry,
	const TArray<FGuid>& Approved,
	const bool bIncludeApproved = true,
	const bool bCompile = true)
{
	return PruneRequestForEntryGuid(Blueprint, PatchId, Graph, Entry->NodeGuid.ToString(), Approved, bIncludeApproved, bCompile);
}

/** A preview that stays a preview: it must never carry apply intent. */
bool PreviewPrune(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexGraphPreparedPatch& OutPrepared,
	FCortexGraphMigrationPrunePlan& OutPlan,
	FCortexCommandResult& OutError)
{
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, OutPrepared, OutError)) return false;
	if (!OutPrepared.PrunePlan.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("the preview published no prune plan"));
		return false;
	}
	return FCortexGraphMigrationPrunePlan::FromJson(OutPrepared.PrunePlan, OutPlan, OutError);
}

/** Turns the prepared request into the apply of the same intent. */
bool PrepareApply(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexGraphPreparedPatch& OutPrepared,
	FCortexGraphMigrationPrunePlan& OutPlan,
	FCortexCommandResult& OutError)
{
	if (!PreviewPrune(Blueprint, Request, OutPrepared, OutPlan, OutError)) return false;
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), OutPrepared.ValidationHash);
	return true;
}

TArray<FString> GuidStrings(const TArray<FGuid>& Guids)
{
	TArray<FString> Values;
	for (const FGuid& Guid : Guids) Values.Add(Guid.ToString());
	Values.Sort();
	return Values;
}

bool PlanContainsGuid(const TArray<FString>& Guids, const FGuid& Guid)
{
	return Guids.Contains(Guid.ToString());
}

const FCortexGraphPruneNode* FindPartitionNode(const TArray<FCortexGraphPruneNode>& Nodes, const FGuid& Guid)
{
	return Nodes.FindByPredicate(
		[&Guid](const FCortexGraphPruneNode& Candidate) { return Candidate.NodeGuid == Guid.ToString(); });
}

bool PlanHasExternalEdge(const FCortexGraphMigrationPrunePlan& Plan, UEdGraphNode* OutputNode, const TCHAR* OutputPin,
	UEdGraphNode* InputNode, const TCHAR* InputPin)
{
	// The planned edge is canonicalized with the output endpoint first, so both orders are accepted.
	return Plan.ExternalEdges.ContainsByPredicate(
		[&](const FCortexGraphPruneEdge& Edge)
		{
			const bool bForward = Edge.FromGuid == OutputNode->NodeGuid.ToString() && Edge.FromPin == OutputPin
				&& Edge.ToGuid == InputNode->NodeGuid.ToString() && Edge.ToPin == InputPin;
			const bool bReverse = Edge.FromGuid == InputNode->NodeGuid.ToString() && Edge.FromPin == InputPin
				&& Edge.ToGuid == OutputNode->NodeGuid.ToString() && Edge.ToPin == OutputPin;
			return bForward || bReverse;
		});
}

/** Fixture shape shared by the preservation cases: one island chain plus a retained chain. */
struct FSharedProducerFixture
{
	FFixture Fixture;
	UEdGraph* Graph = nullptr;
	UK2Node_CustomEvent* Entry = nullptr;
	UK2Node_CallFunction* Producer = nullptr;
	UK2Node_CallFunction* IslandPrint = nullptr;
	UK2Node_CallFunction* RetainedPrint = nullptr;
	UK2Node_CustomEvent* RetainedEntry = nullptr;

	bool Build(const TCHAR* Name)
	{
		if (!Fixture.Create(Name)) return false;
		Graph = EnsureEventGraph(Fixture.Blueprint);
		Entry = AddEntryNode(Graph, TEXT("CortexPruneIslandEntry"), 0, 0);
		Producer = AddPureProducerNode(Graph, 300, 200);
		IslandPrint = AddPrintNode(Graph, TEXT("island"), 600, 0);
		RetainedPrint = AddPrintNode(Graph, TEXT("retained"), 600, 400);
		RetainedEntry = AddEntryNode(Graph, TEXT("CortexPruneRetainedEntry"), 0, 800);
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		LinkNodes(Graph, Entry, TEXT("then"), IslandPrint, TEXT("execute"));
		LinkNodes(Graph, RetainedEntry, TEXT("then"), RetainedPrint, TEXT("execute"));
		LinkNodes(Graph, Producer, TEXT("ReturnValue"), IslandPrint, TEXT("InString"));
		LinkNodes(Graph, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString"));
		return true;
	}

	void Cleanup() { Fixture.Cleanup(); }
};
}

// ---------------------------------------------------------------------------
// 1. TwoEntriesSharedPureProducer
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneTwoEntriesSharedProducerTest,
	"Cortex.Graph.Authoring.Migration.Prune.TwoEntriesSharedPureProducer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneTwoEntriesSharedProducerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FSharedProducerFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Build(TEXT("BP_PruneSharedProducer_T13")));
	if (!Fixture.Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* const Graph = Fixture.Graph;
	UK2Node_CallFunction* const Producer = Fixture.Producer;
	UK2Node_CallFunction* const IslandPrint = Fixture.IslandPrint;
	UK2Node_CallFunction* const RetainedPrint = Fixture.RetainedPrint;
	UK2Node_CustomEvent* const RetainedEntry = Fixture.RetainedEntry;
	TestTrue(TEXT("the shared producer feeds both entries"), CountLinkedPins(Producer, TEXT("ReturnValue")) == 2);

	const FFixtureState Before = Observe(Fixture.Fixture.Blueprint);
	const TArray<FGuid> SharedGuids = { Producer->NodeGuid };
	const FString SharedBefore = CaptureSelectedNativeNodes(Fixture.Fixture.Blueprint, SharedGuids);

	// Preview without an approval: the partition is published before anything is approved.
	FCortexGraphPreparedPatch PreviewPrepared;
	FCortexGraphMigrationPrunePlan PreviewPlan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the prune partition previews: %s"), *Error.ErrorMessage),
		PreviewPrune(Fixture.Fixture.Blueprint, PruneRequest(Fixture.Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131101"),
			Graph, Fixture.Entry, {}, false), PreviewPrepared, PreviewPlan, Error));
	TestTrue(TEXT("the partition awaiting approval removes nothing"), PreviewPlan.bAwaitingApproval);
	TestTrue(TEXT("the partition is complete"), PreviewPlan.bComplete);
	TestTrue(TEXT("the shared producer is not removable"), !PlanContainsGuid(PreviewPlan.ApprovedGuids, Producer->NodeGuid));
	TestNotNull(TEXT("the shared producer is reported as shared"), FindPartitionNode(PreviewPlan.Shared, Producer->NodeGuid));
	TestTrue(TEXT("the entry-to-island link is an approved boundary edge"),
		PlanHasExternalEdge(PreviewPlan, Fixture.Entry, TEXT("then"), IslandPrint, TEXT("execute")));
	TestTrue(TEXT("the shared-producer-to-island link is an approved boundary edge"),
		PlanHasExternalEdge(PreviewPlan, Producer, TEXT("ReturnValue"), IslandPrint, TEXT("InString")));
	TestFalse(TEXT("the retained-consumer link is not a boundary edge"),
		PlanHasExternalEdge(PreviewPlan, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));
	TestEqual(TEXT("the partition mutates no state"), LiveGraphHash(Fixture.Fixture.Blueprint), Before.Hash);
	TestEqual(TEXT("the partition mutates no node"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes);

	// The same partition is published through the command path.
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		const FCortexCommandResult PreviewResult = Router.Execute(TEXT("graph.apply_patch"),
			PruneRequest(Fixture.Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131101"), Graph, Fixture.Entry, {}, false));
		TestTrue(FString::Printf(TEXT("the preview succeeds through the command path: %s"), *PreviewResult.ErrorMessage),
			PreviewResult.bSuccess);
		if (PreviewResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Removable = nullptr;
			TestTrue(TEXT("the preview publishes the removable set"),
				PreviewResult.Data->TryGetArrayField(TEXT("removable"), Removable) && Removable != nullptr
					&& Removable->Num() == 1);
			const TArray<TSharedPtr<FJsonValue>>* Shared = nullptr;
			TestTrue(TEXT("the preview publishes the shared partition"),
				PreviewResult.Data->TryGetArrayField(TEXT("shared"), Shared) && Shared != nullptr && Shared->Num() > 0);
			bool bBlocked = true;
			TestTrue(TEXT("the prune inventory preserves the Boolean asset-block status"),
				PreviewResult.Data->TryGetBoolField(TEXT("blocked"), bBlocked) && !bBlocked);
			const TArray<TSharedPtr<FJsonValue>>* BlockedNodes = nullptr;
			TestTrue(TEXT("the blocked-node partition has a distinct field"),
				PreviewResult.Data->TryGetArrayField(TEXT("blocked_nodes"), BlockedNodes) && BlockedNodes != nullptr);
			const TArray<TSharedPtr<FJsonValue>>* ExternalEdges = nullptr;
			TestTrue(TEXT("the preview publishes the external edges"),
				PreviewResult.Data->TryGetArrayField(TEXT("external_edges"), ExternalEdges) && ExternalEdges != nullptr
					&& ExternalEdges->Num() == 2);
			TestTrue(TEXT("the preview publishes its scan counts"),
				PreviewResult.Data->GetIntegerField(TEXT("scanned_nodes")) > 0
					&& PreviewResult.Data->GetIntegerField(TEXT("scan_limit")) == FCortexGraphPatchOps::MaxScannedNodes);
		}
	}

	// Apply the approved removable set.
	const TArray<FGuid> Approved = { IslandPrint->NodeGuid };
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131101"),
		Graph, Fixture.Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	TestTrue(FString::Printf(TEXT("the approved prune prepares: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestTrue(TEXT("the approved prune is a prospective change"), Prepared.bChanged);
	TestFalse(TEXT("the approved prune is not a reuse"), Plan.bReused);
	TestEqual(TEXT("the approved set is the canonical removable set"), Plan.ApprovedGuids, GuidStrings(Approved));

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the bounded prune applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();
	TestEqual(TEXT("the prune reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the prune compiles once"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("the prune compiles successfully"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("the prune readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestFalse(TEXT("the proven prune does not block"), Outcome.bBlocked);
	TestEqual(TEXT("the prune removes exactly the approved node"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes - 1);
	TestNull(TEXT("the approved node is gone"), FindNodeByGuidInGraph(Graph, IslandPrint->NodeGuid));
	TestNotNull(TEXT("the shared producer survives"), FindNodeByGuidInGraph(Graph, Producer->NodeGuid));
	TestNotNull(TEXT("the entry survives"), FindNodeByGuidInGraph(Graph, Fixture.Entry->NodeGuid));
	TestNotNull(TEXT("the retained chain survives"), FindNodeByGuidInGraph(Graph, RetainedEntry->NodeGuid));
	// Intrinsic state and internal selected-set edges of the shared node, by an independent oracle.
	TestEqual(TEXT("shared producer intrinsic state preserved"),
		CaptureSelectedNativeNodes(Fixture.Fixture.Blueprint, SharedGuids), SharedBefore);
	TestTrue(TEXT("the shared producer keeps its link to the retained consumer"),
		NodesLinked(Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));
	TestEqual(TEXT("the removed boundary link is gone from the shared producer"),
		CountLinkedPins(Producer, TEXT("ReturnValue")), 1);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 2. DataCycleInsideIsland
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneDataCycleTest,
	"Cortex.Graph.Authoring.Migration.Prune.DataCycleInsideIsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneDataCycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneDataCycle_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneCycleEntry"), 0, 0);
	UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("cycle consumer"), 900, 0);
	UK2Node_CallFunction* Conv = AddPureProducerNode(Graph, 600, 200);
	UK2Node_CallFunction* First = AddIntAddNode(Graph, 300, 200);
	UK2Node_CallFunction* Second = AddIntAddNode(Graph, 300, 400);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	// The data cycle is inside the island: each member feeds the other. The schema accepts the links;
	// a K2 compile would report the cyclic dependency, which is exactly why the traversal must be
	// bounded and structural instead of trusting a compiled graph, and why this case applies with
	// compile=false.
	TestTrue(TEXT("cycle link one"), LinkNodes(Graph, Second, TEXT("ReturnValue"), First, TEXT("A")));
	TestTrue(TEXT("cycle link two"), LinkNodes(Graph, First, TEXT("ReturnValue"), Second, TEXT("A")));
	TestTrue(TEXT("the cycle feeds the island producer"), LinkNodes(Graph, First, TEXT("ReturnValue"), Conv, TEXT("InInt")));
	TestTrue(TEXT("the island producer feeds the print"), LinkNodes(Graph, Conv, TEXT("ReturnValue"), Print, TEXT("InString")));
	TestTrue(TEXT("the entry drives the print"), LinkNodes(Graph, Entry, TEXT("then"), Print, TEXT("execute")));

	const TArray<FGuid> Approved = { Print->NodeGuid, Conv->NodeGuid, First->NodeGuid, Second->NodeGuid };
	// The fixture Blueprint also carries the engine's own default event nodes, which belong to no
	// island of this request, so every count below is relative to the live fixture.
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const int32 GraphNodesBefore = Graph->Nodes.Num();
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131201"),
		Graph, Entry, Approved, true, false);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the cyclic island is planned under the scan budget: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestTrue(TEXT("the bounded cycle traversal completes"), Plan.bComplete);
	TestEqual(TEXT("the whole cyclic island is removable"), Plan.ApprovedGuids, GuidStrings(Approved));
	TestTrue(TEXT("the bounded cycle traversal counts its work"), Plan.ScannedNodes > 0 && Plan.ScannedLinks > 0);

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the cyclic island is pruned: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the cyclic prune readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the cyclic prune requests no compile"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	for (const FGuid& Guid : Approved)
	{
		TestNull(FString::Printf(TEXT("the cycle member %s is gone"), *Guid.ToString()), FindNodeByGuidInGraph(Graph, Guid));
	}
	TestNotNull(TEXT("the entry survives the cyclic prune"), FindNodeByGuidInGraph(Graph, Entry->NodeGuid));
	TestEqual(TEXT("the entry keeps no island link"), CountLinkedPins(Entry, TEXT("then")), 0);
	TestEqual(TEXT("the prune removed exactly the approved island from the graph"), Graph->Nodes.Num(), GraphNodesBefore - 4);
	TestEqual(TEXT("the prune removed exactly the approved island from the asset"), CountNativeNodes(Fixture.Blueprint), NodesBefore - 4);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 3. SecondExternalConsumer
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneSecondExternalConsumerTest,
	"Cortex.Graph.Authoring.Migration.Prune.SecondExternalConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneSecondExternalConsumerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneExternalConsumer_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneExternalEntry"), 0, 0);
	DeclareFixtureMember(Fixture.Blueprint, TEXT("PruneCounter"));
	DeclareFixtureMember(Fixture.Blueprint, TEXT("PruneSink"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	UK2Node_VariableSet* Setter = AddVariableSetNode(Graph, TEXT("PruneCounter"), 300, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Graph, TEXT("island tail"), 600, 0);
	// A node outside the island consumes the setter's data output: the setter's value belongs to
	// another chain as well, so this entry's island does not uniquely own it.
	UK2Node_VariableSet* External = AddVariableSetNode(Graph, TEXT("PruneSink"), 300, 400);
	TestTrue(TEXT("the entry drives the setter"), LinkNodes(Graph, Entry, TEXT("then"), Setter, TEXT("execute")));
	TestTrue(TEXT("the setter drives the tail"), LinkNodes(Graph, Setter, TEXT("then"), Tail, TEXT("execute")));
	TestTrue(TEXT("the external consumer takes the setter's value"),
		LinkNodes(Graph, Setter, TEXT("Output_Get"), External, TEXT("PruneSink")));

	const FFixtureState Before = Observe(Fixture.Blueprint);
	const TArray<FGuid> SharedGuids = { Setter->NodeGuid };
	const FString SharedBefore = CaptureSelectedNativeNodes(Fixture.Blueprint, SharedGuids);

	const TArray<FGuid> Approved = { Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131301"), Graph, Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the island with a second external consumer previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestNotNull(TEXT("the externally consumed node is reported as shared"), FindPartitionNode(Plan.Shared, Setter->NodeGuid));
	TestTrue(TEXT("the externally consumed node is not removable"), !PlanContainsGuid(Plan.ApprovedGuids, Setter->NodeGuid));
	TestTrue(TEXT("the removed island link is an approved boundary edge"),
		PlanHasExternalEdge(Plan, Setter, TEXT("then"), Tail, TEXT("execute")));
	TestFalse(TEXT("the external consumer link is not a boundary edge"),
		PlanHasExternalEdge(Plan, Setter, TEXT("Output_Get"), External, TEXT("PruneSink")));

	// Approving the externally consumed node instead of the removable tail is refused by name.
	FCortexGraphPreparedPatch ExtraPrepared;
	FCortexCommandResult ExtraError;
	const TArray<FGuid> Extra = { Tail->NodeGuid, Setter->NodeGuid };
	TestFalse(TEXT("approving an externally consumed node is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
			PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131302"), Graph, Entry, Extra), ExtraPrepared, ExtraError));
	TestEqual(TEXT("the extra-approval refusal is INVALID_OPERATION"), ExtraError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the externally consumed node [%s]"), *ExtraError.ErrorMessage),
		ExtraError.ErrorMessage.Contains(Setter->NodeGuid.ToString()));
	TestEqual(TEXT("the extra-approval refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the pruned tail applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the pruned tail readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the island tail is gone"), FindNodeByGuidInGraph(Graph, Tail->NodeGuid));
	TestNotNull(TEXT("the externally consumed node survives"), FindNodeByGuidInGraph(Graph, Setter->NodeGuid));
	TestEqual(TEXT("the externally consumed node keeps its intrinsic state"),
		CaptureSelectedNativeNodes(Fixture.Blueprint, SharedGuids), SharedBefore);
	TestTrue(TEXT("the external consumer link survives exactly"),
		NodesLinked(Setter, TEXT("Output_Get"), External, TEXT("PruneSink")));
	TestEqual(TEXT("the removed boundary link is gone from the setter"), CountLinkedPins(Setter, TEXT("then")), 0);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 3b. ProducerChainToRetainedConsumer
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneProducerChainTest,
	"Cortex.Graph.Authoring.Migration.Prune.ProducerChainToRetainedConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneProducerChainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneProducerChain_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneChainEntry"), 0, 0);
	DeclareFixtureMember(Fixture.Blueprint, TEXT("PruneCounter"));
	DeclareFixtureMember(Fixture.Blueprint, TEXT("PruneSink"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	// Producer -> Setter (data) -> External. The setter is retained because the external consumer uses
	// its value, and that retention must reach the producer through the chain: the producer's only
	// consumer is a node the approved set does not remove.
	UK2Node_VariableSet* Setter = AddVariableSetNode(Graph, TEXT("PruneCounter"), 300, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(Graph, TEXT("chain tail"), 700, 0);
	UK2Node_CallFunction* Producer = AddIntAddNode(Graph, 300, 300);
	UK2Node_VariableSet* External = AddVariableSetNode(Graph, TEXT("PruneSink"), 300, 700);
	TestTrue(TEXT("the entry drives the setter"), LinkNodes(Graph, Entry, TEXT("then"), Setter, TEXT("execute")));
	TestTrue(TEXT("the setter drives the tail"), LinkNodes(Graph, Setter, TEXT("then"), Tail, TEXT("execute")));
	TestTrue(TEXT("the producer feeds the setter value"),
		LinkNodes(Graph, Producer, TEXT("ReturnValue"), Setter, TEXT("PruneCounter")));
	TestTrue(TEXT("the setter value feeds the retained consumer"),
		LinkNodes(Graph, Setter, TEXT("Output_Get"), External, TEXT("PruneSink")));

	const FFixtureState Before = Observe(Fixture.Blueprint);

	// The whole chain upstream of a retained consumer is retained, so only the tail is removable.
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131351"),
		Graph, Entry, { Tail->NodeGuid });
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the chained island previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestEqual(TEXT("only the removable tail is removable"), Plan.ApprovedGuids, GuidStrings({ Tail->NodeGuid }));
	TestTrue(TEXT("the retained setter is not removable"), !PlanContainsGuid(Plan.ApprovedGuids, Setter->NodeGuid));
	TestTrue(TEXT("the producer of a retained consumer is not removable"),
		!PlanContainsGuid(Plan.ApprovedGuids, Producer->NodeGuid));
	TestNotNull(TEXT("the setter is reported as retained"), FindPartitionNode(Plan.Shared, Setter->NodeGuid));
	TestNotNull(TEXT("the chained producer is reported as retained"), FindPartitionNode(Plan.Shared, Producer->NodeGuid));

	// Approving the chained producer would disconnect the retained setter, so it is refused by name.
	FCortexGraphPreparedPatch ChainPrepared;
	FCortexCommandResult ChainError;
	const TArray<FGuid> WithProducer = { Tail->NodeGuid, Producer->NodeGuid };
	TestFalse(TEXT("approving the chained producer is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
			PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131352"), Graph, Entry, WithProducer),
			ChainPrepared, ChainError));
	TestEqual(TEXT("the chained-producer refusal is INVALID_OPERATION"), ChainError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the chained producer [%s]"), *ChainError.ErrorMessage),
		ChainError.ErrorMessage.Contains(Producer->NodeGuid.ToString()));
	TestEqual(TEXT("the chained-producer refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);

	// The approved tail alone leaves the retained consumer's whole feeding chain intact.
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the chained island applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the chained prune readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the island tail is gone"), FindNodeByGuidInGraph(Graph, Tail->NodeGuid));
	TestNotNull(TEXT("the retained setter survives"), FindNodeByGuidInGraph(Graph, Setter->NodeGuid));
	TestNotNull(TEXT("the chained producer survives"), FindNodeByGuidInGraph(Graph, Producer->NodeGuid));
	TestTrue(TEXT("the producer still feeds the retained setter"),
		NodesLinked(Producer, TEXT("ReturnValue"), Setter, TEXT("PruneCounter")));
	TestTrue(TEXT("the retained consumer link survives exactly"),
		NodesLinked(Setter, TEXT("Output_Get"), External, TEXT("PruneSink")));
	TestEqual(TEXT("the removed boundary link is gone from the setter"), CountLinkedPins(Setter, TEXT("then")), 0);
	TestEqual(TEXT("only the approved tail was removed"), CountNativeNodes(Fixture.Blueprint), Before.Nodes - 1);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4a. BelowNodeLimitPrunes
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneBelowNodeLimitTest,
	"Cortex.Graph.Authoring.Migration.Prune.BelowNodeLimitPrunes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneBelowNodeLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneBelowLimit_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneBelowLimitEntry"), 0, 0);
	UK2Node_CallFunction* Producer = AddPureProducerNode(Graph, 300, 0);

	// A wide island below the published node bound: the scan limit is a node bound, so this fixture
	// must be plannable and prunable even though its traversal examines far more links than nodes.
	constexpr int32 FanOut = 500;
	TArray<FGuid> Approved;
	Approved.Add(Producer->NodeGuid);
	UK2Node_CallFunction* Previous = nullptr;
	for (int32 Index = 0; Index < FanOut; ++Index)
	{
		UK2Node_CallFunction* Print = AddPrintNode(Graph, TEXT("wide"), 600 + (Index % 20) * 200, 0);
		Approved.Add(Print->NodeGuid);
		if (!LinkNodes(Graph, Producer, TEXT("ReturnValue"), Print, TEXT("InString")))
		{
			TestTrue(TEXT("the fan-out data link is wired"), false);
			Fixture.Cleanup();
			return false;
		}
		if (Previous == nullptr)
		{
			LinkNodes(Graph, Entry, TEXT("then"), Print, TEXT("execute"));
		}
		else
		{
			LinkNodes(Graph, Previous, TEXT("then"), Print, TEXT("execute"));
		}
		Previous = Print;
	}
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	TestTrue(TEXT("the wide fixture stays well below the published node bound"),
		NodesBefore < FCortexGraphPatchOps::MaxScannedNodes);

	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131301"),
		Graph, Entry, Approved, true, false);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the wide island plans under the node bound: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestTrue(TEXT("the wide island partition is complete"), Plan.bComplete);
	TestTrue(TEXT("the wide island scan reports the nodes it examined"),
		Plan.ScannedNodes > 0 && Plan.ScannedNodes <= FCortexGraphPatchOps::MaxScannedNodes);
	TestEqual(TEXT("every uniquely owned island node is removable"), Plan.ApprovedGuids.Num(), FanOut + 1);

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the wide island prunes: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the wide prune readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the wide prune removed exactly the island"), CountNativeNodes(Fixture.Blueprint), NodesBefore - (FanOut + 1));
	TestTrue(TEXT("the wide prune kept the entry"), FindNodeByGuidInGraph(Graph, Entry->NodeGuid) != nullptr);
	TestEqual(TEXT("the wide prune left the entry unlinked"), CountLinkedPins(Entry, TEXT("then")), 0);
	if (Plan.ScannedNodes > 0)
	{
		AddInfo(FString::Printf(TEXT("wide island scan counts: nodes=%d links=%d limit=%d fixture_nodes=%d"),
			Plan.ScannedNodes, Plan.ScannedLinks, FCortexGraphPatchOps::MaxScannedNodes, NodesBefore));
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4b. GraphWideScanLimitRefusedWithCounts
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneScanLimitTest,
	"Cortex.Graph.Authoring.Migration.Prune.GraphWideScanLimitRefusedWithCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneScanLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneScanLimit_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneScanEntry"), 0, 0);

	// A graph above the published node bound: the graph-wide node scan must refuse with the observed
	// count, the limit and completeness, instead of proceeding on an incompletely scanned asset.
	const int32 OverLimit = FCortexGraphPatchOps::MaxScannedNodes + 2;
	for (int32 Index = 0; Index < OverLimit; ++Index)
	{
		AddPrintNode(Graph, TEXT("gate"), 600 + (Index % 40) * 200, (Index / 40) * 60);
	}
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	TestTrue(TEXT("the fixture exceeds the published node bound"), NodesBefore > FCortexGraphPatchOps::MaxScannedNodes);

	const FFixtureState Before = Observe(Fixture.Blueprint);
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("the graph-wide scan limit refuses the request"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
			PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131401"), Graph, Entry, {}, false),
			Prepared, Error));
	TestEqual(TEXT("the scan-limit refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal reports the scan limit [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(FString::Printf(TEXT("%d"), FCortexGraphPatchOps::MaxScannedNodes)));
	TestTrue(TEXT("the refusal carries the observed node count"),
		Error.ErrorDetails.IsValid()
			&& Error.ErrorDetails->GetIntegerField(TEXT("scan_limit")) == FCortexGraphPatchOps::MaxScannedNodes
			&& Error.ErrorDetails->GetIntegerField(TEXT("scanned_nodes")) > FCortexGraphPatchOps::MaxScannedNodes);
	TestTrue(TEXT("the refusal never claims completeness"),
		Error.ErrorDetails.IsValid() && !Error.ErrorDetails->GetBoolField(TEXT("complete")));
	TestTrue(FString::Printf(TEXT("the refusal message reports the observed count [%s]"), *Error.ErrorMessage),
		Error.ErrorDetails.IsValid()
			&& Error.ErrorMessage.Contains(FString::Printf(TEXT("%d"), Error.ErrorDetails->GetIntegerField(TEXT("scanned_nodes")))));
	if (Error.ErrorDetails.IsValid())
	{
		AddInfo(FString::Printf(TEXT("graph-wide node-limit refusal counts: nodes=%d limit=%d fixture_nodes=%d"),
			Error.ErrorDetails->GetIntegerField(TEXT("scanned_nodes")),
			Error.ErrorDetails->GetIntegerField(TEXT("scan_limit")),
			Before.Nodes));
	}
	TestEqual(TEXT("the refused scan removes no node"), CountNativeNodes(Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("the refused scan mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 5. IncompleteApprovedSetRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneApprovedSetMismatchTest,
	"Cortex.Graph.Authoring.Migration.Prune.IncompleteApprovedSetRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneApprovedSetMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneApproval_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneApprovalEntry"), 0, 0);
	UK2Node_CallFunction* First = AddPrintNode(Graph, TEXT("first"), 300, 0);
	UK2Node_CallFunction* Second = AddPrintNode(Graph, TEXT("second"), 600, 0);
	UK2Node_CallFunction* Producer = AddPureProducerNode(Graph, 300, 400);
	UK2Node_CallFunction* RetainedPrint = AddPrintNode(Graph, TEXT("retained"), 600, 400);
	UK2Node_CustomEvent* RetainedEntry = AddEntryNode(Graph, TEXT("CortexPruneApprovalRetained"), 0, 800);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	LinkNodes(Graph, Entry, TEXT("then"), First, TEXT("execute"));
	LinkNodes(Graph, First, TEXT("then"), Second, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), Second, TEXT("InString"));
	LinkNodes(Graph, RetainedEntry, TEXT("then"), RetainedPrint, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString"));

	const FFixtureState Before = Observe(Fixture.Blueprint);
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000131501");
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the complete approval previews: %s"), *Error.ErrorMessage),
		PreviewPrune(Fixture.Blueprint, PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, { First->NodeGuid, Second->NodeGuid }),
			Prepared, Plan, Error));
	TestEqual(TEXT("the whole island tail is removable"), Plan.ApprovedGuids.Num(), 2);

	// An incomplete approval names the removable node it is missing.
	{
		FCortexGraphPreparedPatch PartialPrepared;
		FCortexCommandResult PartialError;
		TestFalse(TEXT("an incomplete approved set is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131502"), Graph, Entry, { First->NodeGuid }),
				PartialPrepared, PartialError));
		TestEqual(TEXT("the incomplete approval refusal is INVALID_OPERATION"),
			PartialError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("the refusal names the missing removable node [%s]"), *PartialError.ErrorMessage),
			PartialError.ErrorMessage.Contains(Second->NodeGuid.ToString()));
		TestEqual(TEXT("the incomplete approval mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);
	}
	// An unknown approved GUID is refused instead of being treated as an absent node.
	{
		FGuid UnknownGuid;
		FGuid::Parse(TEXT("00000000-0000-0000-0000-0000000013FF"), UnknownGuid);
		const TArray<FGuid> WithUnknown = { First->NodeGuid, Second->NodeGuid, UnknownGuid };
		FCortexGraphPreparedPatch UnknownPrepared;
		FCortexCommandResult UnknownError;
		TestFalse(TEXT("an unknown approved GUID is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131503"), Graph, Entry, WithUnknown),
				UnknownPrepared, UnknownError));
		TestTrue(FString::Printf(TEXT("the refusal names the unknown GUID [%s]"), *UnknownError.ErrorMessage),
			UnknownError.ErrorMessage.Contains(UnknownGuid.ToString()));
		TestEqual(TEXT("the unknown approval mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);
	}
	// A shared node is not removable, however the caller justifies it.
	{
		const TArray<FGuid> WithShared = { First->NodeGuid, Second->NodeGuid, Producer->NodeGuid };
		FCortexGraphPreparedPatch SharedPrepared;
		FCortexCommandResult SharedError;
		TestFalse(TEXT("approving a shared node is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131504"), Graph, Entry, WithShared),
				SharedPrepared, SharedError));
		TestTrue(FString::Printf(TEXT("the refusal names the shared node [%s]"), *SharedError.ErrorMessage),
			SharedError.ErrorMessage.Contains(Producer->NodeGuid.ToString()));
		TestEqual(TEXT("the shared approval mutates nothing"), LiveGraphHash(Fixture.Blueprint), Before.Hash);
	}
	// The entry itself is a terminator: it is retained and can never be approved.
	{
		const TArray<FGuid> WithEntry = { First->NodeGuid, Second->NodeGuid, Entry->NodeGuid };
		FCortexGraphPreparedPatch EntryPrepared;
		FCortexCommandResult EntryError;
		TestFalse(TEXT("approving the entry terminator is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131505"), Graph, Entry, WithEntry),
				EntryPrepared, EntryError));
		TestTrue(FString::Printf(TEXT("the refusal names the entry terminator [%s]"), *EntryError.ErrorMessage),
			EntryError.ErrorMessage.Contains(Entry->NodeGuid.ToString()));
	}
	TestEqual(TEXT("every approval refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("every approval refusal opens no transaction"), TransactionCount(), 0);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 6. SharedNodeNeverDeleted
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneSharedNodeNeverDeletedTest,
	"Cortex.Graph.Authoring.Migration.Prune.SharedNodeNeverDeleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneSharedNodeNeverDeletedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FSharedProducerFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Build(TEXT("BP_PruneSharedNever_T13")));
	if (!Fixture.Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* const Graph = Fixture.Graph;
	UK2Node_CallFunction* const Producer = Fixture.Producer;
	UK2Node_CallFunction* const IslandPrint = Fixture.IslandPrint;
	UK2Node_CallFunction* const RetainedPrint = Fixture.RetainedPrint;
	const FFixtureState Before = Observe(Fixture.Fixture.Blueprint);
	const FString SharedBefore = CaptureSelectedNativeNodes(Fixture.Fixture.Blueprint, { Producer->NodeGuid });
	const int32 TransactionsBefore = TransactionCount();

	// Approving only the shared node must never delete it.
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("approving only the shared node is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Fixture.Blueprint,
			PruneRequest(Fixture.Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131601"), Graph, Fixture.Entry, { Producer->NodeGuid }),
			Prepared, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the shared node [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(Producer->NodeGuid.ToString()));
	TestNotNull(TEXT("the shared node still resolves"), FindNodeByGuidInGraph(Graph, Producer->NodeGuid));
	TestTrue(TEXT("the shared node keeps both consumer links"),
		NodesLinked(Producer, TEXT("ReturnValue"), IslandPrint, TEXT("InString"))
			&& NodesLinked(Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));
	TestEqual(TEXT("the refusal opens no transaction"), TransactionCount(), TransactionsBefore);
	TestEqual(TEXT("the refusal mutates no node"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("the refusal mutates no state"), LiveGraphHash(Fixture.Fixture.Blueprint), Before.Hash);

	// The correct prune keeps the shared node and its retained-consumer link.
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131602"),
		Graph, Fixture.Entry, { IslandPrint->NodeGuid });
	FCortexGraphPreparedPatch ApplyPrepared;
	FCortexGraphMigrationPrunePlan Plan;
	TestTrue(FString::Printf(TEXT("the correct prune prepares: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Request, ApplyPrepared, Plan, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the correct prune applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Request, Outcome, Error));
	TestNotNull(TEXT("the shared node survived"), FindNodeByGuidInGraph(Graph, Producer->NodeGuid));
	TestEqual(TEXT("the shared producer keeps its intrinsic state"),
		CaptureSelectedNativeNodes(Fixture.Fixture.Blueprint, { Producer->NodeGuid }), SharedBefore);
	TestTrue(TEXT("the shared producer keeps its link to the retained consumer"),
		NodesLinked(Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));
	TestEqual(TEXT("only the approved island node was removed"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes - 1);
	TestNull(TEXT("the approved island node is gone"), FindNodeByGuidInGraph(Graph, IslandPrint->NodeGuid));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 7. RetainsConsumerLinksRemovesBoundaryEdges
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneRetainedLinksTest,
	"Cortex.Graph.Authoring.Migration.Prune.RetainsConsumerLinksRemovesBoundaryEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneRetainedLinksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneRetainedLinks_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneLinksEntry"), 0, 0);
	UK2Node_CallFunction* Producer = AddPureProducerNode(Graph, 300, 200);
	UK2Node_CallFunction* First = AddPrintNode(Graph, TEXT("first"), 600, 0);
	UK2Node_CallFunction* Second = AddPrintNode(Graph, TEXT("second"), 900, 0);
	UK2Node_CustomEvent* RetainedEntry = AddEntryNode(Graph, TEXT("CortexPruneLinksRetained"), 0, 900);
	UK2Node_CallFunction* RetainedPrint = AddPrintNode(Graph, TEXT("retained"), 600, 900);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	LinkNodes(Graph, Entry, TEXT("then"), First, TEXT("execute"));
	LinkNodes(Graph, First, TEXT("then"), Second, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), First, TEXT("InString"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), Second, TEXT("InString"));
	LinkNodes(Graph, RetainedEntry, TEXT("then"), RetainedPrint, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString"));

	const FFixtureState Before = Observe(Fixture.Blueprint);
	const TArray<FGuid> Approved = { First->NodeGuid, Second->NodeGuid };
	TSet<FGuid> Retained;
	Retained.Add(Entry->NodeGuid);
	Retained.Add(Producer->NodeGuid);
	Retained.Add(RetainedEntry->NodeGuid);
	Retained.Add(RetainedPrint->NodeGuid);
	const TArray<FString> RetainedLinksBefore = CollectLinkSet(Graph, Retained);
	TestEqual(TEXT("the retained body starts with its consumer links"), RetainedLinksBefore.Num(), 4);

	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-000000131701"), Graph, Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the island chain previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestTrue(TEXT("the entry-to-island link is an approved boundary edge"),
		PlanHasExternalEdge(Plan, Entry, TEXT("then"), First, TEXT("execute")));
	TestTrue(TEXT("the producer-to-island link is an approved boundary edge"),
		PlanHasExternalEdge(Plan, Producer, TEXT("ReturnValue"), First, TEXT("InString")));
	TestFalse(TEXT("the internal island link is not a boundary edge"),
		PlanHasExternalEdge(Plan, First, TEXT("then"), Second, TEXT("execute")));
	TestFalse(TEXT("the retained-consumer link is not a boundary edge"),
		PlanHasExternalEdge(Plan, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the island chain applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the readback matches"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("exactly the two approved nodes were removed"), CountNativeNodes(Fixture.Blueprint), Before.Nodes - 2);

	// Every approved boundary edge is gone from both of its endpoints.
	for (const FCortexGraphPruneEdge& Edge : Plan.ExternalEdges)
	{
		FGuid FromGuid;
		FGuid ToGuid;
		FGuid::Parse(Edge.FromGuid, FromGuid);
		FGuid::Parse(Edge.ToGuid, ToGuid);
		UEdGraphNode* From = FindNodeByGuidInGraph(Graph, FromGuid);
		UEdGraphNode* To = FindNodeByGuidInGraph(Graph, ToGuid);
		TestFalse(FString::Printf(TEXT("the approved boundary edge %s.%s -> %s.%s is gone"),
			*Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin),
			From != nullptr && To != nullptr && NodesLinked(From, *Edge.FromPin, To, *Edge.ToPin));
	}

	// Every retained-consumer link survives exactly.
	TestEqual(TEXT("every retained-consumer link survives exactly"), CollectLinkSet(Graph, Retained), RetainedLinksBefore);
	TestTrue(TEXT("the retained producer keeps exactly one consumer link"),
		CountLinkedPins(Producer, TEXT("ReturnValue")) == 1
			&& NodesLinked(Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString")));
	TestEqual(TEXT("the entry keeps no island link"), CountLinkedPins(Entry, TEXT("then")), 0);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 8. FailureAfterDeletionRestores
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneFailureAfterDeletionTest,
	"Cortex.Graph.Authoring.Migration.Prune.FailureAfterDeletionRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneFailureAfterDeletionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FSharedProducerFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Build(TEXT("BP_PruneFaultDeletion_T13")));
	if (!Fixture.Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* const Graph = Fixture.Graph;
	const FFixtureState Before = Observe(Fixture.Fixture.Blueprint);
	const FString GraphBefore = CaptureGraphNative(Fixture.Fixture.Blueprint, Graph);
	const int32 TransactionsBefore = TransactionCount();

	const TArray<FGuid> Approved = { Fixture.IslandPrint->NodeGuid };
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000131801");
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Fixture.Blueprint, PatchId, Graph, Fixture.Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the faulted prune previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Request, Prepared, Plan, Error));

	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("migration_prune_removed"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("the injected failure after deletion fails the apply"),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	TestEqual(TEXT("the injected failure reports a restore"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("the injected failure does not block"), Outcome.bBlocked);
	TestEqual(TEXT("the graph is restored exactly [%s]"),
		FirstCaptureDifference(GraphBefore, CaptureGraphNative(Fixture.Fixture.Blueprint, Graph)), FString());
	TestEqual(TEXT("the deletion failure restores the fingerprint"), LiveGraphHash(Fixture.Fixture.Blueprint), Before.Hash);
	TestEqual(TEXT("the deletion failure restores the node count"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("the deletion failure restores the dirty flag"), Fixture.Fixture.Package->IsDirty(), Before.bDirty);
	TestEqual(TEXT("the deletion failure leaves no transaction"), TransactionCount(), TransactionsBefore);
	TestNotNull(TEXT("the approved node is back"), FindNodeByGuidInGraph(Graph, Fixture.IslandPrint->NodeGuid));
	TestTrue(TEXT("the restored node keeps its entry link"),
		NodesLinked(Fixture.Entry, TEXT("then"), Fixture.IslandPrint, TEXT("execute")));
	TestTrue(TEXT("the restored link to the retained body is back"),
		NodesLinked(Fixture.Producer, TEXT("ReturnValue"), Fixture.IslandPrint, TEXT("InString")));

	// Recovery leaves no residue: the retry applies cleanly.
	TSharedPtr<FJsonObject> Retry = PruneRequest(Fixture.Fixture.Blueprint, PatchId, Graph, Fixture.Entry, Approved);
	FCortexGraphPreparedPatch RetryPrepared;
	FCortexGraphMigrationPrunePlan RetryPlan;
	FCortexCommandResult RetryError;
	TestTrue(FString::Printf(TEXT("the retry previews: %s"), *RetryError.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Retry, RetryPrepared, RetryPlan, RetryError));
	FCortexGraphPatchOutcome RetryOutcome;
	TestTrue(FString::Printf(TEXT("the retry applies: %s [%s]"), *RetryError.ErrorMessage, *FString::Join(RetryOutcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Retry, RetryOutcome, RetryError));
	TestEqual(TEXT("the retry readback matches"), RetryOutcome.ReadbackStatus, FString(TEXT("matched")));
	TestNull(TEXT("the retry removed the approved node"), FindNodeByGuidInGraph(Graph, Fixture.IslandPrint->NodeGuid));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 9. FailureInReadbackRestores
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneReadbackFailureTest,
	"Cortex.Graph.Authoring.Migration.Prune.FailureInReadbackRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneReadbackFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FSharedProducerFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Build(TEXT("BP_PruneFaultReadback_T13")));
	if (!Fixture.Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* const Graph = Fixture.Graph;
	const FFixtureState Before = Observe(Fixture.Fixture.Blueprint);
	const FString GraphBefore = CaptureGraphNative(Fixture.Fixture.Blueprint, Graph);

	const TArray<FGuid> Approved = { Fixture.IslandPrint->NodeGuid };
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000131901");
	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Fixture.Blueprint, PatchId, Graph, Fixture.Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the readback-faulted prune previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Request, Prepared, Plan, Error));

	FCortexGraphMigrationOps::SetPruneReadbackFaultForTesting(TEXT("prune_after_removal"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("the injected readback failure fails the apply"),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Request, Outcome, Error));
	FCortexGraphMigrationOps::ClearPruneReadbackFaultForTesting();
	TestEqual(TEXT("the readback failure reports mismatched"), Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	TestEqual(TEXT("the readback failure reports a restore"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("the readback failure does not block"), Outcome.bBlocked);
	TestEqual(TEXT("the readback failure restores the graph exactly [%s]"),
		FirstCaptureDifference(GraphBefore, CaptureGraphNative(Fixture.Fixture.Blueprint, Graph)), FString());
	TestEqual(TEXT("the readback failure restores the fingerprint"), LiveGraphHash(Fixture.Fixture.Blueprint), Before.Hash);
	TestEqual(TEXT("the readback failure restores the node count"), CountNativeNodes(Fixture.Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("the readback failure restores the dirty flag"), Fixture.Fixture.Package->IsDirty(), Before.bDirty);
	TestNotNull(TEXT("the approved node is back after the readback failure"), FindNodeByGuidInGraph(Graph, Fixture.IslandPrint->NodeGuid));
	TestTrue(TEXT("the restored node keeps its links"),
		NodesLinked(Fixture.Entry, TEXT("then"), Fixture.IslandPrint, TEXT("execute"))
			&& NodesLinked(Fixture.Producer, TEXT("ReturnValue"), Fixture.IslandPrint, TEXT("InString")));

	// Recovery leaves no residue: the retry applies cleanly.
	TSharedPtr<FJsonObject> Retry = PruneRequest(Fixture.Fixture.Blueprint, PatchId, Graph, Fixture.Entry, Approved);
	FCortexGraphPreparedPatch RetryPrepared;
	FCortexGraphMigrationPrunePlan RetryPlan;
	FCortexCommandResult RetryError;
	TestTrue(FString::Printf(TEXT("the retry previews: %s"), *RetryError.ErrorMessage),
		PrepareApply(Fixture.Fixture.Blueprint, Retry, RetryPrepared, RetryPlan, RetryError));
	FCortexGraphPatchOutcome RetryOutcome;
	TestTrue(FString::Printf(TEXT("the retry applies: %s [%s]"), *RetryError.ErrorMessage, *FString::Join(RetryOutcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Fixture.Blueprint, Retry, RetryOutcome, RetryError));
	TestEqual(TEXT("the retry readback matches"), RetryOutcome.ReadbackStatus, FString(TEXT("matched")));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 10. RepeatedRequestIsUnchanged
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneRepeatedRequestTest,
	"Cortex.Graph.Authoring.Migration.Prune.RepeatedRequestIsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneRepeatedRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneRepeated_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneRepeatedEntry"), 0, 0);
	UK2Node_CallFunction* IslandPrint = AddPrintNode(Graph, TEXT("island"), 600, 0);
	UK2Node_CallFunction* StayBehind = AddPrintNode(Graph, TEXT("stay behind"), 900, 0);
	UK2Node_CallFunction* Producer = AddPureProducerNode(Graph, 300, 200);
	UK2Node_CallFunction* RetainedPrint = AddPrintNode(Graph, TEXT("retained"), 600, 400);
	UK2Node_CustomEvent* RetainedEntry = AddEntryNode(Graph, TEXT("CortexPruneRepeatedRetained"), 0, 800);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	LinkNodes(Graph, Entry, TEXT("then"), IslandPrint, TEXT("execute"));
	LinkNodes(Graph, RetainedEntry, TEXT("then"), RetainedPrint, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), IslandPrint, TEXT("InString"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString"));

	const TArray<FGuid> Approved = { IslandPrint->NodeGuid };
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000132001");

	TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, Approved);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("the prune previews: %s"), *Error.ErrorMessage),
		PrepareApply(Fixture.Blueprint, Request, Prepared, Plan, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("the prune applies: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the first request applies"), Outcome.ApplyStatus, FString(TEXT("applied")));

	const int32 NodesAfter = CountNativeNodes(Fixture.Blueprint);
	const FString GraphAfter = CaptureGraphNative(Fixture.Blueprint, Graph);
	const int32 TransactionsAfter = TransactionCount();
	const FFixtureState AfterState = Observe(Fixture.Blueprint);
	const FFixtureState UnchangedNodes = { AfterState.Hash, NodesAfter, AfterState.bDirty };

	// The identical request again: the approved set is already absent, so it is an idempotent replay.
	TSharedPtr<FJsonObject> Replay = PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, Approved);
	FCortexGraphPreparedPatch ReplayPrepared;
	FCortexGraphMigrationPrunePlan ReplayPlan;
	FCortexCommandResult ReplayError;
	TestTrue(FString::Printf(TEXT("the repeated request previews: %s"), *ReplayError.ErrorMessage),
		PreviewPrune(Fixture.Blueprint, Replay, ReplayPrepared, ReplayPlan, ReplayError));
	TestFalse(TEXT("the repeated request reports no prospective change"), ReplayPrepared.bChanged);
	TestTrue(TEXT("the repeated request reconciles through the reuse route"), ReplayPlan.bReused);
	Replay->SetBoolField(TEXT("dry_run"), false);
	Replay->SetStringField(TEXT("expected_validation_hash"), ReplayPrepared.ValidationHash);
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("the repeated request reports unchanged: %s"), *ReplayError.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, ReplayError));
	Operations.End();
	TestEqual(TEXT("the repeated request is unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("the repeated request never compiles"), ReplayOutcome.TargetCompileCount, 0);
	TestEqual(TEXT("the repeated request opens no transaction"), TransactionCount(), TransactionsAfter);
	TestEqual(TEXT("the repeated request adds no node"), CountNativeNodes(Fixture.Blueprint), UnchangedNodes.Nodes);
	TestEqual(TEXT("the repeated request leaves the graph"), CaptureGraphNative(Fixture.Blueprint, Graph), GraphAfter);
	TestEqual(TEXT("the repeated request leaves the fingerprint"), LiveGraphHash(Fixture.Blueprint), UnchangedNodes.Hash);
	TestEqual(TEXT("the repeated request leaves the dirty state"), Fixture.Package->IsDirty(), UnchangedNodes.bDirty);

	// A partially removed approved set - one approved node present again - refuses naming what is missing.
	UK2Node_CallFunction* Reintroduced = AddPrintNode(Graph, TEXT("reintroduced"), 1200, 0);
	const TArray<FGuid> Partial = { IslandPrint->NodeGuid, StayBehind->NodeGuid, Reintroduced->NodeGuid };
	FCortexGraphPreparedPatch PartialPrepared;
	FCortexCommandResult PartialError;
	TestFalse(TEXT("a partially removed approved set is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, Partial),
			PartialPrepared, PartialError));
	TestEqual(TEXT("the partial-state refusal is INVALID_OPERATION"),
		PartialError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the partial-state refusal names the missing node [%s]"), *PartialError.ErrorMessage),
		PartialError.ErrorMessage.Contains(IslandPrint->NodeGuid.ToString()));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 11. EnvelopeRefusals
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationPruneEnvelopeTest,
	"Cortex.Graph.Authoring.Migration.Prune.EnvelopeRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationPruneEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationPruneTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PruneEnvelope_T13")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CustomEvent* Entry = AddEntryNode(Graph, TEXT("CortexPruneEnvelopeEntry"), 0, 0);
	UK2Node_CallFunction* IslandPrint = AddPrintNode(Graph, TEXT("island"), 600, 0);
	UK2Node_CallFunction* Producer = AddPureProducerNode(Graph, 300, 200);
	UK2Node_CallFunction* RetainedPrint = AddPrintNode(Graph, TEXT("retained"), 600, 400);
	UK2Node_CustomEvent* RetainedEntry = AddEntryNode(Graph, TEXT("CortexPruneEnvelopeRetained"), 0, 800);
	UEdGraph* OtherGraph = FBlueprintEditorUtils::CreateNewGraph(Fixture.Blueprint, TEXT("CortexPruneEnvelopeFunction"),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Fixture.Blueprint, OtherGraph, true, nullptr);
	LinkNodes(Graph, Entry, TEXT("then"), IslandPrint, TEXT("execute"));
	LinkNodes(Graph, RetainedEntry, TEXT("then"), RetainedPrint, TEXT("execute"));
	LinkNodes(Graph, Producer, TEXT("ReturnValue"), RetainedPrint, TEXT("InString"));
	// The entry outside the named graph is authored before the snapshot, so every refusal below is
	// compared against a fixture that is already complete.
	UK2Node_CallFunction* Outside = AddCallFunctionNode(OtherGraph, FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass(), 0, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const FFixtureState Before = Observe(Fixture.Blueprint);
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000132101");

	// An explicitly empty approved set is refused: a prune that removes nothing is not a prune.
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("an empty approved set is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, {}), Prepared, Error));
		TestEqual(TEXT("the empty approval refusal is INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	// A duplicated approved GUID is refused instead of silently de-duplicated.
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		const TArray<FGuid> Duplicated = { IslandPrint->NodeGuid, IslandPrint->NodeGuid };
		TestFalse(TEXT("a duplicated approved GUID is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, Duplicated), Prepared, Error));
		TestEqual(TEXT("the duplicate refusal is INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	// An unknown field inside the migration object is refused.
	{
		TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, { IslandPrint->NodeGuid });
		Request->GetObjectField(TEXT("migration"))->SetStringField(TEXT("unknown_field"), TEXT("x"));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("an unknown migration field is refused"), FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("the unknown-field refusal is INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	// An unknown field inside the source object is refused.
	{
		TSharedPtr<FJsonObject> Request = PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, { IslandPrint->NodeGuid });
		Request->GetObjectField(TEXT("migration"))->GetObjectField(TEXT("source"))->SetStringField(TEXT("unknown_field"), TEXT("x"));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("an unknown source field is refused"), FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("the unknown-source refusal is INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	// The entry must be a terminator: any other node is refused as the entry.
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("a non-entry node is refused as the entry"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, PatchId, Graph, IslandPrint, { IslandPrint->NodeGuid }), Prepared, Error));
		TestEqual(TEXT("the non-entry refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	}
	// An entry that is not a node of the named graph is refused by name.
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("an entry outside the named graph is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, PatchId, Graph, Outside, { IslandPrint->NodeGuid }), Prepared, Error));
		TestTrue(FString::Printf(TEXT("the refusal names the entry outside the graph [%s]"), *Error.ErrorMessage),
			Error.ErrorMessage.Contains(Outside->NodeGuid.ToString()));
	}
	// An entry GUID that exists nowhere is refused by name.
	{
		FGuid MissingEntryGuid;
		FGuid::Parse(TEXT("00000000-0000-0000-0000-0000000013AB"), MissingEntryGuid);
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("a missing entry GUID is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequestForEntryGuid(Fixture.Blueprint, PatchId, Graph, MissingEntryGuid.ToString(), { IslandPrint->NodeGuid }), Prepared, Error));
		TestTrue(FString::Printf(TEXT("the refusal names the missing entry GUID [%s]"), *Error.ErrorMessage),
			Error.ErrorMessage.Contains(MissingEntryGuid.ToString()));
	}
	// An apply without the approved set of a preview is refused instead of reporting unchanged.
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestTrue(TEXT("the awaiting-approval preview succeeds"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint,
				PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, {}, false), Prepared, Error));
		TSharedPtr<FJsonObject> Apply = PruneRequest(Fixture.Blueprint, PatchId, Graph, Entry, {}, false);
		Apply->SetBoolField(TEXT("dry_run"), false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Prepared.ValidationHash);
		FCortexGraphPreparedPatch ApplyPrepared;
		FCortexCommandResult ApplyError;
		TestFalse(TEXT("an apply without an approved set is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Apply, ApplyPrepared, ApplyError));
		TestEqual(TEXT("the approval-less apply refusal is INVALID_FIELD"), ApplyError.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	TestEqual(TEXT("every envelope refusal mutates no state"), LiveGraphHash(Fixture.Blueprint), Before.Hash);
	TestEqual(TEXT("every envelope refusal mutates no node"), CountNativeNodes(Fixture.Blueprint), Before.Nodes);
	TestEqual(TEXT("every envelope refusal opens no transaction"), TransactionCount(), 0);

	Fixture.Cleanup();
	return true;
}
#endif
