#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphMigrationTestTypes.h"
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
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * `migration.op="copy_subgraph"|"move_subgraph"` coverage.
 *
 * Every case drives the real native entry points (`FCortexGraphPatchOps::Preflight` / `Execute`), so
 * the envelope, the durable plan, the phase shell, the journal and the native readback are
 * exercised exactly as a connected editor exercises them. The preservation oracles below are
 * independent test-side captures of both graphs: a transfer is only accepted when the destination
 * carries the planned authored state and both graphs are proven afterwards.
 */
namespace CortexGraphMigrationTransferTest
{
const TCHAR* const CopyOp = TEXT("copy_subgraph");
const TCHAR* const MoveOp = TEXT("move_subgraph");

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
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphMigrationTransferTestCleanup")));
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

bool DeleteFixtureFile(const FString& Filename)
{
	if (Filename.IsEmpty()) return true;
	return IFileManager::Get().Delete(*Filename, false, true, true);
}

UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	return FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, NodeGuid);
}

UEdGraphNode* FindNodeByGuidInGraph(UEdGraph* Graph, const FGuid& NodeGuid)
{
	return FCortexGraphMigrationOps::FindNodeByGuidInGraph(Graph, NodeGuid);
}

UEdGraph* FindGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid)
{
	return FCortexGraphMigrationOps::FindGraphByGuid(Blueprint, GraphGuid);
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

/** How many nodes carry one GUID anywhere in the asset; the asset-wide uniqueness oracle. */
int32 CountNodesWithGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	int32 Count = 0;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) ++Count;
		}
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
	return Source && Target && Source->LinkedTo.Contains(Target);
}

/**
 * Independent test-side canonical capture of one graph: every node with its class, layout, comment,
 * bubble and enabled state, the authored symbol of the node kinds the fixtures use, and every pin
 * with its type, defaults and the links whose far endpoint is inside the same graph. Recovery is
 * proven against this oracle instead of against the production capture.
 */
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
		FString Capture = FString::Printf(TEXT("guid=%s name=%s class=%s pos=(%d,%d) size=(%d,%d) comment=\"%s\" bubble=%d/%d enabled=%d/%d/%d"),
			*Node->NodeGuid.ToString(), *Node->GetName(), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY,
			Node->NodeWidth, Node->NodeHeight,
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
		else if (const UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
		{
			Capture += FString::Printf(TEXT(" var=%s local=%d"),
				*VarGet->VariableReference.GetMemberName().ToString(),
				VarGet->VariableReference.IsLocalScope() ? 1 : 0);
		}
		else if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			Capture += FString::Printf(TEXT(" entryPins=%d"), Entry->Pins.Num());
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
			Pins.Add(FString::Printf(TEXT("%s|dir=%d|cat=%s|sub=%s|subobj=%s|container=%d|def=%s|defobj=%s|links=[%s]"),
				*Pin->PinName.ToString(),
				static_cast<int32>(Pin->Direction),
				*Pin->PinType.PinCategory.ToString(),
				*Pin->PinType.PinSubCategory.ToString(),
				Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("none"),
				static_cast<int32>(Pin->PinType.ContainerType),
				*Pin->DefaultValue,
				Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT("none"),
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

/** Fixture Blueprint in a transient package plus the file it is persisted to. */
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	FString Filename;

	bool Create(const TCHAR* Name, UClass* ParentClass = nullptr)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass ? ParentClass : ACortexGraphMigrationFixtureActor::StaticClass(),
			Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint) return false;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		// A stale file from an interrupted run makes the engine refuse to save a freshly created
		// package over it, so every fixture starts from its own clean on-disk state.
		DeleteFixtureFile(Filename);
		return true;
	}

	/** Establishes the clean starting package that really matches the file on disk. */
	bool SaveToDisk()
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs);
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
		DeleteFixtureFile(Filename);
	}
};


/** A function graph that implements the fixture declaration, so it owns a result terminator. */
struct FTerminatorFunctionGraph
{
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
};

FTerminatorFunctionGraph AddComputeScoreFunctionGraph(UBlueprint* Blueprint, const TCHAR* Name)
{
	FTerminatorFunctionGraph Built;
	UClass* const OwnerClass = ACortexGraphMigrationFixtureActor::StaticClass();
	Built.Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	Blueprint->FunctionGraphs.Add(Built.Graph);

	Built.Entry = NewObject<UK2Node_FunctionEntry>(Built.Graph);
	Built.Entry->FunctionReference.SetExternalMember(FName(TEXT("ComputeScore")), OwnerClass);
	Built.Entry->CreateNewGuid();
	Built.Entry->AllocateDefaultPins();
	Built.Entry->NodePosX = -400;
	Built.Entry->NodePosY = 0;
	Built.Graph->AddNode(Built.Entry, true, false);

	Built.Result = NewObject<UK2Node_FunctionResult>(Built.Graph);
	Built.Result->FunctionReference = Built.Entry->FunctionReference;
	Built.Result->CreateNewGuid();
	Built.Result->AllocateDefaultPins();
	Built.Result->NodePosX = 700;
	Built.Result->NodePosY = 0;
	Built.Graph->AddNode(Built.Result, true, false);
	return Built;
}

/** A destination graph with one free int32 input pin for a boundary mapping. */
UK2Node_CallFunction* AddIntMultiplyNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("Multiply_IntInt")), UKismetMathLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
{
	if (Blueprint->UbergraphPages.Num() > 0) return Blueprint->UbergraphPages[0];
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	return Graph;
}

/** A fresh user function graph with an entry terminator only. */
UEdGraph* AddVoidFunctionGraph(UBlueprint* Blueprint, const TCHAR* Name)
{
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
	return Graph;
}

UK2Node_FunctionEntry* FindEntryNode(UEdGraph* Graph)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node)) return Entry;
	}
	return nullptr;
}


UK2Node_CallFunction* AddMakeVectorNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("MakeVector")), UKismetMathLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

/** Hand-authors the struct-expanded state of one struct output pin and returns the child X pin. */
UEdGraphPin* SplitStructPinChildren(UEdGraphNode* Node, const TCHAR* ParentPinName)
{
	UEdGraphPin* Parent = Node ? Node->FindPin(FName(ParentPinName)) : nullptr;
	if (!Parent) return nullptr;
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	const TCHAR* ChildNames[] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
	UEdGraphPin* First = nullptr;
	for (const TCHAR* ChildName : ChildNames)
	{
		UEdGraphPin* Child = Node->CreatePin(EGPD_Output, FloatType, FName(ChildName));
		if (!Child) continue;
		Child->ParentPin = Parent;
		Parent->SubPins.Add(Child);
		if (!First) First = Child;
	}
	return First;
}

/** A destination graph with one free float input pin for a split-child boundary mapping. */
UK2Node_CallFunction* AddFloatMultiplyNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("Multiply_FloatFloat")), UKismetMathLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

UK2Node_CallFunction* AddPrintNode(UEdGraph* Graph, const TCHAR* Text, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Call->NodeComment = FString::Printf(TEXT("print %s"), Text);
	if (UEdGraphPin* InString = Call->FindPin(TEXT("InString")))
	{
		InString->DefaultValue = Text;
	}
	Graph->AddNode(Call, true, false);
	return Call;
}

/** A pure producer of the fixtures: `Conv_IntToString`, so shared pure data has a real symbol. */
UK2Node_CallFunction* AddPureIntToStringNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("Conv_IntToString")), UKismetStringLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	if (UEdGraphPin* InInt = Call->FindPin(TEXT("InInt")))
	{
		InInt->DefaultValue = TEXT("3");
	}
	Graph->AddNode(Call, true, false);
	return Call;
}

UK2Node_CallFunction* AddIntAddNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("Add_IntInt")), UKismetMathLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

/** A self-context call to the fixture interface declaration. */
UK2Node_CallFunction* AddInterfaceCallNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetSelfMember(FName(TEXT("OnInterfacePing")));
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

/** A local-scope variable read of one declared local variable. */
UK2Node_VariableGet* AddLocalVariableGetNode(UBlueprint* Blueprint, UEdGraph* Graph, const TCHAR* Name, const int32 X, const int32 Y)
{
	UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
	Get->VariableReference.SetLocalMember(FName(Name), Graph->GetName(),
		FBlueprintEditorUtils::FindLocalVariableGuidByName(Blueprint, Graph, FName(Name)));
	Get->CreateNewGuid();
	Get->AllocateDefaultPins();
	Get->NodePosX = X;
	Get->NodePosY = Y;
	Graph->AddNode(Get, true, false);
	return Get;
}

bool DeclareLocalInt(UBlueprint* Blueprint, UEdGraph* Graph, const TCHAR* Name)
{
	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	return FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, FName(Name), IntType, TEXT("0"))
		|| FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, FName(Name)) != nullptr;
}

bool LinkPins(UEdGraph* Graph, UEdGraphPin* From, UEdGraphPin* To)
{
	const UEdGraphSchema* Schema = Graph->GetSchema();
	return From && To && Schema && Schema->TryCreateConnection(From, To);
}

bool LinkNodes(UEdGraph* Graph, UEdGraphNode* From, const TCHAR* FromPin, UEdGraphNode* To, const TCHAR* ToPin)
{
	return LinkPins(Graph,
		From ? From->FindPin(FName(FromPin)) : nullptr,
		To ? To->FindPin(FName(ToPin)) : nullptr);
}

/** The canonical patch-id string the envelope resolves a `patch_id` field to. */
FString CanonicalPatchId(const TCHAR* PatchId)
{
	FGuid Parsed;
	FGuid::Parse(PatchId, Parsed);
	return Parsed.ToString(EGuidFormats::DigitsWithHyphensInBraces);
}

/** The identity a `copy_subgraph` derives for one selected source node. */
FGuid DerivedCopyGuid(const TCHAR* PatchId, const FGuid& SourceGuid)
{
	return FCortexGraphPatchOps::DeriveNodeGuid(CanonicalPatchId(PatchId), SourceGuid.ToString());
}

// ---------------------------------------------------------------------------
// Request builders
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> GraphRefJson(UEdGraph* Graph)
{
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
	return GraphRef;
}

TSharedPtr<FJsonObject> BoundaryEntry(UEdGraphNode* FromNode, const TCHAR* FromPin, UEdGraphNode* ToNode, const TCHAR* ToPin)
{
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node_guid"), FromNode->NodeGuid.ToString());
	From->SetStringField(TEXT("pin"), FromPin);
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node_guid"), ToNode->NodeGuid.ToString());
	To->SetStringField(TEXT("pin"), ToPin);
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetObjectField(TEXT("from"), From);
	Entry->SetObjectField(TEXT("to"), To);
	return Entry;
}

TSharedPtr<FJsonObject> TransferRequest(
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	const TCHAR* Op,
	UEdGraph* SourceGraph,
	const TArray<FGuid>& Selection,
	UEdGraph* DestinationGraph,
	const TArray<TSharedPtr<FJsonValue>>& Boundary,
	const bool bSave = false,
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
	Migration->SetStringField(TEXT("op"), Op);
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetObjectField(TEXT("graph_ref"), GraphRefJson(SourceGraph));
	TArray<TSharedPtr<FJsonValue>> GuidValues;
	for (const FGuid& Guid : Selection)
	{
		GuidValues.Add(MakeShared<FJsonValueString>(Guid.ToString()));
	}
	Source->SetArrayField(TEXT("node_guids"), GuidValues);
	Migration->SetObjectField(TEXT("source"), Source);
	TSharedPtr<FJsonObject> Destination = MakeShared<FJsonObject>();
	Destination->SetObjectField(TEXT("graph_ref"), GraphRefJson(DestinationGraph));
	Migration->SetObjectField(TEXT("destination"), Destination);
	Migration->SetArrayField(TEXT("boundary"), Boundary);
	Request->SetObjectField(TEXT("migration"), Migration);

	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), bCompile);
	Request->SetBoolField(TEXT("save"), bSave);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

bool PreviewPlan(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexGraphPreparedPatch& OutPrepared,
	FCortexGraphMigrationTransferPlan& OutPlan,
	FCortexCommandResult& OutError)
{
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, OutPrepared, OutError)) return false;
	if (!OutPrepared.TransferPlan.IsValid()) return false;
	FCortexCommandResult PlanError;
	if (!FCortexGraphMigrationTransferPlan::FromJson(OutPrepared.TransferPlan, OutPlan, PlanError))
	{
		OutError = PlanError;
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), OutPrepared.ValidationHash);
	return true;
}

const FCortexGraphTransferDependency* FindDependency(
	const FCortexGraphMigrationTransferPlan& Plan,
	const FGuid& NodeGuid,
	const TCHAR* Kind)
{
	return Plan.Dependencies.FindByPredicate(
		[&NodeGuid, Kind](const FCortexGraphTransferDependency& Candidate)
		{
			return Candidate.NodeGuid == NodeGuid.ToString() && Candidate.Kind == Kind;
		});
}

FString JoinDiagnostics(const TArray<FString>& Diagnostics)
{
	return FString::Join(Diagnostics, TEXT("; "));
}
}

// ---------------------------------------------------------------------------
// 1. CopyInternalChain
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferCopyInternalChainTest,
	"Cortex.Graph.Authoring.Migration.Transfer.CopyInternalChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferCopyInternalChainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferCopyChain_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferCopyTarget"));
	TestNotNull(TEXT("destination function graph created"), DestinationGraph);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	const FString SourceHashBefore = LiveGraphHash(Fixture.Blueprint);
	const FString SourceCaptureBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120101"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("copy preview succeeds: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestTrue(TEXT("copy preview reports a prospective change"), Prepared.bChanged);
	TestFalse(TEXT("copy preview is not a replay"), Plan.bReused);
	TestEqual(TEXT("copy preview publishes the identity map"), Prepared.NodeGuidByClientId.Num(), 2);
	TestEqual(TEXT("copy preview publishes both internal edges as planned"), Plan.InternalEdges.Num(), 1);
	TestEqual(TEXT("copy preview needs no boundary entry for an internal chain"), Plan.Boundary.Num(), 0);
	TestEqual(TEXT("copy preview reports no removal set"), Plan.RemovalSet.Num(), 0);
	TestEqual(TEXT("copy preview publishes both graph preservation contracts"),
		Plan.Preservations.Num(), 2);
	const FGuid HeadCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000120101"), Head->NodeGuid);
	const FGuid TailCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000120101"), Tail->NodeGuid);
	TestTrue(TEXT("copy preview derives a fresh identity for the chain head"), HeadCopy != Head->NodeGuid);
	TestTrue(TEXT("copy preview publishes the derived head identity"),
		Prepared.NodeGuidByClientId.Contains(Head->NodeGuid.ToString())
		&& Prepared.NodeGuidByClientId[Head->NodeGuid.ToString()] == HeadCopy);

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("copy applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("copy reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("copy compiles once"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("copy target compile count"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("copy recovery compile count"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("copy readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("copy never blocks"), Outcome.bBlocked, false);
	TestEqual(TEXT("copy saves nothing"), Operations.Saves, 0);
	TestEqual(TEXT("copy adds exactly the selected nodes"), CountNativeNodes(Fixture.Blueprint), NodesBefore + 2);

	UEdGraphNode* HeadCopyNode = FindNodeByGuidInGraph(DestinationGraph, HeadCopy);
	UEdGraphNode* TailCopyNode = FindNodeByGuidInGraph(DestinationGraph, TailCopy);
	TestNotNull(TEXT("the copied chain head resolves at its derived identity"), HeadCopyNode);
	TestNotNull(TEXT("the copied chain tail resolves at its derived identity"), TailCopyNode);
	if (HeadCopyNode && TailCopyNode)
	{
		TestEqual(TEXT("the copy keeps the source class"), HeadCopyNode->GetClass()->GetPathName(),
			FString(UK2Node_CallFunction::StaticClass()->GetPathName()));
		TestEqual(TEXT("the copy keeps the source comment"), HeadCopyNode->NodeComment, Head->NodeComment);
		TestEqual(TEXT("the copy keeps the source layout"), HeadCopyNode->NodePosX, Head->NodePosX);
		TestEqual(TEXT("the copy keeps the source layout Y"), HeadCopyNode->NodePosY, Head->NodePosY);
		TestTrue(TEXT("the copied internal edge is realized"), NodesLinked(HeadCopyNode, TEXT("then"), TailCopyNode, TEXT("execute")));
	}
	TestNotNull(TEXT("the source chain head still exists"), FindNodeByGuid(Fixture.Blueprint, Head->NodeGuid));
	TestTrue(TEXT("the source internal edge is untouched"),
		NodesLinked(Head, TEXT("then"), Tail, TEXT("execute")));
	{
		const FString SourceAfter = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
		TestTrue(FString::Printf(TEXT("the copy left the source graph byte-identical [%s]"),
			*FirstCaptureDifference(SourceCaptureBefore, SourceAfter)), SourceAfter == SourceCaptureBefore);
	}
	TestNotEqual(TEXT("the copy changed the authored fingerprint"), LiveGraphHash(Fixture.Blueprint), SourceHashBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 2. MoveInternalChain
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferMoveInternalChainTest,
	"Cortex.Graph.Authoring.Migration.Transfer.MoveInternalChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferMoveInternalChainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferMoveChain_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferMoveTarget"));
	TestNotNull(TEXT("destination function graph created"), DestinationGraph);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const int32 SourceNodesBefore = SourceGraph->Nodes.Num();
	const FString HeadComment = Head->NodeComment;
	const int32 HeadX = Head->NodePosX;

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120201"), MoveOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("move preview succeeds: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestEqual(TEXT("move preview publishes the removal set"), Plan.RemovalSet.Num(), 2);
	TestTrue(TEXT("move preview keeps the documented identity"),
		Plan.Nodes.Num() == 2
		&& Plan.Nodes[0].DestinationGuid == Plan.Nodes[0].SourceGuid
		&& Plan.Nodes[1].DestinationGuid == Plan.Nodes[1].SourceGuid);

	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("move applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("move reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("move compiles once"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("move readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("move never blocks"), Outcome.bBlocked, false);
	TestEqual(TEXT("move removes exactly the selected nodes from the source graph"),
		SourceGraph->Nodes.Num(), SourceNodesBefore - 2);
	TestNull(TEXT("the moved chain head is absent from the source graph"), FindNodeByGuidInGraph(SourceGraph, Head->NodeGuid));
	TestNull(TEXT("the moved chain tail is absent from the source graph"), FindNodeByGuidInGraph(SourceGraph, Tail->NodeGuid));

	UEdGraphNode* MovedHead = FindNodeByGuidInGraph(DestinationGraph, Head->NodeGuid);
	UEdGraphNode* MovedTail = FindNodeByGuidInGraph(DestinationGraph, Tail->NodeGuid);
	TestNotNull(TEXT("the moved chain head preserves its identity in the destination"), MovedHead);
	TestNotNull(TEXT("the moved chain tail preserves its identity in the destination"), MovedTail);
	if (MovedHead && MovedTail)
	{
		TestTrue(TEXT("the moved chain keeps its identity exactly once"), MovedHead != Head);
		TestEqual(TEXT("asset-wide uniqueness holds for the moved head"), CountNodesWithGuid(Fixture.Blueprint, Head->NodeGuid), 1);
		TestEqual(TEXT("the move keeps the source comment"), MovedHead->NodeComment, HeadComment);
		TestEqual(TEXT("the move keeps the source layout"), MovedHead->NodePosX, HeadX);
		TestTrue(TEXT("the moved internal edge is realized"), NodesLinked(MovedHead, TEXT("then"), MovedTail, TEXT("execute")));
	}
	// The source graph must be free of dangling links into the moved identities.
	bool bDangling = false;
	for (UEdGraphNode* Node : SourceGraph->Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* Far = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (Far && Selection.Contains(Far->NodeGuid)) bDangling = true;
			}
		}
	}
	TestFalse(TEXT("the source graph keeps no dangling link into the moved nodes"), bDangling);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 3. ExternalConsumerBoundaryMapped
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferExternalConsumerTest,
	"Cortex.Graph.Authoring.Migration.Transfer.ExternalConsumerBoundaryMapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferExternalConsumerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferExternal_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	UK2Node_CallFunction* Consumer = AddPrintNode(SourceGraph, TEXT("consumer"), 800, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	TestTrue(TEXT("external consumer wired"), LinkNodes(SourceGraph, Tail, TEXT("then"), Consumer, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferExternalTarget"));
	UK2Node_CallFunction* DestinationConsumer = AddPrintNode(DestinationGraph, TEXT("destination consumer"), 700, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString SourceCaptureBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Tail, TEXT("then"), DestinationConsumer, TEXT("execute"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120301"), CopyOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("external-consumer copy previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	{
		// The published preview response carries the bounded transfer inventory, so a caller learns
		// which crossing edges need boundary mappings without reading the durable plan.
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		TSharedPtr<FJsonObject> PreviewRequest = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000120301"), CopyOp, SourceGraph, Selection, DestinationGraph, Boundary);
		const FCortexCommandResult PreviewResult = Router.Execute(TEXT("graph.apply_patch"), PreviewRequest);
		TestTrue(FString::Printf(TEXT("the transfer preview succeeds through the command path: %s"), *PreviewResult.ErrorMessage),
			PreviewResult.bSuccess);
		if (PreviewResult.bSuccess && PreviewResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Crossing = nullptr;
			TestTrue(TEXT("the preview publishes the crossing edges"),
				PreviewResult.Data->TryGetArrayField(TEXT("crossing_edges"), Crossing) && Crossing && Crossing->Num() == 1);
			if (Crossing && Crossing->Num() == 1)
			{
				FString CrossingText;
				(*Crossing)[0]->TryGetString(CrossingText);
				TestTrue(FString::Printf(TEXT("the published crossing edge names the covered edge [%s]"), *CrossingText),
					CrossingText.Contains(Tail->NodeGuid.ToString()) && CrossingText.Contains(Consumer->NodeGuid.ToString()));
			}
			const TArray<TSharedPtr<FJsonValue>>* PublishedBoundary = nullptr;
			TestTrue(TEXT("the preview publishes the boundary mapping"),
				PreviewResult.Data->TryGetArrayField(TEXT("boundary"), PublishedBoundary)
				&& PublishedBoundary && PublishedBoundary->Num() == 1);
			const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
			TestTrue(TEXT("the preview publishes the dependency inventory"),
				PreviewResult.Data->TryGetArrayField(TEXT("dependencies"), Dependencies) && Dependencies);
		}
	}
	TestEqual(TEXT("the crossing edge is reported as one boundary mapping"), Plan.Boundary.Num(), 1);
	if (Plan.Boundary.Num() == 1)
	{
		TestEqual(TEXT("the boundary mapping names the covered crossing edge"),
			Plan.Boundary[0].SourceFarGuid, Consumer->NodeGuid.ToString());
		TestEqual(TEXT("the boundary mapping names the crossing pin"),
			Plan.Boundary[0].SourceFarPin, FString(TEXT("execute")));
		TestEqual(TEXT("the boundary mapping names the destination endpoint"),
			Plan.Boundary[0].DestinationNodeGuid, DestinationConsumer->NodeGuid.ToString());
	}

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("external-consumer copy applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("external-consumer copy readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));

	UEdGraphNode* TailCopy = FindNodeByGuidInGraph(DestinationGraph,
		DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000120301"), Tail->NodeGuid));
	TestNotNull(TEXT("the copied tail resolves"), TailCopy);
	if (TailCopy)
	{
		TestTrue(TEXT("the boundary mapping is realized on the copied node"),
			NodesLinked(TailCopy, TEXT("then"), DestinationConsumer, TEXT("execute")));
		TestEqual(TEXT("the destination boundary pin owns exactly that link"),
			CountLinkedPins(DestinationConsumer, TEXT("execute")), 1);
	}
	TestTrue(TEXT("the source crossing edge stays untouched by a copy"),
		NodesLinked(Tail, TEXT("then"), Consumer, TEXT("execute")));

	{
		const FString SourceAfter = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
		TestTrue(FString::Printf(TEXT("the copy left the source graph byte-identical [%s]"),
			*FirstCaptureDifference(SourceCaptureBefore, SourceAfter)), SourceAfter == SourceCaptureBefore);
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4. UncoveredCrossingRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferUncoveredCrossingTest,
	"Cortex.Graph.Authoring.Migration.Transfer.UncoveredCrossingRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferUncoveredCrossingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferUncovered_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Consumer = AddPrintNode(SourceGraph, TEXT("consumer"), 400, 0);
	TestTrue(TEXT("crossing edge wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Consumer, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferUncoveredTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const FString SourceCaptureBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);

	const TArray<FGuid> Selection = { Head->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120401"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("an uncovered crossing edge is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the uncovered crossing edge [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("crossing edge"))
		&& Error.ErrorMessage.Contains(Head->NodeGuid.ToString())
		&& Error.ErrorMessage.Contains(Consumer->NodeGuid.ToString()));
	TestEqual(TEXT("the refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestEqual(TEXT("the refusal leaves the source graph"), CaptureGraphNative(Fixture.Blueprint, SourceGraph), SourceCaptureBefore);

	// A boundary entry that maps a non-crossing pin is refused under the same rule.
	UK2Node_CallFunction* DestinationPrint = AddPrintNode(DestinationGraph, TEXT("destination"), 700, 0);
	const FString WrongBoundaryHashBefore = LiveGraphHash(Fixture.Blueprint);
	TArray<TSharedPtr<FJsonValue>> WrongBoundary;
	WrongBoundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Head, TEXT("InString"), DestinationPrint, TEXT("InString"))));
	TSharedPtr<FJsonObject> WrongRequest = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120402"), CopyOp, SourceGraph, Selection, DestinationGraph, WrongBoundary);
	FCortexCommandResult WrongError;
	TestFalse(TEXT("a boundary entry that names a non-crossing pin is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, WrongRequest, Outcome, WrongError));
	TestTrue(FString::Printf(TEXT("the refusal names the pin that is not a crossing edge [%s]"), *WrongError.ErrorMessage),
		WrongError.ErrorMessage.Contains(TEXT("not a crossing edge")));
	TestEqual(TEXT("the second refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), WrongBoundaryHashBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 5. SharedPureDataBoundary
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferSharedPureDataTest,
	"Cortex.Graph.Authoring.Migration.Transfer.SharedPureDataBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferSharedPureDataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferSharedPure_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Producer = AddPureIntToStringNode(SourceGraph, 0, 0);
	UK2Node_CallFunction* Consumer = AddPrintNode(SourceGraph, TEXT("consumer"), 400, 0);
	TestTrue(TEXT("shared pure data feeds the selection"),
		LinkNodes(SourceGraph, Producer, TEXT("ReturnValue"), Consumer, TEXT("InString")));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const int32 ProducerLinksBefore = CountLinkedPins(Producer, TEXT("ReturnValue"));

	const TArray<FGuid> Selection = { Consumer->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Consumer, TEXT("InString"), Producer, TEXT("ReturnValue"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120501"), CopyOp, SourceGraph, Selection, SourceGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("shared-pure-data copy previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestEqual(TEXT("the shared pure producer is reported as the covered crossing edge"),
		Plan.Boundary.Num() == 1 ? Plan.Boundary[0].SourceFarGuid : FString(), Producer->NodeGuid.ToString());

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("shared-pure-data copy applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("shared-pure-data copy readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	if (Outcome.ApplyStatus == TEXT("applied"))
	{
		FString ReadbackFailure;
		TestTrue(FString::Printf(TEXT("the shared comparator matches the live asset after the copy [%s]"), *ReadbackFailure),
			FCortexGraphMigrationOps::VerifyTransferAgainstNative(Fixture.Blueprint, Plan, ReadbackFailure));
	}

	UEdGraphNode* ConsumerCopy = FindNodeByGuidInGraph(SourceGraph,
		DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000120501"), Consumer->NodeGuid));
	TestNotNull(TEXT("the copied consumer resolves in the same graph"), ConsumerCopy);
	if (ConsumerCopy)
	{
		TestTrue(TEXT("the copy is fed by the shared producer instead of duplicating it"),
			NodesLinked(Producer, TEXT("ReturnValue"), ConsumerCopy, TEXT("InString")));
		TestTrue(TEXT("the original consumer keeps its own link to the shared producer"),
			NodesLinked(Producer, TEXT("ReturnValue"), Consumer, TEXT("InString")));
		TestEqual(TEXT("the shared pure producer was not duplicated"),
			CountLinkedPins(Producer, TEXT("ReturnValue")), ProducerLinksBefore + 1);
	}
	TestEqual(TEXT("the shared producer still exists exactly once"),
		CountNodesWithGuid(Fixture.Blueprint, Producer->NodeGuid), 1);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 6. LocalVariableBoundaryRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferLocalVariableRefusedTest,
	"Cortex.Graph.Authoring.Migration.Transfer.LocalVariableBoundaryRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferLocalVariableRefusedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferLocalVarRefuse_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexLocalVarSource"));
	TestTrue(TEXT("source declares the local variable"), DeclareLocalInt(Fixture.Blueprint, SourceGraph, TEXT("LocalCount")));
	UK2Node_VariableGet* LocalRead = AddLocalVariableGetNode(Fixture.Blueprint, SourceGraph, TEXT("LocalCount"), 0, 0);
	TestNotNull(TEXT("local variable read created"), LocalRead);
	TestTrue(TEXT("local variable read is a local scope reference"),
		LocalRead->VariableReference.IsLocalScope());
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexLocalVarTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const FString SourceCaptureBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);

	const TArray<FGuid> Selection = { LocalRead->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120601"), MoveOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("a local variable the destination does not declare is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the local variable refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the variable [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("LocalCount")));
	TestTrue(FString::Printf(TEXT("the refusal names the node [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(LocalRead->NodeGuid.ToString()));
	TestEqual(TEXT("the refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestEqual(TEXT("the refusal leaves the source graph"), CaptureGraphNative(Fixture.Blueprint, SourceGraph), SourceCaptureBefore);

	// The same request is refused while the destination declares the variable with another type.
	{
		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;
		UEdGraph* WrongTypeGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexLocalVarWrongType"));
		FBlueprintEditorUtils::AddLocalVariable(Fixture.Blueprint, WrongTypeGraph, FName(TEXT("LocalCount")), StringType, TEXT(""));
		const FString WrongTypeHashBefore = LiveGraphHash(Fixture.Blueprint);
		TSharedPtr<FJsonObject> WrongRequest = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000120602"), MoveOp, SourceGraph, Selection, WrongTypeGraph, {});
		FCortexCommandResult WrongError;
		TestFalse(TEXT("a local variable declared with another type is refused"),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, WrongRequest, Outcome, WrongError));
		TestTrue(FString::Printf(TEXT("the type refusal names the variable and both types [%s]"), *WrongError.ErrorMessage),
			WrongError.ErrorMessage.Contains(TEXT("LocalCount"))
			&& WrongError.ErrorMessage.Contains(TEXT("different type")));
		TestEqual(TEXT("the type refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), WrongTypeHashBefore);
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 7. LocalVariableBoundaryMapped
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferLocalVariableMappedTest,
	"Cortex.Graph.Authoring.Migration.Transfer.LocalVariableBoundaryMapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferLocalVariableMappedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferLocalVarMap_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexLocalVarSource"));
	TestTrue(TEXT("source declares the local variable"), DeclareLocalInt(Fixture.Blueprint, SourceGraph, TEXT("LocalCount")));
	UK2Node_VariableGet* LocalRead = AddLocalVariableGetNode(Fixture.Blueprint, SourceGraph, TEXT("LocalCount"), 0, 0);
	UK2Node_CallFunction* Add = AddIntAddNode(SourceGraph, 300, 0);
	UK2Node_CallFunction* Print = AddPrintNode(SourceGraph, TEXT("local chain"), 600, 0);
	TestTrue(TEXT("local read feeds the pure add"),
		LinkNodes(SourceGraph, LocalRead, TEXT("LocalCount"), Add, TEXT("A")));
	UK2Node_CallFunction* Convert = AddPureIntToStringNode(SourceGraph, 400, 200);
	TestTrue(TEXT("add feeds the conversion"),
		LinkNodes(SourceGraph, Add, TEXT("ReturnValue"), Convert, TEXT("InInt")));
	TestTrue(TEXT("conversion feeds the print"),
		LinkNodes(SourceGraph, Convert, TEXT("ReturnValue"), Print, TEXT("InString")));
	UK2Node_FunctionEntry* SourceEntry = FindEntryNode(SourceGraph);
	TestNotNull(TEXT("source entry terminator exists"), SourceEntry);
	TestTrue(TEXT("source entry drives the chain"),
		LinkNodes(SourceGraph, SourceEntry, TEXT("then"), Print, TEXT("execute")));

	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexLocalVarTarget"));
	TestTrue(TEXT("destination declares the identically typed local variable"),
		DeclareLocalInt(Fixture.Blueprint, DestinationGraph, TEXT("LocalCount")));
	UK2Node_FunctionEntry* DestinationEntry = FindEntryNode(DestinationGraph);
	TestNotNull(TEXT("destination entry terminator exists"), DestinationEntry);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { LocalRead->NodeGuid, Add->NodeGuid, Convert->NodeGuid, Print->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Print, TEXT("execute"), DestinationEntry, TEXT("then"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120701"), MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("local-variable transfer previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	const FCortexGraphTransferDependency* LocalDependency =
		FindDependency(Plan, LocalRead->NodeGuid, TEXT("local_variable"));
	TestNotNull(TEXT("preview reports the local variable dependency"), LocalDependency);
	if (LocalDependency)
	{
		TestEqual(TEXT("the reported dependency names the variable"), LocalDependency->Member, FString(TEXT("LocalCount")));
		TestFalse(TEXT("the reported dependency carries the source variable type"), LocalDependency->Type.IsEmpty());
	}

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("local-variable transfer applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("local-variable transfer readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("local-variable transfer compiles once"), Outcome.CompileStatus, FString(TEXT("compiled")));

	UEdGraphNode* MovedRead = FindNodeByGuidInGraph(DestinationGraph, LocalRead->NodeGuid);
	UEdGraphNode* MovedPrint = FindNodeByGuidInGraph(DestinationGraph, Print->NodeGuid);
	TestNotNull(TEXT("the moved local read resolves in the destination"), MovedRead);
	if (const UK2Node_VariableGet* MovedVariable = Cast<UK2Node_VariableGet>(MovedRead))
	{
		TestTrue(TEXT("the moved read stays a local scope reference"), MovedVariable->VariableReference.IsLocalScope());
		TestEqual(TEXT("the moved read names the same variable"),
			MovedVariable->VariableReference.GetMemberName().ToString(), FString(TEXT("LocalCount")));
	}
	if (MovedRead && MovedPrint)
	{
		TestTrue(TEXT("the moved terminator boundary is realized"),
			NodesLinked(MovedPrint, TEXT("execute"), DestinationEntry, TEXT("then")));
	}
	TestNull(TEXT("the moved chain head is absent from the source graph"),
		FindNodeByGuidInGraph(SourceGraph, LocalRead->NodeGuid));
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 8. InterfaceMemberDependency
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferInterfaceDependencyTest,
	"Cortex.Graph.Authoring.Migration.Transfer.InterfaceMemberDependency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferInterfaceDependencyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferInterface_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexInterfaceSource"));
	UK2Node_FunctionEntry* SourceEntry = FindEntryNode(SourceGraph);
	UK2Node_CallFunction* InterfaceCall = AddInterfaceCallNode(SourceGraph, 0, 0);
	TestTrue(TEXT("interface call wired to the source entry"),
		LinkNodes(SourceGraph, SourceEntry, TEXT("then"), InterfaceCall, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexInterfaceTarget"));
	UK2Node_FunctionEntry* DestinationEntry = FindEntryNode(DestinationGraph);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { InterfaceCall->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(InterfaceCall, TEXT("execute"), DestinationEntry, TEXT("then"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120801"), CopyOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("interface dependency previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	const FCortexGraphTransferDependency* InterfaceDependency =
		FindDependency(Plan, InterfaceCall->NodeGuid, TEXT("interface"));
	TestNotNull(TEXT("preview reports the interface member dependency as an interface"), InterfaceDependency);
	if (InterfaceDependency)
	{
		TestEqual(TEXT("the reported dependency names the interface member"),
			InterfaceDependency->Member, FString(TEXT("OnInterfacePing")));
		TestTrue(FString::Printf(TEXT("the reported dependency names the declaring interface [%s]"),
			*InterfaceDependency->OwnerClass),
			InterfaceDependency->OwnerClass.Contains(TEXT("CortexGraphMigrationFixtureInterface")));
	}

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("interface dependency transfer applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("interface dependency readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));

	UEdGraphNode* CopiedCall = FindNodeByGuidInGraph(DestinationGraph,
		DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000120801"), InterfaceCall->NodeGuid));
	TestNotNull(TEXT("the copied interface call resolves"), CopiedCall);
	if (const UK2Node_CallFunction* CopiedFunction = Cast<UK2Node_CallFunction>(CopiedCall))
	{
		TestEqual(TEXT("the copied call keeps the interface member symbol"),
			CopiedFunction->FunctionReference.GetMemberName().ToString(), FString(TEXT("OnInterfacePing")));
		TestTrue(TEXT("the copied call keeps its self context"), CopiedFunction->FunctionReference.IsSelfContext());
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 9. FunctionTerminatorBoundary
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferTerminatorBoundaryTest,
	"Cortex.Graph.Authoring.Migration.Transfer.FunctionTerminatorBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferTerminatorBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferTerminator_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTerminatorSource"));
	UK2Node_FunctionEntry* SourceEntry = FindEntryNode(SourceGraph);
	TestNotNull(TEXT("source terminator exists"), SourceEntry);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("terminator chain"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("terminator tail"), 400, 0);
	TestTrue(TEXT("terminator feeds the selected head"),
		LinkNodes(SourceGraph, SourceEntry, TEXT("then"), Head, TEXT("execute")));
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTerminatorTarget"));
	UK2Node_FunctionEntry* DestinationEntry = FindEntryNode(DestinationGraph);
	TestNotNull(TEXT("destination terminator exists"), DestinationEntry);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Head, TEXT("execute"), DestinationEntry, TEXT("then"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120901"), MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("terminator boundary previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestEqual(TEXT("the terminator crossing needs exactly one boundary entry"), Plan.Boundary.Num(), 1);
	if (Plan.Boundary.Num() == 1)
	{
		TestEqual(TEXT("the boundary entry names the source terminator as the covered crossing edge"),
			Plan.Boundary[0].SourceFarGuid, SourceEntry->NodeGuid.ToString());
	}

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("terminator boundary transfer applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("terminator boundary readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	UEdGraphNode* MovedHead = FindNodeByGuidInGraph(DestinationGraph, Head->NodeGuid);
	TestNotNull(TEXT("the moved head resolves"), MovedHead);
	if (MovedHead)
	{
		TestTrue(TEXT("the destination terminator boundary is realized"),
			NodesLinked(DestinationEntry, TEXT("then"), MovedHead, TEXT("execute")));
	}

	// The same transfer without the boundary entry is refused: a terminator crossing is never implicit.
	UEdGraph* SecondSource = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTerminatorSource2"));
	UK2Node_FunctionEntry* SecondEntry = FindEntryNode(SecondSource);
	UK2Node_CallFunction* SecondHead = AddPrintNode(SecondSource, TEXT("second head"), 0, 0);
	TestTrue(TEXT("second terminator feeds the second selection"),
		LinkNodes(SecondSource, SecondEntry, TEXT("then"), SecondHead, TEXT("execute")));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const TArray<FGuid> SecondSelection = { SecondHead->NodeGuid };
	TSharedPtr<FJsonObject> Unmapped = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000120902"), MoveOp, SecondSource, SecondSelection, DestinationGraph, {});
	FCortexCommandResult UnmappedError;
	TestFalse(TEXT("an unmapped terminator crossing is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Unmapped, Outcome, UnmappedError));
	TestTrue(FString::Printf(TEXT("the refusal names the terminator crossing [%s]"), *UnmappedError.ErrorMessage),
		UnmappedError.ErrorMessage.Contains(SecondEntry->NodeGuid.ToString()));
	TestEqual(TEXT("the terminator refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 10. DuplicateDestinationIdentityRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferDuplicateIdentityTest,
	"Cortex.Graph.Authoring.Migration.Transfer.DuplicateDestinationIdentityRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferDuplicateIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferDuplicate_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferDuplicateTarget"));

	// A node already occupies one planned destination identity, so the identity set is partial.
	UK2Node_CallFunction* Squatter = AddPrintNode(DestinationGraph, TEXT("squatter"), 900, 0);
	Squatter->NodeGuid = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121001"), Head->NodeGuid);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121001"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("a partial destination identity set is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the duplicate-identity refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the partial identity set [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("partial"))
		&& Error.ErrorMessage.Contains(DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121001"), Tail->NodeGuid).ToString()));
	TestEqual(TEXT("the refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestTrue(TEXT("the squatter still owns its identity exactly once"),
		CountNodesWithGuid(Fixture.Blueprint, Squatter->NodeGuid) == 1);

	// A destination node elsewhere in the asset is a conflict that can never be repaired.
	UEdGraph* OtherGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferDuplicateOther"));
	UK2Node_CallFunction* Foreign = AddPrintNode(OtherGraph, TEXT("foreign"), 0, 900);
	Foreign->NodeGuid = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121002"), Tail->NodeGuid);
	const FString ConflictHashBefore = LiveGraphHash(Fixture.Blueprint);
	TSharedPtr<FJsonObject> ConflictRequest = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121002"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexCommandResult ConflictError;
	TestFalse(TEXT("a planned identity that another graph already owns is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, ConflictRequest, Outcome, ConflictError));
	TestTrue(FString::Printf(TEXT("the conflict refusal names the owning graph [%s]"), *ConflictError.ErrorMessage),
		ConflictError.ErrorMessage.Contains(TEXT("already exists in graph")));
	TestEqual(TEXT("the conflict refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), ConflictHashBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 11. UnsupportedNodeRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferUnsupportedNodeTest,
	"Cortex.Graph.Authoring.Migration.Transfer.UnsupportedNodeRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferUnsupportedNodeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferUnsupported_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferUnsupportedTarget"));

	auto RefuseUnsupported = [&](UEdGraphNode* Node, const TCHAR* Expected, const TCHAR* PatchId, int32 Index)
	{
		if (!Node)
		{
			TestTrue(FString::Printf(TEXT("unsupported fixture %d was created"), Index), false);
			return;
		}
		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
		const TArray<FGuid> Selection = { Node->NodeGuid };
		TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint, PatchId, CopyOp, SourceGraph, Selection, DestinationGraph, {});
		FCortexCommandResult Error;
		FCortexGraphPatchOutcome Outcome;
		TestFalse(FString::Printf(TEXT("unsupported node %d is refused"), Index),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(FString::Printf(TEXT("unsupported node %d refusal is INVALID_OPERATION"), Index),
			Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("unsupported node %d refusal names the class and the reason [%s]"), Index, *Error.ErrorMessage),
			Error.ErrorMessage.Contains(Node->GetClass()->GetName())
			&& Error.ErrorMessage.Contains(Expected));
		TestEqual(FString::Printf(TEXT("unsupported node %d refusal mutates nothing"), Index),
			LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestEqual(FString::Printf(TEXT("unsupported node %d refusal leaves the node count"), Index),
			CountNativeNodes(Fixture.Blueprint), NodesBefore);
	};

	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(SourceGraph);
	Knot->CreateNewGuid();
	Knot->AllocateDefaultPins();
	Knot->NodePosX = 0;
	Knot->NodePosY = 600;
	SourceGraph->AddNode(Knot, true, false);
	RefuseUnsupported(Knot, TEXT("knot"), TEXT("00000000-0000-0000-0000-000000121101"), 1);

	UK2Node_Tunnel* Tunnel = NewObject<UK2Node_Tunnel>(SourceGraph);
	Tunnel->CreateNewGuid();
	Tunnel->AllocateDefaultPins();
	Tunnel->NodePosX = 300;
	Tunnel->NodePosY = 600;
	SourceGraph->AddNode(Tunnel, true, false);
	RefuseUnsupported(Tunnel, TEXT("tunnel"), TEXT("00000000-0000-0000-0000-000000121102"), 2);

	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(SourceGraph);
	Composite->CreateNewGuid();
	Composite->NodePosX = 600;
	Composite->NodePosY = 600;
	SourceGraph->AddNode(Composite, true, false);
	RefuseUnsupported(Composite, TEXT("composite"), TEXT("00000000-0000-0000-0000-000000121103"), 3);

	UK2Node_CallFunction* Latent = NewObject<UK2Node_CallFunction>(SourceGraph);
	Latent->FunctionReference.SetExternalMember(FName(TEXT("Delay")), UKismetSystemLibrary::StaticClass());
	Latent->CreateNewGuid();
	Latent->AllocateDefaultPins();
	Latent->NodePosX = 900;
	Latent->NodePosY = 600;
	SourceGraph->AddNode(Latent, true, false);
	TestTrue(TEXT("the latent fixture call really is latent"), Latent->IsLatentFunction());
	RefuseUnsupported(Latent, TEXT("latent"), TEXT("00000000-0000-0000-0000-000000121104"), 4);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 12. CrossAssetDestinationRefused
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferCrossAssetTest,
	"Cortex.Graph.Authoring.Migration.Transfer.CrossAssetDestinationRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferCrossAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferCrossAsset_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FFixture Other;
	TestTrue(TEXT("second fixture created"), Other.Create(TEXT("BP_TransferCrossAssetOther_T12")));
	if (!Other.Blueprint) { Fixture.Cleanup(); Other.Cleanup(); return false; }

	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UEdGraph* ForeignGraph = EnsureEventGraph(Other.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121201"), CopyOp, SourceGraph, Selection, ForeignGraph, {});
	FCortexCommandResult Error;
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("a cross-asset destination is refused"),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("the cross-asset refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names cross-asset transfer [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("cross-asset")));
	TestEqual(TEXT("the cross-asset refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the cross-asset refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestTrue(TEXT("the foreign asset is untouched"), CountNativeNodes(Other.Blueprint) > 0);

	Fixture.Cleanup();
	Other.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 13. FailedMoveRestoresBothGraphs
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferFailedMoveRestoresTest,
	"Cortex.Graph.Authoring.Migration.Transfer.FailedMoveRestoresBothGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferFailedMoveRestoresTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	auto RunFaultCase = [&](const TCHAR* FixtureName, const TCHAR* FaultPoint, int32 Index)
	{
		FFixture Fixture;
		TestTrue(FString::Printf(TEXT("fault case %d fixture created"), Index), Fixture.Create(FixtureName));
		if (!Fixture.Blueprint) { Fixture.Cleanup(); return; }
		UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
		UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
		UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
		UK2Node_CallFunction* Consumer = AddPrintNode(SourceGraph, TEXT("consumer"), 800, 0);
		TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
		TestTrue(TEXT("external consumer wired"), LinkNodes(SourceGraph, Tail, TEXT("then"), Consumer, TEXT("execute")));
		UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexFailedMoveTarget"));
		UK2Node_CallFunction* DestinationConsumer = AddPrintNode(DestinationGraph, TEXT("destination consumer"), 700, 0);
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

		const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
		const FString SourceBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
		const FString DestinationBefore = CaptureGraphNative(Fixture.Blueprint, DestinationGraph);
		const bool bDirtyBefore = Fixture.Package->IsDirty();
		const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
		const FGuid SourceEntryGuid = FindEntryNode(SourceGraph) ? FindEntryNode(SourceGraph)->NodeGuid : FGuid();

		const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
		TArray<TSharedPtr<FJsonValue>> Boundary;
		Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Tail, TEXT("then"), DestinationConsumer, TEXT("execute"))));
		const TCHAR* PatchId = Index == 1
			? TEXT("00000000-0000-0000-0000-000000121301")
			: (Index == 2 ? TEXT("00000000-0000-0000-0000-000000121303") : TEXT("00000000-0000-0000-0000-000000121302"));
		TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint, PatchId, MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
		FCortexGraphPreparedPatch Prepared;
		FCortexGraphMigrationTransferPlan Plan;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("fault case %d move previews: %s"), Index, *Error.ErrorMessage),
			PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));

		FCortexGraphPatchOps::SetApplyFaultPointForTesting(FName(FaultPoint));
		FOperations Operations;
		Operations.Begin();
		FCortexGraphPatchOutcome Outcome;
		TestFalse(FString::Printf(TEXT("fault case %d fails at %s"), Index, FaultPoint),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		Operations.End();
		FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);

		TestEqual(FString::Printf(TEXT("fault case %d reports a restore [%s]"), Index, *JoinDiagnostics(Outcome.Diagnostics)),
			Outcome.RollbackStatus, FString(TEXT("restored")));
		TestEqual(FString::Printf(TEXT("fault case %d never blocks the asset"), Index),
			Outcome.bBlocked, false);
		const FString SourceAfter = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
		const FString DestinationAfter = CaptureGraphNative(Fixture.Blueprint, DestinationGraph);
		TestTrue(FString::Printf(TEXT("fault case %d restores the source graph [%s]"), Index,
			*FirstCaptureDifference(SourceBefore, SourceAfter)), SourceAfter == SourceBefore);
		TestTrue(FString::Printf(TEXT("fault case %d restores the destination graph [%s]"), Index,
			*FirstCaptureDifference(DestinationBefore, DestinationAfter)), DestinationAfter == DestinationBefore);
		TestEqual(FString::Printf(TEXT("fault case %d restores the authored fingerprint"), Index),
			LiveGraphHash(Fixture.Blueprint), HashBefore);
		TestEqual(FString::Printf(TEXT("fault case %d restores the dirty flag"), Index),
			Fixture.Package->IsDirty(), bDirtyBefore);
		TestEqual(FString::Printf(TEXT("fault case %d restores the node count"), Index),
			CountNativeNodes(Fixture.Blueprint), NodesBefore);
		TestNotNull(FString::Printf(TEXT("fault case %d restores the moved node"), Index),
			FindNodeByGuidInGraph(SourceGraph, Head->NodeGuid));
		TestTrue(FString::Printf(TEXT("fault case %d restores the crossing link"), Index),
			NodesLinked(Tail, TEXT("then"), Consumer, TEXT("execute")));
		if (SourceEntryGuid.IsValid())
		{
			TestNotNull(FString::Printf(TEXT("fault case %d keeps the source terminator"), Index),
				FindNodeByGuidInGraph(SourceGraph, SourceEntryGuid));
		}

		// A retry of the restored request is a clean transfer, which proves recovery left no residue.
		FCortexCommandResult RetryError;
		FCortexGraphPatchOutcome RetryOutcome;
		TSharedPtr<FJsonObject> RetryRequest = TransferRequest(Fixture.Blueprint, PatchId, MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
		FCortexGraphPreparedPatch RetryPrepared;
		FCortexGraphMigrationTransferPlan RetryPlan;
		TestTrue(FString::Printf(TEXT("fault case %d retry previews: %s"), Index, *RetryError.ErrorMessage),
			PreviewPlan(Fixture.Blueprint, RetryRequest, RetryPrepared, RetryPlan, RetryError));
		TestTrue(FString::Printf(TEXT("fault case %d retry applies: %s [%s]"), Index, *RetryError.ErrorMessage, *JoinDiagnostics(RetryOutcome.Diagnostics)),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, RetryRequest, RetryOutcome, RetryError));
		TestEqual(FString::Printf(TEXT("fault case %d retry readback matched"), Index),
			RetryOutcome.ReadbackStatus, FString(TEXT("matched")));
		TestNotNull(FString::Printf(TEXT("fault case %d retry moved the node"), Index),
			FindNodeByGuidInGraph(DestinationGraph, Head->NodeGuid));
		Fixture.Cleanup();
	};

	RunFaultCase(TEXT("BP_TransferFaultDestination_T12"), TEXT("migration_transfer_after_destination"), 1);
	RunFaultCase(TEXT("BP_TransferFaultWiring_T12"), TEXT("migration_transfer_after_wiring"), 2);
	RunFaultCase(TEXT("BP_TransferFaultRemoval_T12"), TEXT("migration_transfer_after_source_removal"), 3);
	return true;
}

// ---------------------------------------------------------------------------
// 14. CopyLeavesSourceAndIdentityIntact
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferCopySourceIntactTest,
	"Cortex.Graph.Authoring.Migration.Transfer.CopyLeavesSourceAndIdentityIntact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferCopySourceIntactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferCopyIntact_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferCopyIntactTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString SourceHashBefore = LiveGraphHash(Fixture.Blueprint);
	const FString SourceCaptureBefore = CaptureGraphNative(Fixture.Blueprint, SourceGraph);

	// The selection is disjoint from the capture that follows it, so the hash stays unchanged.
	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121401"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("copy preview succeeds: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	const FGuid HeadCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121401"), Head->NodeGuid);
	TestTrue(TEXT("copy does not duplicate the source GUID"), HeadCopy != Head->NodeGuid);

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("copy applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("copy readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));

	TestNotNull(TEXT("the copied node exists"), FindNodeByGuidInGraph(DestinationGraph, HeadCopy));
	TestTrue(TEXT("the copied node is a distinct object"), FindNodeByGuidInGraph(DestinationGraph, HeadCopy) != static_cast<UEdGraphNode*>(Head));
	TestEqual(TEXT("asset-wide uniqueness holds for the copy identity"),
		CountNodesWithGuid(Fixture.Blueprint, HeadCopy), 1);
	TestEqual(TEXT("asset-wide uniqueness holds for the source identity"),
		CountNodesWithGuid(Fixture.Blueprint, Head->NodeGuid), 1);
	TestTrue(TEXT("the source node still resolves by its own identity"),
		FindNodeByGuid(Fixture.Blueprint, Head->NodeGuid) == Head);
	{
		const FString SourceAfter = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
		TestTrue(FString::Printf(TEXT("the copy left the whole source graph byte-identical [%s]"),
			*FirstCaptureDifference(SourceCaptureBefore, SourceAfter)), SourceAfter == SourceCaptureBefore);
	}
	TestNotEqual(TEXT("the copy still changes the authored fingerprint"), LiveGraphHash(Fixture.Blueprint), SourceHashBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 15. RepeatedCopyReconciled
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferRepeatedCopyTest,
	"Cortex.Graph.Authoring.Migration.Transfer.RepeatedCopyReconciled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferRepeatedCopyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferRepeatedCopy_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferRepeatedTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121501"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("first copy previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("first copy applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("first copy reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);
	const int32 TransactionsAfterFirst = TransactionCount();
	const FGuid HeadCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121501"), Head->NodeGuid);

	TSharedPtr<FJsonObject> Replay = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121501"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch ReplayPrepared;
	FCortexGraphMigrationTransferPlan ReplayPlan;
	FCortexCommandResult ReplayError;
	TestTrue(FString::Printf(TEXT("repeated copy previews: %s"), *ReplayError.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Replay, ReplayPrepared, ReplayPlan, ReplayError));
	TestFalse(TEXT("repeated copy preview reports no prospective change"), ReplayPrepared.bChanged);
	TestTrue(TEXT("repeated copy reconciles through the reuse route"), ReplayPlan.bReused);
	TestEqual(TEXT("repeated copy publishes both reused identities"), ReplayPrepared.ReusedClientIds.Num(), 2);
	{
		// The reconciliation compares the plan's destination contract against live native state, so
		// that comparison is asserted directly here with the first differing line reported.
		const FCortexGraphTransferPreservation* DestinationContract = Plan.Preservations.FindByPredicate(
			[](const FCortexGraphTransferPreservation& Contract)
			{
				return Contract.Label == TEXT("destination_graph");
			});
		TestNotNull(TEXT("the first plan publishes its destination contract"), DestinationContract);
		if (DestinationContract)
		{
			TArray<FGuid> Excluded;
			for (const FString& GuidText : DestinationContract->ExcludedGuids)
			{
				FGuid Guid;
				if (FGuid::Parse(GuidText, Guid)) Excluded.Add(Guid);
			}
			const FString Live = FCortexGraphMigrationOps::CapturePreservation(Fixture.Blueprint, DestinationGraph, Excluded);
			TestTrue(FString::Printf(TEXT("the destination contract still matches live state after the copy [%s]"),
				*FirstCaptureDifference(DestinationContract->Capture, Live)), Live == DestinationContract->Capture);
		}
	}
	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("repeated copy reports unchanged: %s"), *ReplayError.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, ReplayError));
	TestEqual(TEXT("repeated copy reports unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("repeated copy never compiles"), ReplayOutcome.TargetCompileCount, 0);
	TestEqual(TEXT("repeated copy opens no transaction"), TransactionCount(), TransactionsAfterFirst);
	TestEqual(TEXT("repeated copy appends no second node set"), CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);
	TestEqual(TEXT("repeated copy leaves exactly one copied node"), CountNodesWithGuid(Fixture.Blueprint, HeadCopy), 1);

	// A partially removed destination identity set refuses instead of appending a second copy.
	UEdGraphNode* Removed = FindNodeByGuidInGraph(DestinationGraph, HeadCopy);
	TestNotNull(TEXT("the copied node to remove resolves"), Removed);
	if (Removed)
	{
		DestinationGraph->RemoveNode(Removed);
		TSharedPtr<FJsonObject> Partial = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000121501"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
		FCortexGraphPreparedPatch PartialPrepared;
		FCortexCommandResult PartialError;
		TestFalse(TEXT("a partial destination identity set refuses on replay"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Partial, PartialPrepared, PartialError));
		TestEqual(TEXT("the partial replay refusal is INVALID_OPERATION"),
			PartialError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(FString::Printf(TEXT("the partial replay refusal names what is missing [%s]"), *PartialError.ErrorMessage),
			PartialError.ErrorMessage.Contains(TEXT("partial")) && PartialError.ErrorMessage.Contains(HeadCopy.ToString()));
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 16. MoveReplayIsUnchanged
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferMoveReplayTest,
	"Cortex.Graph.Authoring.Migration.Transfer.MoveReplayIsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferMoveReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferMoveReplay_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	UK2Node_CallFunction* StayBehind = AddPrintNode(SourceGraph, TEXT("stay behind"), 800, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferMoveReplayTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	// The move carries a boundary mapping for a real source crossing edge, so the replay must
	// reconcile that mapping instead of refusing it.
	UK2Node_CallFunction* SourceConsumer = AddPrintNode(SourceGraph, TEXT("move replay consumer"), 1200, 0);
	TestTrue(TEXT("the move's source crossing edge is wired"),
		LinkNodes(SourceGraph, Tail, TEXT("then"), SourceConsumer, TEXT("execute")));
	UK2Node_CallFunction* DestinationPrint = AddPrintNode(DestinationGraph, TEXT("move replay target"), 900, 0);
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Tail, TEXT("then"), DestinationPrint, TEXT("execute"))));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121601"), MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("move previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		TSharedPtr<FJsonObject> PreviewRequest = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000121601"), MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
		const FCortexCommandResult PreviewResult = Router.Execute(TEXT("graph.apply_patch"), PreviewRequest);
		TestTrue(FString::Printf(TEXT("the move preview succeeds through the command path: %s"), *PreviewResult.ErrorMessage),
			PreviewResult.bSuccess);
		if (PreviewResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Removal = nullptr;
			TestTrue(TEXT("the preview publishes the removal set"),
				PreviewResult.Data->TryGetArrayField(TEXT("removal_set"), Removal) && Removal && Removal->Num() == 2);
		}
	}
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("move applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("move reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	const int32 NodesAfterMove = CountNativeNodes(Fixture.Blueprint);
	const FString SourceCaptureAfterMove = CaptureGraphNative(Fixture.Blueprint, SourceGraph);
	const FString DestinationCaptureAfterMove = CaptureGraphNative(Fixture.Blueprint, DestinationGraph);
	const int32 TransactionsAfterMove = TransactionCount();

	TSharedPtr<FJsonObject> Replay = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121601"), MoveOp, SourceGraph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch ReplayPrepared;
	FCortexGraphMigrationTransferPlan ReplayPlan;
	FCortexCommandResult ReplayError;
	TestTrue(FString::Printf(TEXT("move replay previews: %s"), *ReplayError.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Replay, ReplayPrepared, ReplayPlan, ReplayError));
	TestFalse(TEXT("move replay preview reports no prospective change"), ReplayPrepared.bChanged);
	TestTrue(TEXT("move replay reconciles through the reuse route"), ReplayPlan.bReused);
	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("move replay reports unchanged: %s"), *ReplayError.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, ReplayError));
	TestEqual(TEXT("move replay reports unchanged"), ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("move replay never compiles"), ReplayOutcome.TargetCompileCount, 0);
	TestEqual(TEXT("move replay opens no transaction"), TransactionCount(), TransactionsAfterMove);
	TestEqual(TEXT("move replay adds no nodes"), CountNativeNodes(Fixture.Blueprint), NodesAfterMove);
	TestEqual(TEXT("move replay leaves the source graph"), CaptureGraphNative(Fixture.Blueprint, SourceGraph), SourceCaptureAfterMove);
	TestEqual(TEXT("move replay leaves the destination graph"), CaptureGraphNative(Fixture.Blueprint, DestinationGraph), DestinationCaptureAfterMove);
	TestEqual(TEXT("move replay keeps the moved identity unique"), CountNodesWithGuid(Fixture.Blueprint, Head->NodeGuid), 1);

	// A partial replay - one selected node still present in the source - refuses instead of guessing.
	TArray<FGuid> PartialSelection = Selection;
	PartialSelection.Add(StayBehind->NodeGuid);
	TSharedPtr<FJsonObject> Partial = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121602"), MoveOp, SourceGraph, PartialSelection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch PartialPrepared;
	FCortexCommandResult PartialError;
	TestFalse(TEXT("a partial move replay is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Partial, PartialPrepared, PartialError));
	TestEqual(TEXT("the partial replay refusal is INVALID_OPERATION"),
		PartialError.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the partial replay refusal names the still-present node [%s]"), *PartialError.ErrorMessage),
		PartialError.ErrorMessage.Contains(StayBehind->NodeGuid.ToString()));
	TestEqual(TEXT("the partial replay refusal mutates nothing"),
		CaptureGraphNative(Fixture.Blueprint, SourceGraph), SourceCaptureAfterMove);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 17. SaveReloadKeepsCopiedIdentity
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferSaveReloadTest,
	"Cortex.Graph.Authoring.Migration.Transfer.SaveReloadKeepsCopiedIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferSaveReloadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferSaveReload_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UK2Node_CallFunction* Tail = AddPrintNode(SourceGraph, TEXT("tail"), 400, 0);
	TestTrue(TEXT("internal chain wired"), LinkNodes(SourceGraph, Head, TEXT("then"), Tail, TEXT("execute")));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferSaveReloadTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	TestFalse(TEXT("baseline save leaves a clean package"), Fixture.Package->IsDirty());

	const TArray<FGuid> Selection = { Head->NodeGuid, Tail->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121701"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save=true copy previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	Request->SetBoolField(TEXT("save"), true);
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("save=true copy applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();
	TestEqual(TEXT("save=true copy reports the apply"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("save=true copy reports the save"), Outcome.SaveStatus, FString(TEXT("saved")));
	TestEqual(TEXT("save=true copy verifies post-save persistence"), Outcome.PostSaveStatus, FString(TEXT("verified")));
	TestEqual(TEXT("save=true copy performs exactly one save"), Operations.Saves, 1);
	TestFalse(TEXT("save=true copy leaves the package clean"), Fixture.Package->IsDirty());

	const FGuid HeadCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121701"), Head->NodeGuid);
	const FGuid TailCopy = DerivedCopyGuid(TEXT("00000000-0000-0000-0000-000000121701"), Tail->NodeGuid);
	const FGuid DestinationGraphGuid = DestinationGraph->GraphGuid;
	const FString ObjectName = Fixture.Blueprint->GetName();
	const FString PackageName = Fixture.Package->GetName();
	// The reload replaces these objects, so the expected authored state is snapshotted first.
	const FString ExpectedClass = Head->GetClass()->GetPathName();
	const FString ExpectedComment = Head->NodeComment;
	const int32 ExpectedPosX = Head->NodePosX;
	UBlueprint* const BeforeReload = Fixture.Blueprint;

	TArray<UPackage*> PackagesToReload;
	PackagesToReload.Add(BeforeReload->GetOutermost());
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
	TestTrue(FString::Printf(TEXT("engine reload of the saved fixture succeeds: %s"), *ReloadError.ToString()), bReloaded);
	UPackage* ReloadedPackage = FindPackage(nullptr, *PackageName);
	UBlueprint* Reloaded = ReloadedPackage ? FindObject<UBlueprint>(ReloadedPackage, *ObjectName) : nullptr;
	TestNotNull(TEXT("the reloaded Blueprint resolves from disk"), Reloaded);
	if (Reloaded)
	{
		TestTrue(TEXT("the reloaded Blueprint is a different UObject instance"), Reloaded != BeforeReload);
		UEdGraph* ReloadedDestination = FindGraphByGuid(Reloaded, DestinationGraphGuid);
		TestNotNull(TEXT("the destination graph survives the reload"), ReloadedDestination);
		if (ReloadedDestination)
		{
			UEdGraphNode* ReloadedHead = FindNodeByGuidInGraph(ReloadedDestination, HeadCopy);
			UEdGraphNode* ReloadedTail = FindNodeByGuidInGraph(ReloadedDestination, TailCopy);
			TestNotNull(TEXT("the copied head identity survives a real engine reload"), ReloadedHead);
			TestNotNull(TEXT("the copied tail identity survives a real engine reload"), ReloadedTail);
			if (ReloadedHead && ReloadedTail)
			{
				TestEqual(TEXT("the reloaded copy keeps its class"),
					ReloadedHead->GetClass()->GetPathName(), ExpectedClass);
				TestEqual(TEXT("the reloaded copy keeps its layout"), ReloadedHead->NodePosX, ExpectedPosX);
				TestEqual(TEXT("the reloaded copy keeps its comment"), ReloadedHead->NodeComment, ExpectedComment);
				TestTrue(TEXT("the reloaded copy keeps its internal edge"),
					NodesLinked(ReloadedHead, TEXT("then"), ReloadedTail, TEXT("execute")));
			}
			TestEqual(TEXT("the reloaded asset keeps each copied identity exactly once"),
				CountNodesWithGuid(Reloaded, HeadCopy), 1);
		}
		TestNotNull(TEXT("the reloaded source node still resolves"), FindNodeByGuid(Reloaded, Head->NodeGuid));
	}
	// The reload replaced the fixture objects, so cleanup addresses the live package and detaches
	// its mapped linker before the fixture file can be removed on Windows.
	ResetTransaction();
	if (ReloadedPackage)
	{
		ReloadedPackage->ClearFlags(RF_Standalone);
		ReloadedPackage->MarkAsGarbage();
		ResetLoaders(ReloadedPackage);
	}
	Fixture.Package = nullptr;
	Fixture.Blueprint = nullptr;
	FlushAsyncLoading();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("reload-case fixture file is removed in cleanup"), DeleteFixtureFile(Fixture.Filename));
	return true;
}

// ---------------------------------------------------------------------------
// 18. EnvelopeRefusals
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferEnvelopeTest,
	"Cortex.Graph.Authoring.Migration.Transfer.EnvelopeRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferEnvelope_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Head = AddPrintNode(SourceGraph, TEXT("head"), 0, 0);
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexTransferEnvelopeTarget"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const TArray<FGuid> Selection = { Head->NodeGuid };

	auto ExpectRefusal = [&](const TSharedPtr<FJsonObject>& Request, const FString& Code, const TCHAR* Context, const FString& Expected)
	{
		FCortexCommandResult Error;
		FCortexGraphPatchOutcome Outcome;
		TestFalse(FString::Printf(TEXT("%s is refused"), Context),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
		TestEqual(FString::Printf(TEXT("%s refusal code [%s]"), Context, *Error.ErrorMessage),
			Error.ErrorCode, FString(Code));
		TestTrue(FString::Printf(TEXT("%s refusal names the reason [%s]"), Context, *Error.ErrorMessage),
			Expected.IsEmpty() || Error.ErrorMessage.Contains(Expected));
		TestEqual(FString::Printf(TEXT("%s mutates nothing"), Context), LiveGraphHash(Fixture.Blueprint), HashBefore);
	};

	// The transfer shell must not carry the implementation target of replace_entry.
	TSharedPtr<FJsonObject> WithTarget = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121801"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRefJson(DestinationGraph));
	WithTarget->SetObjectField(TEXT("target"), Target);
	ExpectRefusal(WithTarget, CortexErrorCodes::InvalidField, TEXT("a transfer request that carries target"), TEXT("must be absent"));

	// The transfer shell must not carry the authoring arrays.
	TSharedPtr<FJsonObject> WithNodes = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121802"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	WithNodes->SetArrayField(TEXT("nodes"), { MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()) });
	ExpectRefusal(WithNodes, CortexErrorCodes::InvalidField, TEXT("a transfer request with an authoring node"), TEXT("nodes"));

	// An unknown nested field is refused like any other unknown envelope field.
	TSharedPtr<FJsonObject> UnknownNested = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121803"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	{
		const TSharedPtr<FJsonObject>* MigrationPtr = nullptr;
		UnknownNested->TryGetObjectField(TEXT("migration"), MigrationPtr);
		(*MigrationPtr)->SetStringField(TEXT("selection"), TEXT("all"));
	}
	ExpectRefusal(UnknownNested, CortexErrorCodes::InvalidField, TEXT("a transfer request with an unknown migration field"), TEXT("selection"));

	// A selection entry outside the named source graph is refused by identity.
	TSharedPtr<FJsonObject> ForeignSelection = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121804"), CopyOp, DestinationGraph, Selection, DestinationGraph, {});
	ExpectRefusal(ForeignSelection, CortexErrorCodes::InvalidOperation, TEXT("a selection from another graph"), Head->NodeGuid.ToString());

	// An empty selection is refused by the envelope.
	TSharedPtr<FJsonObject> EmptySelection = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121805"), CopyOp, SourceGraph, {}, DestinationGraph, {});
	ExpectRefusal(EmptySelection, CortexErrorCodes::InvalidField, TEXT("an empty selection"), TEXT("at least one node"));

	// move_subgraph must name a different destination graph.
	TSharedPtr<FJsonObject> SelfMove = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121806"), MoveOp, SourceGraph, Selection, SourceGraph, {});
	ExpectRefusal(SelfMove, CortexErrorCodes::InvalidOperation, TEXT("a move into the source graph"), TEXT("different from the source graph"));

	// An unknown migration operation is refused while the published ones stay accepted.
	TSharedPtr<FJsonObject> UnknownOp = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121807"), TEXT("rotate_subgraph"), SourceGraph, Selection, DestinationGraph, {});
	ExpectRefusal(UnknownOp, CortexErrorCodes::UnsupportedOperation, TEXT("an unknown migration operation"), TEXT("rotate_subgraph"));

	// A stale validation token is refused before any planning output is applied.
	TSharedPtr<FJsonObject> StaleToken = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121808"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	StaleToken->SetBoolField(TEXT("dry_run"), false);
	StaleToken->SetStringField(TEXT("expected_validation_hash"), TEXT("stale"));
	ExpectRefusal(StaleToken, CortexErrorCodes::StalePrecondition, TEXT("a stale validation token"), TEXT("expected_validation_hash"));

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 19. SplitPinLinkRefused (F1)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferSplitPinTest,
	"Cortex.Graph.Authoring.Migration.Transfer.SplitPinLinkRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferSplitPinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferSplitPin_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* SourceGraph = EnsureEventGraph(Fixture.Blueprint);
	UK2Node_CallFunction* Source = AddMakeVectorNode(SourceGraph, 0, 0);
	UEdGraphPin* ChildX = SplitStructPinChildren(Source, TEXT("ReturnValue"));
	TestNotNull(TEXT("the struct output pin is expanded"), ChildX);
	TestNotNull(TEXT("the expanded pin really is a split child"), ChildX ? ChildX->ParentPin : nullptr);
	UK2Node_CallFunction* Outside = AddFloatMultiplyNode(SourceGraph, 400, 0);
	TestTrue(TEXT("the split child is linked outside the selection"),
		ChildX && LinkPins(SourceGraph, ChildX, Outside->FindPin(TEXT("A"))));
	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexSplitPinTarget"));
	UK2Node_CallFunction* Destination = AddFloatMultiplyNode(DestinationGraph, 400, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	const FString HashBefore = LiveGraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	// A linked split child is refused instead of being silently dropped by the clone path.
	const TArray<FGuid> Selection = { Source->NodeGuid };
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000121901"), CopyOp, SourceGraph, Selection, DestinationGraph, {});
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("a linked split child is refused"),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("the split-child refusal is INVALID_OPERATION"), Error.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(FString::Printf(TEXT("the refusal names the node and the expanded pin [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(Source->NodeGuid.ToString())
		&& Error.ErrorMessage.Contains(TEXT("ReturnValue"))
		&& Error.ErrorMessage.Contains(TEXT("X"))
		&& Error.ErrorMessage.Contains(TEXT("split")));
	TestEqual(TEXT("the split-child refusal mutates nothing"), LiveGraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the split-child refusal leaves the node count"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestNull(TEXT("the split-child refusal leaves the destination graph"), FindNodeByGuidInGraph(DestinationGraph, Source->NodeGuid));

	// The rule is total, not link-specific: an expanded pin without any link is refused too, because
	// the clone path does not prove the expanded state either. A plain node still transfers, so the
	// refusal is not blanket.
	{
		UK2Node_CallFunction* UnlinkedSource = AddMakeVectorNode(SourceGraph, 0, 400);
		TestNotNull(TEXT("the unlinked expanded node exists"), SplitStructPinChildren(UnlinkedSource, TEXT("ReturnValue")));
		const TArray<FGuid> UnlinkedSelection = { UnlinkedSource->NodeGuid };
		TSharedPtr<FJsonObject> UnlinkedRequest = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000121902"), CopyOp, SourceGraph, UnlinkedSelection, DestinationGraph, {});
		FCortexGraphPreparedPatch UnlinkedPrepared;
		FCortexCommandResult UnlinkedError;
		TestFalse(TEXT("an unlinked expanded pin is refused under the same rule"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, UnlinkedRequest, UnlinkedPrepared, UnlinkedError));
		TestTrue(FString::Printf(TEXT("the unlinked refusal names the expanded pin [%s]"), *UnlinkedError.ErrorMessage),
			UnlinkedError.ErrorMessage.Contains(TEXT("ReturnValue")) && UnlinkedError.ErrorMessage.Contains(TEXT("split")));

		UK2Node_CallFunction* PlainSource = AddPrintNode(SourceGraph, TEXT("plain"), 0, 800);
		const TArray<FGuid> PlainSelection = { PlainSource->NodeGuid };
		TSharedPtr<FJsonObject> PlainRequest = TransferRequest(Fixture.Blueprint,
			TEXT("00000000-0000-0000-0000-000000121903"), CopyOp, SourceGraph, PlainSelection, DestinationGraph, {});
		FCortexGraphPreparedPatch PlainPrepared;
		FCortexGraphMigrationTransferPlan PlainPlan;
		FCortexCommandResult PlainError;
		TestTrue(FString::Printf(TEXT("a node without expanded pins still transfers: %s"), *PlainError.ErrorMessage),
			PreviewPlan(Fixture.Blueprint, PlainRequest, PlainPrepared, PlainPlan, PlainError));
	}

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 20. FunctionResultBoundary (F7)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationTransferFunctionResultBoundaryTest,
	"Cortex.Graph.Authoring.Migration.Transfer.FunctionResultBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationTransferFunctionResultBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphMigrationTransferTest;
	ClearFaults();

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_TransferResultBoundary_T12")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	FTerminatorFunctionGraph Source = AddComputeScoreFunctionGraph(Fixture.Blueprint, TEXT("ComputeScore"));
	TestNotNull(TEXT("the source function result terminator exists"), Source.Result);
	UK2Node_CallFunction* Producer = AddIntAddNode(Source.Graph, 0, 0);
	UEdGraphPin* ResultInput = Source.Result ? Source.Result->FindPin(TEXT("ReturnValue")) : nullptr;
	TestNotNull(TEXT("the function result owns a return value input"), ResultInput);
	TestTrue(TEXT("the selected output feeds the function result boundary"),
		ResultInput && LinkPins(Source.Graph, Producer->FindPin(TEXT("ReturnValue")), ResultInput));

	UEdGraph* DestinationGraph = AddVoidFunctionGraph(Fixture.Blueprint, TEXT("CortexResultBoundaryTarget"));
	UK2Node_CallFunction* DestinationConsumer = AddIntMultiplyNode(DestinationGraph, 500, 0);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);

	const TArray<FGuid> Selection = { Producer->NodeGuid };
	TArray<TSharedPtr<FJsonValue>> Boundary;
	Boundary.Add(MakeShared<FJsonValueObject>(BoundaryEntry(Producer, TEXT("ReturnValue"), DestinationConsumer, TEXT("A"))));
	TSharedPtr<FJsonObject> Request = TransferRequest(Fixture.Blueprint,
		TEXT("00000000-0000-0000-0000-000000122001"), MoveOp, Source.Graph, Selection, DestinationGraph, Boundary);
	FCortexGraphPreparedPatch Prepared;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("function-result boundary previews: %s"), *Error.ErrorMessage),
		PreviewPlan(Fixture.Blueprint, Request, Prepared, Plan, Error));
	TestEqual(TEXT("the function-result crossing needs exactly one boundary entry"), Plan.Boundary.Num(), 1);
	if (Plan.Boundary.Num() == 1)
	{
		TestEqual(TEXT("the boundary entry names the source function result as the covered crossing edge"),
			Plan.Boundary[0].SourceFarGuid, Source.Result ? Source.Result->NodeGuid.ToString() : FString());
		TestEqual(TEXT("the boundary entry names the result return value pin"),
			Plan.Boundary[0].SourceFarPin, FString(TEXT("ReturnValue")));
	}

	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("function-result boundary transfer applies: %s [%s]"), *Error.ErrorMessage, *JoinDiagnostics(Outcome.Diagnostics)),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("function-result boundary readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("function-result boundary compiles once"), Outcome.CompileStatus, FString(TEXT("compiled")));
	UEdGraphNode* Moved = FindNodeByGuidInGraph(DestinationGraph, Producer->NodeGuid);
	TestNotNull(TEXT("the moved producer resolves in the destination"), Moved);
	if (Moved)
	{
		TestTrue(TEXT("the moved producer output is wired to the destination boundary pin"),
			NodesLinked(Moved, TEXT("ReturnValue"), DestinationConsumer, TEXT("A")));
	}
	TestNull(TEXT("the moved producer is absent from the source graph"), FindNodeByGuidInGraph(Source.Graph, Producer->NodeGuid));
	if (Source.Result)
	{
		UEdGraphPin* LeftBehind = Source.Result->FindPin(TEXT("ReturnValue"));
		TestEqual(TEXT("the source function result keeps no dangling link"), LeftBehind ? LeftBehind->LinkedTo.Num() : -1, 0);
	}

	Fixture.Cleanup();
	return true;
}
#endif
