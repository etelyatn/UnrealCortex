#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "GameFramework/GameMode.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "PackageTools.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Explicit persistence (R1-R3) and deterministic-identity retry reconciliation (R4-R6) coverage.
 *
 * Every case pins the persistence outcome independently of the in-memory apply outcome, and every
 * replay case reconciles through real engine state instead of a receipt: the deterministic node
 * identity is derived from the patch GUID plus the client id, checked against canonical intent, and
 * any partially or conflicting identity set is refused rather than repaired.
 */
namespace CortexGraphPatchPersistenceTest
{
/** Observations around real coordinator operations, installed per test. */
struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;
	TArray<FString> SavedPackages;

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
			[](const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
			{
				if (!Active) return;
				++Active->Saves;
				if (SavedPackage) Active->SavedPackages.Add(SavedPackage->GetName());
			});
	}

	void End()
	{
		UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

	TArray<FString> SortedSavedPackages() const
	{
		TArray<FString> Sorted = SavedPackages;
		Sorted.Sort();
		return Sorted;
	}

private:
	static FOperations* Active;
	FDelegateHandle SaveHandle;
};

FOperations* FOperations::Active = nullptr;

/** Clears every injected coordinator fault so no case can leak into the next one. */
static void ClearFaults()
{
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
}

static void ResetTransaction()
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchPersistenceTestCleanup")));
	}
}

static TArray<uint8> ReadFileBytes(const FString& Filename)
{
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Filename);
	return Bytes;
}

/** Byte-for-byte comparison; a missing or empty file never compares equal. */
static bool SameBytes(const TArray<uint8>& Left, const TArray<uint8>& Right)
{
	return Left.Num() > 0 && Left.Num() == Right.Num()
		&& FMemory::Memcmp(Left.GetData(), Right.GetData(), Left.Num()) == 0;
}

/** Deletes one fixture file; returns false when a still-locked file could not be removed. */
static bool DeleteFixtureFile(const FString& Filename)
{
	if (Filename.IsEmpty()) return true;
	return IFileManager::Get().Delete(*Filename, false, true, true);
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
			ParentClass ? ParentClass : AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
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
	}
};

static TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint, const TCHAR* PatchId)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Blueprint->UbergraphPages[0]->GraphGuid.ToString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

static TSharedPtr<FJsonObject> AddNode(
	const TSharedPtr<FJsonObject>& Request,
	const TCHAR* ClientId,
	const TCHAR* NodeClass,
	const TSharedPtr<FJsonObject>& Params = nullptr,
	const TSharedPtr<FJsonObject>& Defaults = nullptr)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), ClientId);
	Node->SetStringField(TEXT("node_class"), NodeClass);
	if (Params.IsValid()) Node->SetObjectField(TEXT("params"), Params);
	if (Defaults.IsValid()) Node->SetObjectField(TEXT("defaults"), Defaults);
	TArray<TSharedPtr<FJsonValue>> Nodes = Request->GetArrayField(TEXT("nodes"));
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	return Node;
}

/** Adds authored layout to a node object that is already part of a request. */
static void SetPosition(const TSharedPtr<FJsonObject>& Node, const int32 X, const int32 Y)
{
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), X);
	Position->SetNumberField(TEXT("y"), Y);
	Node->SetObjectField(TEXT("position"), Position);
}

static void AddConnection(
	const TSharedPtr<FJsonObject>& Request,
	const TFunction<void(TSharedPtr<FJsonObject>&)>& FromSetup,
	const TFunction<void(TSharedPtr<FJsonObject>&)>& ToSetup)
{
	TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	FromSetup(From);
	ToSetup(To);
	Connection->SetObjectField(TEXT("from"), From);
	Connection->SetObjectField(TEXT("to"), To);
	TArray<TSharedPtr<FJsonValue>> Connections = Request->GetArrayField(TEXT("connections"));
	Connections.Add(MakeShared<FJsonValueObject>(Connection));
	Request->SetArrayField(TEXT("connections"), Connections);
}

static TSharedPtr<FJsonObject> IntLiteral(const int64 Value)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("int"));
	Literal->SetNumberField(TEXT("value"), Value);
	return Literal;
}

static TSharedPtr<FJsonObject> BoolLiteral(const bool Value)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("bool"));
	Literal->SetBoolField(TEXT("value"), Value);
	return Literal;
}

static TSharedPtr<FJsonObject> ConvertParams()
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("function_name"), TEXT("KismetStringLibrary.Conv_IntToString"));
	return Params;
}

static TSharedPtr<FJsonObject> PrintParams()
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	return Params;
}

/** The canonical planned tagged default of the conversion node. */
static TSharedPtr<FJsonObject> ConvertDefaultLiteral() { return IntLiteral(42); }

/** The canonical planned tagged default of the print node. */
static TSharedPtr<FJsonObject> NoteDefaultLiteral() { return BoolLiteral(false); }

/**
 * Canonical planned intent: an overridden ReceiveBeginPlay implementation entry, a print call
 * reached from that entry, a conversion call feeding the print input, tagged defaults and authored
 * layout. Every planned node is reachable from the entry, so the target compile keeps the planned
 * wiring intact instead of pruning it.
 */
static TSharedPtr<FJsonObject> IntentRequest(UBlueprint* Blueprint, const TCHAR* PatchId)
{
	TSharedPtr<FJsonObject> Request = BaseRequest(Blueprint, PatchId);
	Request->RemoveField(TEXT("target"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);

	TSharedPtr<FJsonObject> ConvertDefaults = MakeShared<FJsonObject>();
	ConvertDefaults->SetObjectField(TEXT("InInt"), ConvertDefaultLiteral());
	TSharedPtr<FJsonObject> Convert = AddNode(Request, TEXT("convert"), TEXT("CallFunction"), ConvertParams(), ConvertDefaults);
	SetPosition(Convert, 240, 160);

	TSharedPtr<FJsonObject> NoteDefaults = MakeShared<FJsonObject>();
	NoteDefaults->SetObjectField(TEXT("bPrintToScreen"), NoteDefaultLiteral());
	TSharedPtr<FJsonObject> Note = AddNode(Request, TEXT("note"), TEXT("CallFunction"), PrintParams(), NoteDefaults);
	SetPosition(Note, 560, 160);

	AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From) { From->SetBoolField(TEXT("entry"), true); From->SetStringField(TEXT("pin"), TEXT("then")); },
		[](TSharedPtr<FJsonObject>& To) { To->SetStringField(TEXT("client_id"), TEXT("note")); To->SetStringField(TEXT("pin"), TEXT("execute")); });
	AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From) { From->SetStringField(TEXT("client_id"), TEXT("convert")); From->SetStringField(TEXT("pin"), TEXT("ReturnValue")); },
		[](TSharedPtr<FJsonObject>& To) { To->SetStringField(TEXT("client_id"), TEXT("note")); To->SetStringField(TEXT("pin"), TEXT("InString")); });
	return Request;
}

/** Runs a preview and converts the request into an apply request carrying its validation token. */
static bool PreviewForApply(
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

/** Applies one prepared request while deliberately discarding the coordinator result object. */
static bool ApplyDiscardingOutcome(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexCommandResult& OutError)
{
	FCortexGraphPatchOutcome Discarded;
	return FCortexGraphPatchOps::Execute(Blueprint, Request, Discarded, OutError);
}

static FString FingerprintHash(const TSharedPtr<FJsonObject>& Fingerprint)
{
	FString Hash;
	if (Fingerprint.IsValid()) Fingerprint->TryGetStringField(TEXT("graph_authoring_hash"), Hash);
	return Hash;
}

static FString LiveGraphHash(UBlueprint* Blueprint)
{
	return FingerprintHash(FCortexGraphPatchState::ComputeFingerprint(Blueprint));
}

static int32 CountNativeNodes(UBlueprint* Blueprint)
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

static UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) return Node;
		}
	}
	return nullptr;
}

static int32 CountNodesWithGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	int32 Count = 0;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			Count += Node && Node->NodeGuid == NodeGuid ? 1 : 0;
		}
	}
	return Count;
}

static FGuid FindClientGuid(const FCortexGraphPreparedPatch& Prepared, const TCHAR* ClientId)
{
	const FGuid* Guid = Prepared.NodeGuidByClientId.Find(ClientId);
	return Guid ? *Guid : FGuid();
}

/** True when the fixture already overrides the named inherited event in its ubergraph. */
static bool UbergraphOverridesEvent(UBlueprint* Blueprint, const TCHAR* FunctionName)
{
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event && Event->EventReference.GetMemberName() == FName(FunctionName)) return true;
		}
	}
	return false;
}

/**
 * Nodes the canonical intent adds to a fixture: the two planned nodes plus the implementation entry
 * when the fixture does not already carry the inherited event override. Must be evaluated before
 * the patch runs.
 */
static int32 ExpectedIntentNodeDelta(UBlueprint* Blueprint)
{
	return 2 + (UbergraphOverridesEvent(Blueprint, TEXT("ReceiveBeginPlay")) ? 0 : 1);
}

/** Canonical native capture of one node: class, layout, symbol and every pin's stored value. */
static FString CaptureNodeState(const UEdGraphNode* Node)
{
	if (!Node) return TEXT("<none>");
	FString Capture = FString::Printf(TEXT("class=%s pos=(%d,%d)"),
		*Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY);
	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
	{
		const UClass* Owner = Call->FunctionReference.GetMemberParentClass();
		Capture += FString::Printf(TEXT(" symbol=%s@%s"),
			*Call->FunctionReference.GetMemberName().ToString(),
			Owner ? *Owner->GetPathName() : TEXT("none"));
	}
	TArray<FString> Pins;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pins.Add(FString::Printf(TEXT("%s=%s/%s/%d"), *Pin->PinName.ToString(),
				*Pin->DefaultValue, *Pin->DefaultTextValue.ToString(), Pin->LinkedTo.Num()));
		}
	}
	Pins.Sort();
	Capture += FString::Printf(TEXT(" pins=[%s]"), *FString::Join(Pins, TEXT(",")));
	return Capture;
}

/** Resolves one planned call node and proves its deterministic identity survived a reload. */
static UK2Node_CallFunction* FindPlannedCallNode(
	FAutomationTestBase& Test,
	UBlueprint* Blueprint,
	const FString& ClientId,
	const FGuid& DerivedGuid,
	const TCHAR* ExpectedFunctionName)
{
	Test.TestTrue(FString::Printf(TEXT("%s: planned identity is a valid node GUID"), *ClientId), DerivedGuid.IsValid());
	UEdGraphNode* Node = FindNodeByGuid(Blueprint, DerivedGuid);
	Test.TestNotNull(FString::Printf(TEXT("%s: deterministic node GUID resolves"), *ClientId), Node);
	if (!Node) return nullptr;
	Test.TestEqual(FString::Printf(TEXT("%s: deterministic node GUID is unique"), *ClientId),
		CountNodesWithGuid(Blueprint, DerivedGuid), 1);
	UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
	Test.TestNotNull(FString::Printf(TEXT("%s: node class survived"), *ClientId), Call);
	if (!Call) return nullptr;
	Test.TestEqual(FString::Printf(TEXT("%s: canonical node class survived"), *ClientId),
		Call->GetClass()->GetPathName(), FString(UK2Node_CallFunction::StaticClass()->GetPathName()));
	Test.TestEqual(FString::Printf(TEXT("%s: resolved symbol survived"), *ClientId),
		Call->FunctionReference.GetMemberName().ToString(), FString(ExpectedFunctionName));
	return Call;
}

/** Proves one native pin still holds the canonical planned tagged default. */
static void CheckDefaultIdentity(
	FAutomationTestBase& Test,
	const UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& PlannedLiteral,
	const TCHAR* Context)
{
	Test.TestNotNull(FString::Printf(TEXT("%s: pin resolves"), Context), Pin);
	if (!Pin) return;
	FString Expected;
	FString Actual;
	FString Failure;
	const bool bComparable = FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, PlannedLiteral, Expected, Actual, Failure);
	Test.TestTrue(FString::Printf(TEXT("%s: planned default stays comparable [%s]"), Context, *Failure), bComparable);
	Test.TestEqual(FString::Printf(TEXT("%s: canonical default identity"), Context), Actual, Expected);
}

/** Canonical JSON form of one descriptor field, used for exact token-by-token comparison. */
static FString FrozenJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return FString();
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
	return Out;
}

/**
 * Proves the canonical reader identity of one reloaded default: ReadDefault must report the planned
 * kind and the planned value identity, so a client that only reads defaults through the public
 * reader still sees the applied intent after the asset has been reloaded from disk.
 */
static void CheckDefaultDescriptorIdentity(
	FAutomationTestBase& Test,
	const UEdGraphPin* Pin,
	const TSharedPtr<FJsonObject>& PlannedLiteral,
	const TCHAR* Context)
{
	TSharedPtr<FJsonObject> Descriptor;
	FCortexCommandResult Error;
	Test.TestTrue(FString::Printf(TEXT("%s: ReadDefault succeeds [%s]"), Context, *Error.ErrorMessage),
		FCortexGraphPinDefaults::ReadDefault(Pin, Descriptor, Error));
	if (!Descriptor.IsValid()) return;
	Test.TestEqual(FString::Printf(TEXT("%s: canonical default kind"), Context),
		Descriptor->GetStringField(TEXT("kind")), PlannedLiteral->GetStringField(TEXT("kind")));
	Test.TestEqual(FString::Printf(TEXT("%s: canonical default value identity"), Context),
		FrozenJsonValue(Descriptor->TryGetField(TEXT("value"))),
		FrozenJsonValue(PlannedLiteral->TryGetField(TEXT("value"))));
}

/** Parent call owned by the graph of one implementation entry node, if any. */
static UK2Node_CallParentFunction* FindParentCall(UEdGraphNode* EntryNode)
{
	if (!EntryNode || !EntryNode->GetGraph()) return nullptr;
	for (UEdGraphNode* Node : EntryNode->GetGraph()->Nodes)
	{
		if (UK2Node_CallParentFunction* Parent = Cast<UK2Node_CallParentFunction>(Node))
		{
			return Parent;
		}
	}
	return nullptr;
}

/** Proves one planned edge is exactly the native link, single-linked. */
static void CheckEdge(
	FAutomationTestBase& Test,
	UEdGraphPin* SourcePin,
	UEdGraphPin* TargetPin,
	const TCHAR* Context)
{
	Test.TestNotNull(FString::Printf(TEXT("%s: source pin resolves"), Context), SourcePin);
	Test.TestNotNull(FString::Printf(TEXT("%s: target pin resolves"), Context), TargetPin);
	if (!SourcePin || !TargetPin) return;
	Test.TestTrue(FString::Printf(TEXT("%s: native link is exactly the planned edge"), Context),
		SourcePin->LinkedTo.Contains(TargetPin));
	Test.TestEqual(FString::Printf(TEXT("%s: planned input stays single-linked"), Context),
		TargetPin->LinkedTo.Num(), 1);
}
}

// ---------------------------------------------------------------------------
// 1. save=false never touches the file, the package or the engine save path
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceSaveFalseKeepsBytesTest,
	"Cortex.Graph.Authoring.Persistence.SaveFalseKeepsBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceSaveFalseKeepsBytesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchPersistSaveFalse_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("baseline file really exists with content"), BaselineBytes.Num() > 0);
	TestFalse(TEXT("baseline save leaves a clean package"), Fixture.Package->IsDirty());
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const int32 ExpectedDelta = ExpectedIntentNodeDelta(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0901"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save=false preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("save=false patch applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("save=false reports the real apply outcome"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("save=false never claims a save"), Outcome.SaveStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("save=false never claims post-save verification"), Outcome.PostSaveStatus, FString(TEXT("not_requested")));
	TestFalse(TEXT("save=false keeps bSaved false"), Outcome.bSaved);
	TestEqual(TEXT("save=false performs zero engine save events"), Operations.Saves, 0);
	TestEqual(TEXT("save=false performs one target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("save=false reuses nothing"), Outcome.ReusedClientIds.Num(), 0);
	TestEqual(TEXT("save=false added exactly the planned nodes and entry"),
		CountNativeNodes(Fixture.Blueprint), NodesBefore + ExpectedDelta);
	TestTrue(TEXT("save=false leaves the package dirty"), Fixture.Package->IsDirty());
	TestFalse(TEXT("save=false reports the pre-apply dirty state"), Outcome.bDirtyBefore);
	TestTrue(TEXT("save=false reports the post-apply dirty state"), Outcome.bDirtyAfter);
	TestTrue(TEXT("save=false reports a pre-apply fingerprint"), Outcome.FingerprintBefore.IsValid());
	TestTrue(TEXT("save=false reports a post-apply fingerprint"), Outcome.FingerprintAfter.IsValid());
	TestNotEqual(TEXT("save=false changes the authoring fingerprint"),
		FingerprintHash(Outcome.FingerprintAfter), FingerprintHash(Outcome.FingerprintBefore));
	TestEqual(TEXT("save=false reports the live post-apply fingerprint"),
		FingerprintHash(Outcome.FingerprintAfter), LiveGraphHash(Fixture.Blueprint));
	TestTrue(TEXT("save=false keeps the file bytes identical"),
		SameBytes(ReadFileBytes(Fixture.Filename), BaselineBytes));

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 2. save=true commits once, then the saved fixture carries canonical identity
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceSaveSuccessAndReloadTest,
	"Cortex.Graph.Authoring.Persistence.SaveSuccessAndReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceSaveSuccessAndReloadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchPersistSaveSuccess_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("baseline file really exists with content"), BaselineBytes.Num() > 0);
	TestFalse(TEXT("baseline save leaves a clean package"), Fixture.Package->IsDirty());
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const int32 ExpectedDelta = ExpectedIntentNodeDelta(Fixture.Blueprint);
	const FString TargetPackageName = Fixture.Package->GetName();

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0902"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save=true preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	// Persistence is independently validated, so a save=true apply may consume a save=false preview.
	Request->SetBoolField(TEXT("save"), true);
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("save=true patch applies and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error));
	Operations.End();

	TestEqual(TEXT("save=true reports the real apply outcome"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("save=true readback is authoritative"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("save=true reports a save"), Outcome.SaveStatus, FString(TEXT("saved")));
	TestTrue(TEXT("save=true sets bSaved"), Outcome.bSaved);
	TestEqual(TEXT("save=true verifies post-save persistence"), Outcome.PostSaveStatus, FString(TEXT("verified")));
	TestEqual(TEXT("save=true never rolls back"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("save=true performs exactly one engine save event"), Operations.Saves, 1);
	TArray<FString> ExpectedSavedPackages;
	ExpectedSavedPackages.Add(TargetPackageName);
	TestEqual(TEXT("save=true saves only the target package"),
		FString::Join(Operations.SortedSavedPackages(), TEXT(",")),
		FString::Join(ExpectedSavedPackages, TEXT(",")));
	TestFalse(TEXT("save=true leaves the package clean"), Fixture.Package->IsDirty());
	TestFalse(TEXT("save=true reports the clean pre-apply package"), Outcome.bDirtyBefore);
	TestFalse(TEXT("save=true reports the clean post-save package"), Outcome.bDirtyAfter);
	TestNotEqual(TEXT("save=true changes the authoring fingerprint"),
		FingerprintHash(Outcome.FingerprintAfter), FingerprintHash(Outcome.FingerprintBefore));
	TestEqual(TEXT("post-save fingerprint reports the saved state"),
		FingerprintHash(Outcome.FingerprintAfter), LiveGraphHash(Fixture.Blueprint));
	TestEqual(TEXT("save=true added exactly the planned nodes and entry"),
		CountNativeNodes(Fixture.Blueprint), NodesBefore + ExpectedDelta);
	const TArray<uint8> AppliedBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("saved file exists on disk"), AppliedBytes.Num() > 0);
	TestFalse(TEXT("saved file really contains the applied patch"),
		SameBytes(AppliedBytes, BaselineBytes));

	// R7: reload the saved fixture through a real engine reload and prove canonical identity.
	const FString ObjectName = Fixture.Blueprint->GetName();
	const FString PackageName = Fixture.Package->GetName();
	UBlueprint* const BeforeReload = Fixture.Blueprint;
	const FGuid EntryGuidBeforeReload = Outcome.Locators.EntryNodeGuid;
	const FGuid ConvertGuid = FindClientGuid(Prepared, TEXT("convert"));
	const FGuid NoteGuid = FindClientGuid(Prepared, TEXT("note"));
	TArray<UPackage*> PackagesToReload;
	PackagesToReload.Add(BeforeReload->GetOutermost());
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
	TestTrue(FString::Printf(TEXT("engine reload of the saved fixture succeeds: %s"), *ReloadError.ToString()), bReloaded);
	UPackage* ReloadedPackage = FindPackage(nullptr, *PackageName);
	UBlueprint* Reloaded = ReloadedPackage ? FindObject<UBlueprint>(ReloadedPackage, *ObjectName) : nullptr;
	TestNotNull(TEXT("reloaded Blueprint resolves from disk"), Reloaded);
	if (Reloaded)
	{
		TestTrue(TEXT("reloaded Blueprint is a different UObject instance"), Reloaded != BeforeReload);
		TestEqual(TEXT("reloaded Blueprint carries the same node count without duplicates"),
			CountNativeNodes(Reloaded), NodesBefore + ExpectedDelta);
		UK2Node_CallFunction* ReloadedConvert = FindPlannedCallNode(
			*this, Reloaded, TEXT("convert"), ConvertGuid, TEXT("Conv_IntToString"));
		UK2Node_CallFunction* ReloadedNote = FindPlannedCallNode(
			*this, Reloaded, TEXT("note"), NoteGuid, TEXT("PrintString"));
		if (ReloadedConvert)
		{
			TestEqual(TEXT("reloaded convert node keeps its planned layout"), ReloadedConvert->NodePosX, 240);
			TestEqual(TEXT("reloaded convert node keeps its planned Y"), ReloadedConvert->NodePosY, 160);
			CheckDefaultIdentity(*this, ReloadedConvert->FindPin(TEXT("InInt")),
				ConvertDefaultLiteral(), TEXT("reloaded convert default"));
			CheckDefaultDescriptorIdentity(*this, ReloadedConvert->FindPin(TEXT("InInt")),
				ConvertDefaultLiteral(), TEXT("reloaded convert default"));
		}
		if (ReloadedNote)
		{
			TestEqual(TEXT("reloaded note node keeps its planned layout"), ReloadedNote->NodePosX, 560);
			TestEqual(TEXT("reloaded note node keeps its planned Y"), ReloadedNote->NodePosY, 160);
			CheckDefaultIdentity(*this, ReloadedNote->FindPin(TEXT("bPrintToScreen")),
				NoteDefaultLiteral(), TEXT("reloaded note default"));
			CheckDefaultDescriptorIdentity(*this, ReloadedNote->FindPin(TEXT("bPrintToScreen")),
				NoteDefaultLiteral(), TEXT("reloaded note default"));
		}
		if (ReloadedConvert && ReloadedNote)
		{
			CheckEdge(*this, ReloadedConvert->FindPin(TEXT("ReturnValue")),
				ReloadedNote->FindPin(TEXT("InString")), TEXT("reloaded planned data edge"));
		}
		if (ReloadedConvert && ReloadedNote)
		{
			UEdGraphNode* ReloadedEntry = FindNodeByGuid(Reloaded, EntryGuidBeforeReload);
			TestNotNull(TEXT("reloaded implementation entry resolves by its locator"), ReloadedEntry);
			const UK2Node_Event* ReloadedEvent = Cast<UK2Node_Event>(ReloadedEntry);
			TestNotNull(TEXT("reloaded entry is still an event node"), ReloadedEvent);
			if (ReloadedEvent)
			{
				TestEqual(TEXT("reloaded entry keeps its canonical symbol"),
					ReloadedEvent->EventReference.GetMemberName().ToString(), FString(TEXT("ReceiveBeginPlay")));
			}
			CheckEdge(*this, ReloadedEntry ? ReloadedEntry->FindPin(TEXT("then")) : nullptr,
				ReloadedNote->FindPin(TEXT("execute")), TEXT("reloaded planned exec edge"));
		}
	}

	ResetTransaction();
	if (ReloadedPackage)
	{
		ReloadedPackage->ClearFlags(RF_Standalone);
		ReloadedPackage->MarkAsGarbage();
		// The reloaded package keeps a mapped linker open, which holds the fixture file locked on
		// Windows, so detach it and collect before the file can be removed.
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
// 3. A save failure preserves the verified in-memory outcome
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceSaveFailureTest,
	"Cortex.Graph.Authoring.Persistence.SaveFailurePreservesVerifiedOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceSaveFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchPersistSaveFailure_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("baseline file really exists with content"), BaselineBytes.Num() > 0);

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0903"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("save-failure preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	// Persistence is independently validated, so a save=true apply may consume a save=false preview.
	Request->SetBoolField(TEXT("save"), true);
	FCortexGraphPatchOps::SetSaveFaultForTesting(true);
	FCortexGraphPatchOutcome Outcome;
	const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	Operations.End();

	TestFalse(TEXT("an injected save failure fails the patch"), bApplied);
	TestEqual(TEXT("save failure reports the save error code"), Error.ErrorCode, CortexErrorCodes::SaveFailed);
	TestEqual(TEXT("save failure preserves the verified memory outcome"),
		Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("save failure keeps the authoritative readback"),
		Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("save failure is not persisted success"), Outcome.SaveStatus, FString(TEXT("failed")));
	TestEqual(TEXT("save failure never claims post-save verification"),
		Outcome.PostSaveStatus, FString(TEXT("not_requested")));
	TestFalse(TEXT("save failure keeps bSaved false"), Outcome.bSaved);
	TestNotEqual(TEXT("save failure is never reported as a rollback"),
		Outcome.RollbackStatus, FString(TEXT("restored")));
	TestEqual(TEXT("save failure performs zero real save events"), Operations.Saves, 0);
	TestTrue(TEXT("save failure keeps the package dirty"), Fixture.Package->IsDirty());
	TestTrue(TEXT("save failure reports the applied in-memory state"), Outcome.bDirtyAfter);
	TestEqual(TEXT("save failure keeps the in-memory fingerprint applied"),
		FingerprintHash(Outcome.FingerprintAfter), LiveGraphHash(Fixture.Blueprint));
	TestTrue(TEXT("save failure keeps the file untouched"),
		SameBytes(ReadFileBytes(Fixture.Filename), BaselineBytes));
	TestFalse(TEXT("save failure does not block the asset"), Outcome.bBlocked);
	FString BlockReason;
	TestFalse(TEXT("save failure leaves the asset mutable"),
		FCortexAssetMutationGuard::IsBlocked(Fixture.Blueprint, BlockReason));
	TestTrue(TEXT("save failure keeps the asset inspectable"),
		FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint).IsValid());
	TestNotNull(TEXT("save failure keeps the applied nodes inspectable"),
		FindNodeByGuid(Fixture.Blueprint, FindClientGuid(Prepared, TEXT("note"))));

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4. A post-save verification failure never rolls back a committed file
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistencePostSaveVerificationTest,
	"Cortex.Graph.Authoring.Persistence.PostSaveVerificationFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistencePostSaveVerificationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchPersistPostSave_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("baseline file really exists with content"), BaselineBytes.Num() > 0);

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0904"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("post-save case preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	// Persistence is independently validated, so a save=true apply may consume a save=false preview.
	Request->SetBoolField(TEXT("save"), true);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(TEXT("asset_file"));
	FCortexGraphPatchOutcome Outcome;
	const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
	Operations.End();

	TestFalse(TEXT("a failed post-save verification fails the patch"), bApplied);
	TestEqual(TEXT("post-save verification failure reports the verification error code"),
		Error.ErrorCode, CortexErrorCodes::VerificationFailed);
	TestEqual(TEXT("post-save failure still reports the real save"), Outcome.SaveStatus, FString(TEXT("saved")));
	TestTrue(TEXT("post-save failure keeps bSaved true"), Outcome.bSaved);
	TestEqual(TEXT("post-save failure reports the failed verification"),
		Outcome.PostSaveStatus, FString(TEXT("failed")));
	TestEqual(TEXT("post-save failure preserves the applied outcome"),
		Outcome.ApplyStatus, FString(TEXT("applied")));
	TestNotEqual(TEXT("post-save failure is never reported as a rollback"),
		Outcome.RollbackStatus, FString(TEXT("restored")));
	TestEqual(TEXT("post-save failure performs exactly one engine save event"), Operations.Saves, 1);
	TestFalse(TEXT("post-save failure does not block the asset"), Outcome.bBlocked);
	FString BlockReason;
	TestFalse(TEXT("post-save failure leaves the asset mutable"),
		FCortexAssetMutationGuard::IsBlocked(Fixture.Blueprint, BlockReason));
	TestTrue(TEXT("post-save failure names the failed check"),
		Error.ErrorMessage.Contains(TEXT("asset_file")));
	TestTrue(TEXT("post-save failure carries the reopen guidance"),
		Error.ErrorMessage.Contains(TEXT("reopened before further authoring")));
	TestTrue(TEXT("post-save failure reports the same diagnostic in the outcome"),
		FString::Join(Outcome.Diagnostics, TEXT(" | ")).Contains(TEXT("asset_file")));
	TestFalse(TEXT("post-save failure keeps the committed package clean"), Fixture.Package->IsDirty());
	TestFalse(TEXT("post-save failure keeps the committed file on disk"),
		SameBytes(ReadFileBytes(Fixture.Filename), BaselineBytes));

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 5. save=true refuses a dirty start and never saves unrelated dirty work
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceDirtyStartTest,
	"Cortex.Graph.Authoring.Persistence.DirtyStartSaveRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceDirtyStartTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	// (a) an unrelated dirty edit on the target package refuses save=true before any side effect
	{
		FFixture Fixture;
		TestTrue(TEXT("dirty-start fixture created"), Fixture.Create(TEXT("BP_PatchPersistDirtyStart_T09")));
		if (Fixture.Blueprint)
		{
			TestTrue(TEXT("dirty-start baseline saved to disk"), Fixture.SaveToDisk());
			const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
			TestTrue(TEXT("dirty-start baseline file exists with content"), BaselineBytes.Num() > 0);
			TestFalse(TEXT("dirty-start baseline leaves a clean package"), Fixture.Package->IsDirty());

			// An unrelated authored edit (not part of the patch) dirties the target package.
			Fixture.Blueprint->GetOutermost()->SetDirtyFlag(true);
			const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
			const FString HashBefore = LiveGraphHash(Fixture.Blueprint);

			TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0905"));
			FOperations Operations;
			Operations.Begin();
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("dirty-start preview succeeds: %s"), *Error.ErrorMessage),
				PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
			// The dirty starting package must be refused before the apply request reaches any phase.
			Request->SetBoolField(TEXT("save"), true);
			FCortexGraphPatchOutcome Outcome;
			const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
			Operations.End();

			TestFalse(TEXT("a dirty starting package refuses save=true"), bApplied);
			TestEqual(TEXT("dirty start reports the editor dirty state"),
				Error.ErrorCode, CortexErrorCodes::DirtyEditorState);
			TestEqual(TEXT("dirty start applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("dirty start compiles nothing"), Operations.TargetCompiles, 0);
			TestEqual(TEXT("dirty start saves nothing"), Operations.Saves, 0);
			TestEqual(TEXT("dirty start leaves the node count untouched"),
				CountNativeNodes(Fixture.Blueprint), NodesBefore);
			TestEqual(TEXT("dirty start leaves the fingerprint untouched"),
				LiveGraphHash(Fixture.Blueprint), HashBefore);
			TestTrue(TEXT("dirty start leaves the package dirty"), Fixture.Package->IsDirty());
			TestTrue(TEXT("dirty start leaves the file bytes untouched"),
				SameBytes(ReadFileBytes(Fixture.Filename), BaselineBytes));
			TestEqual(TEXT("dirty start reports the real dirty before-state"), Outcome.bDirtyBefore, true);
			TestEqual(TEXT("dirty start reports the real dirty after-state"), Outcome.bDirtyAfter, true);

			DeleteFixtureFile(Fixture.Filename);
			Fixture.Cleanup();
		}
		else
		{
			Fixture.Cleanup();
		}
	}

	// (b) saving the target never saves an unrelated dirty package
	{
		FFixture Target;
		FFixture Unrelated;
		TestTrue(TEXT("save-isolation target fixture created"), Target.Create(TEXT("BP_PatchPersistSaveIsolation_T09")));
		TestTrue(TEXT("save-isolation unrelated fixture created"), Unrelated.Create(TEXT("BP_PatchPersistSaveIsolationOther_T09")));
		if (Target.Blueprint && Unrelated.Blueprint)
		{
			TestTrue(TEXT("save-isolation target baseline saved"), Target.SaveToDisk());
			TestTrue(TEXT("save-isolation unrelated baseline saved"), Unrelated.SaveToDisk());
			const TArray<uint8> UnrelatedBytes = ReadFileBytes(Unrelated.Filename);
			TestTrue(TEXT("save-isolation unrelated file exists with content"), UnrelatedBytes.Num() > 0);
			Unrelated.Package->SetDirtyFlag(true);
			TestTrue(TEXT("save-isolation unrelated package starts dirty"), Unrelated.Package->IsDirty());

			TSharedPtr<FJsonObject> Request = IntentRequest(Target.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0906"));
			FOperations Operations;
			Operations.Begin();
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("save-isolation preview succeeds: %s"), *Error.ErrorMessage),
				PreviewForApply(Target.Blueprint, Request, Prepared, Error));
			Request->SetBoolField(TEXT("save"), true);
			FCortexGraphPatchOutcome Outcome;
			TestTrue(FString::Printf(TEXT("save-isolation patch applies and saves: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Execute(Target.Blueprint, Request, Outcome, Error));
			Operations.End();

			TestEqual(TEXT("save-isolation reports the save"), Outcome.SaveStatus, FString(TEXT("saved")));
			TestEqual(TEXT("save-isolation performs exactly one engine save event"), Operations.Saves, 1);
			TArray<FString> ExpectedSavedPackages;
			ExpectedSavedPackages.Add(Target.Package->GetName());
			TestEqual(TEXT("save-isolation saves only the target package"),
				FString::Join(Operations.SortedSavedPackages(), TEXT(",")),
				FString::Join(ExpectedSavedPackages, TEXT(",")));
			TestTrue(TEXT("save-isolation leaves the unrelated package dirty"), Unrelated.Package->IsDirty());
			TestTrue(TEXT("save-isolation leaves the unrelated file bytes untouched"),
				SameBytes(ReadFileBytes(Unrelated.Filename), UnrelatedBytes));
			TestFalse(TEXT("save-isolation leaves the target package clean"), Target.Package->IsDirty());
		}
		DeleteFixtureFile(Target.Filename);
		DeleteFixtureFile(Unrelated.Filename);
		Target.Cleanup();
		Unrelated.Cleanup();
	}
	return true;
}

// ---------------------------------------------------------------------------
// 6. A stale replay never bypasses the guard, even with completed identities
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceStaleRetryTest,
	"Cortex.Graph.Authoring.Persistence.StaleRetryRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceStaleRetryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("stale-retry fixture created"), Fixture.Create(TEXT("BP_PatchPersistStaleRetry_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	const int32 NodesBeforeFirst = CountNativeNodes(Fixture.Blueprint);
	const int32 ExpectedDelta = ExpectedIntentNodeDelta(Fixture.Blueprint);

	// The first application completes and verifies entirely in memory.
	TSharedPtr<FJsonObject> First = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0907"));
	const TSharedPtr<FJsonObject>* PreApplyFingerprint = nullptr;
	First->TryGetObjectField(TEXT("expected_fingerprint"), PreApplyFingerprint);
	TestNotNull(TEXT("pre-apply fingerprint captured"), PreApplyFingerprint ? PreApplyFingerprint->Get() : nullptr);
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("stale-retry first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	const FString PreApplyToken = FirstPrepared.ValidationHash;
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("stale-retry first apply succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, First, FirstOutcome, Error));
	TestEqual(TEXT("stale-retry first apply is a real apply"),
		FirstOutcome.ApplyStatus, FString(TEXT("applied")));
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);
	TestEqual(TEXT("stale-retry first apply added the planned nodes and entry"),
		NodesAfterFirst, NodesBeforeFirst + ExpectedDelta);
	const FGuid NoteGuid = FindClientGuid(FirstPrepared, TEXT("note"));
	TestTrue(TEXT("stale-retry first apply used the deterministic identity"),
		FindNodeByGuid(Fixture.Blueprint, NoteGuid) != nullptr);
	const FString LiveHashAfterFirst = LiveGraphHash(Fixture.Blueprint);

	// The replay carries the pre-apply fingerprint and token even though the identities prove a
	// completed prior apply; it must be refused before any mutation, compile or save.
	TSharedPtr<FJsonObject> Replay = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0907"));
	Replay->SetObjectField(TEXT("expected_fingerprint"), *PreApplyFingerprint);
	Replay->SetBoolField(TEXT("dry_run"), false);
	Replay->SetStringField(TEXT("expected_validation_hash"), PreApplyToken);
	const int32 TransactionsBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome ReplayOutcome;
	const bool bReplayed = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, Error);
	Operations.End();

	TestFalse(TEXT("a stale replay is refused"), bReplayed);
	TestEqual(TEXT("a stale replay reports a stale precondition"),
		Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestEqual(TEXT("a stale replay performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("a stale replay performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("a stale replay applies nothing"),
		ReplayOutcome.ApplyStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("a stale replay never claims a save"),
		ReplayOutcome.SaveStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("a stale replay does not duplicate nodes"),
		CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);
	TestEqual(TEXT("a stale replay keeps the deterministic identity unique"),
		CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 1);
	TestEqual(TEXT("a stale replay opens no transaction"),
		(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionsBefore);
	TestEqual(TEXT("a stale replay leaves the authoring state untouched"),
		LiveGraphHash(Fixture.Blueprint), LiveHashAfterFirst);
	TestEqual(TEXT("a stale replay reports the real live after-fingerprint"),
		FingerprintHash(ReplayOutcome.FingerprintAfter), LiveHashAfterFirst);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 7. A complete replay is an idempotent no-op that needs no allow_noop
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceCompleteReuseTest,
	"Cortex.Graph.Authoring.Persistence.CompleteReuseNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceCompleteReuseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("reuse fixture created"), Fixture.Create(TEXT("BP_PatchPersistReuse_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("reuse baseline saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> SavedBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("reuse baseline file exists with content"), SavedBytes.Num() > 0);

	TSharedPtr<FJsonObject> First = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0908"));
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("reuse first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	// Persistence is independently validated, so a save=true apply may consume a save=false preview.
	First->SetBoolField(TEXT("save"), true);
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("reuse first apply saves and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, First, FirstOutcome, Error));
	TestEqual(TEXT("reuse first apply reports the save"),
		FirstOutcome.SaveStatus, FString(TEXT("saved")));
	TestFalse(TEXT("reuse first apply leaves a clean package"), Fixture.Package->IsDirty());
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);
	const FString HashAfterFirst = LiveGraphHash(Fixture.Blueprint);
	const TArray<uint8> BytesAfterFirst = ReadFileBytes(Fixture.Filename);

	// The replay carries the same patch and client ids with a fresh fingerprint and save=true: a
	// complete reuse match is an idempotent replay, not an empty request, so it needs no allow_noop.
	TSharedPtr<FJsonObject> Replay = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0908"));
	Replay->SetBoolField(TEXT("allow_noop"), false);
	const int32 TransactionsBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch ReplayPrepared;
	TestTrue(FString::Printf(TEXT("reuse replay preview succeeds without allow_noop: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Replay, ReplayPrepared, Error));
	// The replay still asks for persistence: a complete reuse must not save anything.
	Replay->SetBoolField(TEXT("save"), true);
	TestFalse(TEXT("reuse replay preflight reports no prospective change"), ReplayPrepared.bChanged);
	TestTrue(TEXT("reuse replay preflight reports a complete reuse match"), ReplayPrepared.bFullyReused);
	TArray<FString> ExpectedReused;
	ExpectedReused.Add(TEXT("convert"));
	ExpectedReused.Add(TEXT("note"));
	TestEqual(TEXT("reuse replay preflight reports the reused client ids"),
		FString::Join(ReplayPrepared.ReusedClientIds, TEXT(",")), FString::Join(ExpectedReused, TEXT(",")));
	TestEqual(TEXT("reuse replay preview compiles nothing"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("reuse replay preview saves nothing"), Operations.Saves, 0);
	TestEqual(TEXT("reuse replay preview leaves the nodes untouched"),
		CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);

	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("reuse replay applies as an idempotent no-op: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, Error));
	Operations.End();

	TestEqual(TEXT("reuse replay reports unchanged"),
		ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("reuse replay reports the reused client ids"),
		FString::Join(ReplayOutcome.ReusedClientIds, TEXT(",")), FString::Join(ExpectedReused, TEXT(",")));
	TestEqual(TEXT("reuse replay performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("reuse replay performs no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("reuse replay performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("reuse replay never claims a save"),
		ReplayOutcome.SaveStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("reuse replay never claims post-save verification"),
		ReplayOutcome.PostSaveStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("reuse replay does not duplicate nodes"),
		CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);
	TestEqual(TEXT("reuse replay opens no transaction"),
		(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionsBefore);
	TestEqual(TEXT("reuse replay leaves the fingerprint untouched"),
		LiveGraphHash(Fixture.Blueprint), HashAfterFirst);
	TestEqual(TEXT("reuse replay reports the untouched fingerprint"),
		FingerprintHash(ReplayOutcome.FingerprintAfter), HashAfterFirst);
	TestEqual(TEXT("reuse replay reports the clean package"),
		ReplayOutcome.bDirtyAfter, false);
	TestTrue(TEXT("reuse replay keeps the file bytes identical"),
		SameBytes(ReadFileBytes(Fixture.Filename), BytesAfterFirst));
	TestEqual(TEXT("reuse replay maps every client id to the deterministic GUID"),
		ReplayOutcome.Locators.NodeGuidByClientId.Num(), 2);
	const FGuid* ConvertLocator = ReplayOutcome.Locators.NodeGuidByClientId.Find(TEXT("convert"));
	const FGuid* NoteLocator = ReplayOutcome.Locators.NodeGuidByClientId.Find(TEXT("note"));
	TestTrue(TEXT("reuse replay locates the conversion node"),
		ConvertLocator != nullptr && *ConvertLocator == FindClientGuid(ReplayPrepared, TEXT("convert")));
	TestTrue(TEXT("reuse replay locates the print node"),
		NoteLocator != nullptr && *NoteLocator == FindClientGuid(ReplayPrepared, TEXT("note")));
	TestTrue(TEXT("reuse replay reports the implementation entry locator"),
		ReplayOutcome.Locators.bHasEntryNode && ReplayOutcome.Locators.EntryNodeGuid.IsValid());
	TestTrue(TEXT("reuse replay reuses the existing native nodes"),
		FindNodeByGuid(Fixture.Blueprint, FindClientGuid(ReplayPrepared, TEXT("note"))) != nullptr);

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 8. A partial identity set is refused instead of appended
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistencePartialIdentityTest,
	"Cortex.Graph.Authoring.Persistence.PartialIdentitySetFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistencePartialIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("partial-identity fixture created"), Fixture.Create(TEXT("BP_PatchPersistPartial_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}

	TSharedPtr<FJsonObject> First = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0909"));
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("partial-identity first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("partial-identity first apply succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, First, FirstOutcome, Error));
	const FGuid ConvertGuid = FindClientGuid(FirstPrepared, TEXT("convert"));
	const FGuid NoteGuid = FindClientGuid(FirstPrepared, TEXT("note"));
	TestNotNull(TEXT("partial-identity conversion node exists"), FindNodeByGuid(Fixture.Blueprint, ConvertGuid));

	// One previously created deterministic node disappears: the identity set becomes partial.
	UEdGraphNode* ConvertNode = FindNodeByGuid(Fixture.Blueprint, ConvertGuid);
	TestNotNull(TEXT("partial-identity conversion node exists"), ConvertNode);
	if (!ConvertNode)
	{
		Fixture.Cleanup();
		return false;
	}
	ConvertNode->DestroyNode();
	TestNull(TEXT("partial-identity conversion node was removed"), FindNodeByGuid(Fixture.Blueprint, ConvertGuid));
	const int32 NodesBeforeReplay = CountNativeNodes(Fixture.Blueprint);
	const FString HashBeforeReplay = LiveGraphHash(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Replay = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0909"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch ReplayPrepared;
	FCortexCommandResult ReplayError;
	FCortexGraphPatchOutcome ReplayOutcome;
	const bool bPrepared = FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Replay, ReplayPrepared, ReplayError);
	if (bPrepared)
	{
		Replay->SetBoolField(TEXT("dry_run"), false);
		Replay->SetStringField(TEXT("expected_validation_hash"), ReplayPrepared.ValidationHash);
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, ReplayError);
	}
	Operations.End();

	TestFalse(TEXT("a partial identity set is refused"), bPrepared);
	TestEqual(TEXT("a partial identity set reports an invalid operation"),
		ReplayError.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(FString::Printf(TEXT("a partial identity set names the reused identity [%s]"), *ReplayError.ErrorMessage),
		ReplayError.ErrorMessage.Contains(TEXT("note")));
	TestTrue(FString::Printf(TEXT("a partial identity set names the missing identity [%s]"), *ReplayError.ErrorMessage),
		ReplayError.ErrorMessage.Contains(TEXT("convert")));
	TestEqual(TEXT("a partial identity set appends nothing"),
		CountNativeNodes(Fixture.Blueprint), NodesBeforeReplay);
	TestEqual(TEXT("a partial identity set keeps the surviving identity unique"),
		CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 1);
	TestEqual(TEXT("a partial identity set creates no duplicate identity"),
		CountNodesWithGuid(Fixture.Blueprint, ConvertGuid), 0);
	TestEqual(TEXT("a partial identity set performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("a partial identity set performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("a partial identity set leaves the authoring state untouched"),
		LiveGraphHash(Fixture.Blueprint), HashBeforeReplay);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 9. A conflicting identity is refused and the user node stays untouched
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceConflictingIdentityTest,
	"Cortex.Graph.Authoring.Persistence.ConflictingIdentityFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceConflictingIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("conflict fixture created"), Fixture.Create(TEXT("BP_PatchPersistConflict_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}

	TSharedPtr<FJsonObject> First = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090a"));
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("conflict first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("conflict first apply succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, First, FirstOutcome, Error));
	const FGuid NoteGuid = FindClientGuid(FirstPrepared, TEXT("note"));
	UEdGraphNode* NoteNode = FindNodeByGuid(Fixture.Blueprint, NoteGuid);
	TestNotNull(TEXT("conflict print node exists"), NoteNode);
	if (!NoteNode)
	{
		Fixture.Cleanup();
		return false;
	}

	// The user relocates the node the deterministic identity resolves to: conflicting layout.
	NoteNode->NodePosX += 37;
	const FString ConflictCapture = CaptureNodeState(NoteNode);
	const int32 NodesBeforeReplay = CountNativeNodes(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Replay = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090a"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch ReplayPrepared;
	FCortexCommandResult ReplayError;
	FCortexGraphPatchOutcome ReplayOutcome;
	const bool bPrepared = FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Replay, ReplayPrepared, ReplayError);
	if (bPrepared)
	{
		Replay->SetBoolField(TEXT("dry_run"), false);
		Replay->SetStringField(TEXT("expected_validation_hash"), ReplayPrepared.ValidationHash);
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, ReplayError);
	}
	Operations.End();

	TestFalse(TEXT("a conflicting identity is refused"), bPrepared);
	TestEqual(TEXT("a conflicting identity reports an invalid operation"),
		ReplayError.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(FString::Printf(TEXT("a conflicting identity names the client id [%s]"), *ReplayError.ErrorMessage),
		ReplayError.ErrorMessage.Contains(TEXT("note")));
	TestTrue(FString::Printf(TEXT("a conflicting identity names the conflicting dimension [%s]"), *ReplayError.ErrorMessage),
		ReplayError.ErrorMessage.Contains(TEXT("position")));
	TestEqual(TEXT("a conflicting identity appends nothing"),
		CountNativeNodes(Fixture.Blueprint), NodesBeforeReplay);
	TestEqual(TEXT("a conflicting identity never duplicates the GUID"),
		CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 1);
	TestEqual(TEXT("a conflicting identity leaves the user node untouched"),
		CaptureNodeState(FindNodeByGuid(Fixture.Blueprint, NoteGuid)), ConflictCapture);
	TestEqual(TEXT("a conflicting identity performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("a conflicting identity performs no save"), Operations.Saves, 0);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 10. A lost response is reconciled by inspection and a fresh preview
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceLostResponseTest,
	"Cortex.Graph.Authoring.Persistence.LostResponseInspectionAndReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceLostResponseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("lost-response fixture created"), Fixture.Create(TEXT("BP_PatchPersistLostResponse_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}

	// The first command result is deliberately thrown away: the client never learned the outcome.
	TSharedPtr<FJsonObject> First = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090b"));
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("lost-response first preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	const bool bFirstApplied = ApplyDiscardingOutcome(Fixture.Blueprint, First, Error);
	TestTrue(FString::Printf(TEXT("lost-response first apply succeeds: %s"), *Error.ErrorMessage), bFirstApplied);
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);

	// Inspection instead of the lost response: the authoring context reports the current fingerprint.
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	TSharedPtr<FJsonObject> ContextParams = MakeShared<FJsonObject>();
	ContextParams->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	TSharedPtr<FJsonObject> ContextTarget = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ContextGraphRef = MakeShared<FJsonObject>();
	ContextGraphRef->SetStringField(TEXT("graph_guid"), Fixture.Blueprint->UbergraphPages[0]->GraphGuid.ToString());
	ContextGraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	ContextTarget->SetObjectField(TEXT("graph_ref"), ContextGraphRef);
	ContextParams->SetObjectField(TEXT("target"), ContextTarget);
	const FCortexCommandResult ContextResult = Router.Execute(TEXT("graph.get_authoring_context"), ContextParams);
	TestTrue(FString::Printf(TEXT("authoring context inspection succeeds: %s"), *ContextResult.ErrorMessage),
		ContextResult.bSuccess);
	FString InspectedHash;
	if (ContextResult.bSuccess && ContextResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* InspectedFingerprint = nullptr;
		if (ContextResult.Data->TryGetObjectField(TEXT("fingerprint"), InspectedFingerprint) && InspectedFingerprint)
		{
			(*InspectedFingerprint)->TryGetStringField(TEXT("graph_authoring_hash"), InspectedHash);
		}
	}
	TestEqual(TEXT("inspection reports the live fingerprint"), InspectedHash, LiveGraphHash(Fixture.Blueprint));

	// A fresh preview derives the same deterministic identities the lost response would have carried.
	TSharedPtr<FJsonObject> FreshPreview = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090b"));
	FCortexGraphPreparedPatch FreshPrepared;
	TestTrue(FString::Printf(TEXT("fresh inspection preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Fixture.Blueprint, FreshPreview, FreshPrepared, Error));
	TestFalse(TEXT("fresh inspection preview reports no prospective change"), FreshPrepared.bChanged);
	const FGuid NoteGuid = FindClientGuid(FreshPrepared, TEXT("note"));
	const FGuid ConvertGuid = FindClientGuid(FreshPrepared, TEXT("convert"));
	// The public node listing is the reconciliation read a client actually has: it must expose the
	// deterministic node_guid next to the unchanged name-based node_id.
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	ListParams->SetStringField(TEXT("graph_name"), Fixture.Blueprint->UbergraphPages[0]->GetName());
	const FCortexCommandResult ListResult = Router.Execute(TEXT("graph.get_subgraph"), ListParams);
	TestTrue(FString::Printf(TEXT("node listing inspection succeeds: %s"), *ListResult.ErrorMessage),
		ListResult.bSuccess);
	TSet<FString> ListedGuids;
	TSet<FString> ListedIds;
	if (ListResult.bSuccess && ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (ListResult.Data->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Nodes)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Entry.IsValid()) continue;
				FString NodeGuidText;
				FString NodeId;
				if (Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText)) ListedGuids.Add(NodeGuidText);
				if (Entry->TryGetStringField(TEXT("node_id"), NodeId)) ListedIds.Add(NodeId);
			}
		}
	}
	TestTrue(TEXT("node listing exposes the deterministic print identity"),
		ListedGuids.Contains(NoteGuid.ToString()));
	TestTrue(TEXT("node listing exposes the deterministic conversion identity"),
		ListedGuids.Contains(ConvertGuid.ToString()));
	// node_id keeps its existing meaning: the object name, unaffected by the additive node_guid.
	UEdGraphNode* NoteNode = FindNodeByGuid(Fixture.Blueprint, NoteGuid);
	TestNotNull(TEXT("the listed deterministic identity still resolves natively"), NoteNode);
	if (NoteNode)
	{
		TestTrue(TEXT("node listing keeps the name-based node_id"), ListedIds.Contains(NoteNode->GetName()));
	}

	// Re-preview with the fresh fingerprint and re-apply: an unchanged replay without duplicates.
	TSharedPtr<FJsonObject> Replay = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090b"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch ReplayPrepared;
	TestTrue(FString::Printf(TEXT("lost-response replay preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Replay, ReplayPrepared, Error));
	FCortexGraphPatchOutcome ReplayOutcome;
	TestTrue(FString::Printf(TEXT("lost-response replay applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, Error));
	Operations.End();

	TestEqual(TEXT("lost-response replay reports unchanged"),
		ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("lost-response replay performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("lost-response replay performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("lost-response replay does not duplicate nodes"),
		CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 11. Deterministic identities are reproducible and collision-checked
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceGuidIdentityTest,
	"Cortex.Graph.Authoring.Persistence.DeterministicGuidIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceGuidIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture First;
	FFixture Second;
	TestTrue(TEXT("identity fixture A created"), First.Create(TEXT("BP_PatchPersistIdentityA_T09")));
	TestTrue(TEXT("identity fixture B created"), Second.Create(TEXT("BP_PatchPersistIdentityB_T09")));
	if (!First.Blueprint || !Second.Blueprint)
	{
		First.Cleanup();
		Second.Cleanup();
		return false;
	}

	FCortexCommandResult Error;
	TSharedPtr<FJsonObject> SharedPatchA = IntentRequest(First.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090c"));
	FCortexGraphPreparedPatch PreparedA;
	TestTrue(FString::Printf(TEXT("identity preview on blueprint A succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(First.Blueprint, SharedPatchA, PreparedA, Error));
	TSharedPtr<FJsonObject> SharedPatchB = IntentRequest(Second.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090c"));
	FCortexGraphPreparedPatch PreparedB;
	TestTrue(FString::Printf(TEXT("identity preview on blueprint B succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Second.Blueprint, SharedPatchB, PreparedB, Error));

	const FGuid ConvertGuid = FindClientGuid(PreparedA, TEXT("convert"));
	const FGuid NoteGuid = FindClientGuid(PreparedA, TEXT("note"));
	TestTrue(TEXT("identity preview derives a valid conversion GUID"), ConvertGuid.IsValid());
	TestTrue(TEXT("identity preview derives a valid print GUID"), NoteGuid.IsValid());
	TestNotEqual(TEXT("two client ids never derive the same GUID"),
		ConvertGuid.ToString(), NoteGuid.ToString());
	TestEqual(TEXT("the same patch and client id derive the same GUID across assets"),
		FindClientGuid(PreparedB, TEXT("convert")).ToString(), ConvertGuid.ToString());
	TestEqual(TEXT("the same patch and client id derive the same print GUID across assets"),
		FindClientGuid(PreparedB, TEXT("note")).ToString(), NoteGuid.ToString());

	// A different client id and a different patch id must derive different identities.
	TSharedPtr<FJsonObject> OtherClientRequest = BaseRequest(First.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090c"));
	AddNode(OtherClientRequest, TEXT("alpha"), TEXT("CallFunction"), PrintParams());
	FCortexGraphPreparedPatch OtherClientPrepared;
	TestTrue(FString::Printf(TEXT("other client id preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(First.Blueprint, OtherClientRequest, OtherClientPrepared, Error));
	TestNotEqual(TEXT("a different client id derives a different GUID"),
		FindClientGuid(OtherClientPrepared, TEXT("alpha")).ToString(), ConvertGuid.ToString());

	TSharedPtr<FJsonObject> OtherPatchRequest = IntentRequest(First.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090d"));
	FCortexGraphPreparedPatch OtherPatchPrepared;
	TestTrue(FString::Printf(TEXT("other patch id preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(First.Blueprint, OtherPatchRequest, OtherPatchPrepared, Error));
	TestNotEqual(TEXT("a different patch id derives a different GUID"),
		FindClientGuid(OtherPatchPrepared, TEXT("convert")).ToString(), ConvertGuid.ToString());

	// Applying the patch creates exactly the derived identities, and the compile keeps them.
	TSharedPtr<FJsonObject> Apply = IntentRequest(First.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090c"));
	FCortexGraphPreparedPatch ApplyPrepared;
	TestTrue(FString::Printf(TEXT("identity apply preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(First.Blueprint, Apply, ApplyPrepared, Error));
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("identity patch applies and compiles: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(First.Blueprint, Apply, Outcome, Error));
	TestEqual(TEXT("identity patch compiles after apply"),
		Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("identity patch verifies after compile"),
		Outcome.ReadbackStatus, FString(TEXT("matched")));
	const FGuid AppliedConvertGuid = FindClientGuid(ApplyPrepared, TEXT("convert"));
	TestEqual(TEXT("the derived identity is reproducible across preview and apply"),
		AppliedConvertGuid.ToString(), ConvertGuid.ToString());
	UK2Node_CallFunction* AppliedConvert = FindPlannedCallNode(
		*this, First.Blueprint, TEXT("convert"), AppliedConvertGuid, TEXT("Conv_IntToString"));
	TestNotNull(TEXT("the compiled node keeps the derived identity"), AppliedConvert);

	// A derived identity that already exists in another graph of the same asset is refused. The
	// collision identity is derived on a fresh fixture so no earlier apply can affect the preview.
	FFixture Collision;
	TestTrue(TEXT("identity collision fixture created"), Collision.Create(TEXT("BP_PatchPersistIdentityC_T09")));
	if (!Collision.Blueprint)
	{
		First.Cleanup();
		Second.Cleanup();
		Collision.Cleanup();
		return false;
	}
	TSharedPtr<FJsonObject> CollisionPreviewRequest = IntentRequest(Collision.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090e"));
	FCortexGraphPreparedPatch CollisionPrepared;
	TestTrue(FString::Printf(TEXT("collision preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Collision.Blueprint, CollisionPreviewRequest, CollisionPrepared, Error));
	const FGuid CollidingGuid = FindClientGuid(CollisionPrepared, TEXT("convert"));
	TestTrue(TEXT("collision preview derives a valid GUID"), CollidingGuid.IsValid());

	UEdGraph* OtherGraph = FBlueprintEditorUtils::CreateNewGraph(
		Collision.Blueprint, TEXT("T09CollisionGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Collision.Blueprint, OtherGraph, false, nullptr);
	UK2Node_CallFunction* Occupant = NewObject<UK2Node_CallFunction>(OtherGraph);
	Occupant->NodeGuid = CollidingGuid;
	Occupant->FunctionReference.SetExternalMember(FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
	Occupant->AllocateDefaultPins();
	OtherGraph->AddNode(Occupant, false, false);
	TestNotNull(TEXT("cross-graph collision occupant created"), FindNodeByGuid(Collision.Blueprint, CollidingGuid));
	const int32 NodesBeforeCollision = CountNativeNodes(Collision.Blueprint);

	TSharedPtr<FJsonObject> CollisionRequest = IntentRequest(Collision.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090e"));
	FCortexGraphPreparedPatch CollisionApplyPrepared;
	FCortexCommandResult CollisionError;
	FCortexGraphPatchOutcome CollisionOutcome;
	const bool bCollisionPrepared = FCortexGraphPatchOps::Preflight(
		Collision.Blueprint, CollisionRequest, CollisionApplyPrepared, CollisionError);
	TestFalse(TEXT("a cross-graph identity collision is refused"), bCollisionPrepared);
	TestEqual(TEXT("a cross-graph identity collision reports an invalid operation"),
		CollisionError.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(FString::Printf(TEXT("a cross-graph collision names the colliding client id [%s]"), *CollisionError.ErrorMessage),
		CollisionError.ErrorMessage.Contains(TEXT("convert")));
	TestTrue(FString::Printf(TEXT("a cross-graph collision names the owning graph [%s]"), *CollisionError.ErrorMessage),
		CollisionError.ErrorMessage.Contains(TEXT("T09CollisionGraph")));
	TestEqual(TEXT("a cross-graph collision creates no duplicate-GUID node"),
		CountNodesWithGuid(Collision.Blueprint, CollidingGuid), 1);
	TestEqual(TEXT("a cross-graph collision appends nothing"),
		CountNativeNodes(Collision.Blueprint), NodesBeforeCollision);
	TestEqual(TEXT("a cross-graph collision applies nothing"),
		CollisionOutcome.ApplyStatus, FString(TEXT("not_requested")));

	First.Cleanup();
	Second.Cleanup();
	Collision.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 12. Persistence flag combinations behave exactly as specified
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceFlagCombinationTest,
	"Cortex.Graph.Authoring.Persistence.FlagCombinations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceFlagCombinationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("flag-combination fixture created"), Fixture.Create(TEXT("BP_PatchPersistFlags_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("flag-combination baseline saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("flag-combination baseline file exists with content"), BaselineBytes.Num() > 0);
	// (a) preview with save=true is refused
	{
		TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e090f"));
		Request->SetBoolField(TEXT("dry_run"), true);
		Request->SetBoolField(TEXT("save"), true);
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("preview with save=true is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("preview with save=true reports an invalid operation"),
			Error.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// (b) apply with compile=false and save=true is refused
	{
		TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0910"));
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetBoolField(TEXT("compile"), false);
		Request->SetBoolField(TEXT("save"), true);
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("apply compile=false with save=true is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("apply compile=false with save=true reports an invalid operation"),
			Error.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("apply compile=false with save=true names the compile requirement"),
			Error.ErrorMessage.Contains(TEXT("compile")));
	}

	// (c) a preview never compiles, while the same apply does compile exactly once
	{
		FFixture Compile;
		TestTrue(TEXT("compile-flag fixture created"), Compile.Create(TEXT("BP_PatchPersistFlagsCompile_T09")));
		if (!Compile.Blueprint)
		{
			Compile.Cleanup();
			DeleteFixtureFile(Fixture.Filename);
			Fixture.Cleanup();
			return false;
		}
		TSharedPtr<FJsonObject> Request = IntentRequest(Compile.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0911"));
		FOperations Operations;
		Operations.Begin();
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("compile=true preview succeeds: %s"), *Error.ErrorMessage),
			PreviewForApply(Compile.Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("a compile=true preview compiles nothing"), Operations.TargetCompiles, 0);
		TestEqual(TEXT("a compile=true preview saves nothing"), Operations.Saves, 0);
		FCortexGraphPatchOutcome Outcome;
		TestTrue(FString::Printf(TEXT("compile=true apply succeeds: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Execute(Compile.Blueprint, Request, Outcome, Error));
		Operations.End();
		TestEqual(TEXT("the matching apply compiles exactly once"), Operations.TargetCompiles, 1);
		Compile.Cleanup();
	}

	// (d) an apply save=true may consume the validation token of a save=false preview
	{
		FFixture Token;
		TestTrue(TEXT("token-reuse fixture created"), Token.Create(TEXT("BP_PatchPersistFlagsToken_T09")));
		if (!Token.Blueprint)
		{
			Token.Cleanup();
			DeleteFixtureFile(Fixture.Filename);
			Fixture.Cleanup();
			return false;
		}
		TestTrue(TEXT("token-reuse baseline saved to disk"), Token.SaveToDisk());
		const TArray<uint8> TokenBaselineBytes = ReadFileBytes(Token.Filename);
		TestTrue(TEXT("token-reuse baseline file exists with content"), TokenBaselineBytes.Num() > 0);
		TestFalse(TEXT("token-reuse baseline leaves a clean package"), Token.Package->IsDirty());

		TSharedPtr<FJsonObject> Request = IntentRequest(Token.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0912"));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("save=false preview succeeds: %s"), *Error.ErrorMessage),
			PreviewForApply(Token.Blueprint, Request, Prepared, Error));
		TestFalse(TEXT("the save=false preview records no save intent"), Prepared.bSave);
		Request->SetBoolField(TEXT("save"), true);
		FOperations Operations;
		Operations.Begin();
		FCortexGraphPatchOutcome Outcome;
		TestTrue(FString::Printf(TEXT("apply save=true consuming a save=false token succeeds: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Execute(Token.Blueprint, Request, Outcome, Error));
		Operations.End();
		TestEqual(TEXT("token reuse reaches a real save"), Outcome.SaveStatus, FString(TEXT("saved")));
		TestEqual(TEXT("token reuse verifies post-save persistence"),
			Outcome.PostSaveStatus, FString(TEXT("verified")));
		TestEqual(TEXT("token reuse performs exactly one engine save event"), Operations.Saves, 1);
		TestFalse(TEXT("token reuse leaves the package clean"), Token.Package->IsDirty());
		TestFalse(TEXT("token reuse really wrote the applied patch"),
			SameBytes(ReadFileBytes(Token.Filename), TokenBaselineBytes));
		DeleteFixtureFile(Token.Filename);
		Token.Cleanup();
	}

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 13. A save failure still reports the durable locators a client can reconcile
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceSaveFailureLocatorsTest,
	"Cortex.Graph.Authoring.Persistence.SaveFailureAfterApplyKeepsInspectableLocators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceSaveFailureLocatorsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("locator fixture created"), Fixture.Create(TEXT("BP_PatchPersistLocators_T09")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("locator baseline saved to disk"), Fixture.SaveToDisk());
	const TArray<uint8> BaselineBytes = ReadFileBytes(Fixture.Filename);
	TestTrue(TEXT("locator baseline file exists with content"), BaselineBytes.Num() > 0);
	const FGuid TargetGraphGuid = Fixture.Blueprint->UbergraphPages[0]->GraphGuid;

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0913"));
	FOperations Operations;
	Operations.Begin();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("locator preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Request, Prepared, Error));
	// Persistence is independently validated, so a save=true apply may consume a save=false preview.
	Request->SetBoolField(TEXT("save"), true);
	const FGuid ConvertGuid = FindClientGuid(Prepared, TEXT("convert"));
	const FGuid NoteGuid = FindClientGuid(Prepared, TEXT("note"));
	FCortexGraphPatchOps::SetSaveFaultForTesting(true);
	FCortexGraphPatchOutcome Outcome;
	const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.Blueprint, Request, Outcome, Error);
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	Operations.End();

	TestFalse(TEXT("the injected save failure fails the patch"), bApplied);
	TestEqual(TEXT("the injected save failure reports the save error code"),
		Error.ErrorCode, CortexErrorCodes::SaveFailed);
	TestEqual(TEXT("the failed save keeps the durable graph locator"),
		Outcome.Locators.GraphGuid.ToString(), TargetGraphGuid.ToString());
	TestTrue(TEXT("the failed save reports the durable entry locator"),
		Outcome.Locators.bHasEntryNode && Outcome.Locators.EntryNodeGuid.IsValid());
	TestEqual(TEXT("the failed save reports every planned client identity"),
		Outcome.Locators.NodeGuidByClientId.Num(), 2);
	const FGuid* ReportedConvert = Outcome.Locators.NodeGuidByClientId.Find(TEXT("convert"));
	const FGuid* ReportedNote = Outcome.Locators.NodeGuidByClientId.Find(TEXT("note"));
	TestTrue(TEXT("the failed save reports the deterministic conversion identity"),
		ReportedConvert != nullptr && *ReportedConvert == ConvertGuid);
	TestTrue(TEXT("the failed save reports the deterministic print identity"),
		ReportedNote != nullptr && *ReportedNote == NoteGuid);
	TestNotNull(TEXT("the reported conversion locator resolves natively"),
		FindNodeByGuid(Fixture.Blueprint, ConvertGuid));
	TestNotNull(TEXT("the reported print locator resolves natively"),
		FindNodeByGuid(Fixture.Blueprint, NoteGuid));
	TestEqual(TEXT("the failed save performs no real save event"), Operations.Saves, 0);
	TestTrue(TEXT("the failed save leaves the file untouched"),
		SameBytes(ReadFileBytes(Fixture.Filename), BaselineBytes));

	// A client that never learned the outcome reconciles by inspection: a fresh preview reuses the
	// residual identities instead of creating duplicates.
	TSharedPtr<FJsonObject> Reconcile = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0913"));
	FOperations ReconcileOperations;
	ReconcileOperations.Begin();
	FCortexGraphPreparedPatch ReconcilePrepared;
	TestTrue(FString::Printf(TEXT("post-failure reconciliation preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, Reconcile, ReconcilePrepared, Error));
	TestFalse(TEXT("post-failure reconciliation preview reports no prospective change"),
		ReconcilePrepared.bChanged);
	FCortexGraphPatchOutcome ReconcileOutcome;
	TestTrue(FString::Printf(TEXT("post-failure reconciliation applies as a no-op: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, Reconcile, ReconcileOutcome, Error));
	ReconcileOperations.End();
	TestEqual(TEXT("post-failure reconciliation reports unchanged"),
		ReconcileOutcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("post-failure reconciliation performs no compile"), ReconcileOperations.TargetCompiles, 0);
	TestEqual(TEXT("post-failure reconciliation performs no save"), ReconcileOperations.Saves, 0);
	TestEqual(TEXT("post-failure reconciliation keeps the deterministic identity unique"),
		CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 1);

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}


// ---------------------------------------------------------------------------
// 14. A parent-call implementation target is verified, not assumed
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPersistenceParentCallTest,
	"Cortex.Graph.Authoring.Persistence.ParentCallReuseVerified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPersistenceParentCallTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchPersistenceTest::ClearFaults();
	using namespace CortexGraphPatchPersistenceTest;

	FFixture Fixture;
	TestTrue(TEXT("parent-call fixture created"),
		Fixture.Create(TEXT("BP_PatchPersistParentCall_T09"), AGameMode::StaticClass()));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}

	// Intent: an explicit parent-call implementation plus one standalone planned node, so the replay
	// still carries planned identities while the implementation owns the parent call.
	auto MakeIntent = [&](const TCHAR* PatchId, const TCHAR* CallKind = TEXT("parent"))
	{
		TSharedPtr<FJsonObject> Request = BaseRequest(Fixture.Blueprint, PatchId);
		Request->RemoveField(TEXT("target"));
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
		Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.GameMode"));
		Implementation->SetStringField(TEXT("function_name"), TEXT("ReadyToStartMatch"));
		Implementation->SetStringField(TEXT("call_kind"), CallKind);
		Target->SetObjectField(TEXT("implementation"), Implementation);
		Request->SetObjectField(TEXT("target"), Target);

		TSharedPtr<FJsonObject> NoteDefaults = MakeShared<FJsonObject>();
		NoteDefaults->SetObjectField(TEXT("bPrintToScreen"), NoteDefaultLiteral());
		TSharedPtr<FJsonObject> Note = AddNode(Request, TEXT("note"), TEXT("CallFunction"), PrintParams(), NoteDefaults);
		SetPosition(Note, 480, 160);
		return Request;
	};

	TSharedPtr<FJsonObject> First = MakeIntent(TEXT("00000000-0000-0000-0000-0000000e0916"));
	FCortexGraphPreparedPatch FirstPrepared;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("parent-call preview succeeds: %s"), *Error.ErrorMessage),
		PreviewForApply(Fixture.Blueprint, First, FirstPrepared, Error));
	FCortexGraphPatchOutcome FirstOutcome;
	TestTrue(FString::Printf(TEXT("parent-call patch applies and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Fixture.Blueprint, First, FirstOutcome, Error));
	TestEqual(TEXT("parent-call patch compiles after apply"),
		FirstOutcome.CompileStatus, FString(TEXT("compiled")));
	TestTrue(TEXT("parent-call patch reports its implementation entry locator"),
		FirstOutcome.Locators.bHasEntryNode);
	const FGuid NoteGuid = FindClientGuid(FirstPrepared, TEXT("note"));
	TestNotNull(TEXT("parent-call planned node exists"), FindNodeByGuid(Fixture.Blueprint, NoteGuid));

	// The engine really created the parent call and the entry wiring the comparator must verify.
	UEdGraphNode* EntryNode = FindNodeByGuid(Fixture.Blueprint, FirstOutcome.Locators.EntryNodeGuid);
	TestNotNull(TEXT("parent-call entry locator resolves"), EntryNode);
	UK2Node_CallParentFunction* ParentNode = FindParentCall(EntryNode);
	TestNotNull(TEXT("the implementation owner created a parent call"), ParentNode);
	if (EntryNode && ParentNode)
	{
		UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* ParentExec = ParentNode->GetExecPin();
		TestTrue(TEXT("the entry feeds the parent call exec"),
			EntryThen && ParentExec && EntryThen->LinkedTo.Contains(ParentExec) && ParentExec->LinkedTo.Contains(EntryThen));
	}
	const int32 NodesAfterFirst = CountNativeNodes(Fixture.Blueprint);

	// An exact replay of a parent-call target is an idempotent no-op.
	{
		TSharedPtr<FJsonObject> Replay = MakeIntent(TEXT("00000000-0000-0000-0000-0000000e0916"));
		FOperations Operations;
		Operations.Begin();
		FCortexGraphPreparedPatch ReplayPrepared;
		TestTrue(FString::Printf(TEXT("parent-call replay preview succeeds: %s"), *Error.ErrorMessage),
			PreviewForApply(Fixture.Blueprint, Replay, ReplayPrepared, Error));
		TestFalse(TEXT("parent-call replay preflight reports no prospective change"), ReplayPrepared.bChanged);
		FCortexGraphPatchOutcome ReplayOutcome;
		TestTrue(FString::Printf(TEXT("parent-call replay applies as a no-op: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Execute(Fixture.Blueprint, Replay, ReplayOutcome, Error));
		Operations.End();
		TestEqual(TEXT("parent-call replay reports unchanged"),
			ReplayOutcome.ApplyStatus, FString(TEXT("unchanged")));
		TestEqual(TEXT("parent-call replay performs no compile"), Operations.TargetCompiles, 0);
		TestEqual(TEXT("parent-call replay performs no save"), Operations.Saves, 0);
		TestEqual(TEXT("parent-call replay does not duplicate nodes"),
			CountNativeNodes(Fixture.Blueprint), NodesAfterFirst);
	}

	// An identity owned by two graphs of the same asset is ambiguous even when the target graph holds
	// an exact match. The implementation's own function graph is scanned before graphs added later,
	// which is exactly the order in which a duplicate used to hide inside the target graph.
	{
		UEdGraph* DuplicateGraph = FBlueprintEditorUtils::CreateNewGraph(
			Fixture.Blueprint, TEXT("T09DuplicateParentGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Fixture.Blueprint, DuplicateGraph, false, nullptr);
		UEdGraphNode* OwnerNode = FindNodeByGuid(Fixture.Blueprint, NoteGuid);
		TestNotNull(TEXT("duplicate-identity target node exists"), OwnerNode);
		if (OwnerNode)
		{
			UK2Node_CallFunction* DuplicateNode = Cast<UK2Node_CallFunction>(
				StaticDuplicateObject(OwnerNode, DuplicateGraph));
			TestNotNull(TEXT("duplicate-identity node duplicated"), DuplicateNode);
			if (DuplicateNode)
			{
				DuplicateNode->NodeGuid = NoteGuid;
				// The duplicate's copied links are dropped locally so only its GUID duplicates.
				for (UEdGraphPin* Pin : DuplicateNode->Pins)
				{
					if (Pin) Pin->LinkedTo.Reset();
				}
				DuplicateGraph->AddNode(DuplicateNode, false, false);
			}
			TestEqual(TEXT("the deterministic identity is now owned by two graphs"),
				CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 2);

			TSharedPtr<FJsonObject> DuplicateReplay = MakeIntent(TEXT("00000000-0000-0000-0000-0000000e0916"));
			FCortexGraphPreparedPatch DuplicatePrepared;
			FCortexCommandResult DuplicateError;
			TestFalse(TEXT("an identity duplicated across two graphs is refused"),
				FCortexGraphPatchOps::Preflight(Fixture.Blueprint, DuplicateReplay, DuplicatePrepared, DuplicateError));
			TestEqual(TEXT("a duplicated identity reports an invalid operation"),
				DuplicateError.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestTrue(FString::Printf(TEXT("a duplicated identity names the client id [%s]"), *DuplicateError.ErrorMessage),
				DuplicateError.ErrorMessage.Contains(TEXT("note")));
			TestEqual(TEXT("a duplicated identity leaves both nodes in place"),
				CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 2);
			// Remove the duplicate again so the later wiring cases start from a unique identity.
			if (DuplicateNode) DuplicateNode->DestroyNode();
			TestEqual(TEXT("the duplicate is removed again"),
				CountNodesWithGuid(Fixture.Blueprint, NoteGuid), 1);
		}
	}

	// The duplicate block above added a graph, so the wiring cases snapshot the node set again.
	const int32 NodesBeforeWiring = CountNativeNodes(Fixture.Blueprint);

	// The parent call wiring is part of the implementation intent: a replay whose parent call is no
	// longer fed by the entry must not be reported as a complete replay.
	if (EntryNode && ParentNode)
	{
		UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* ParentExec = ParentNode->GetExecPin();
		if (EntryThen && ParentExec)
		{
			EntryThen->BreakLinkTo(ParentExec);
		}
		TSharedPtr<FJsonObject> BrokenWiring = MakeIntent(TEXT("00000000-0000-0000-0000-0000000e0916"));
		FCortexGraphPreparedPatch BrokenPrepared;
		FCortexCommandResult BrokenError;
		TestFalse(TEXT("a replay with broken parent-call wiring is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, BrokenWiring, BrokenPrepared, BrokenError));
		TestEqual(TEXT("broken parent-call wiring reports an invalid operation"),
			BrokenError.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(FString::Printf(TEXT("broken parent-call wiring names the parent call [%s]"), *BrokenError.ErrorMessage),
			BrokenError.ErrorMessage.Contains(TEXT("parent call")));
		TestEqual(TEXT("broken parent-call wiring appends nothing"),
			CountNativeNodes(Fixture.Blueprint), NodesBeforeWiring);
		if (EntryThen && ParentExec)
		{
			EntryNode->GetGraph()->GetSchema()->TryCreateConnection(EntryThen, ParentExec);
		}
	}

	// A missing parent call is not a complete replay either.
	if (ParentNode)
	{
		ParentNode->DestroyNode();
		TSharedPtr<FJsonObject> MissingParent = MakeIntent(TEXT("00000000-0000-0000-0000-0000000e0916"));
		FCortexGraphPreparedPatch MissingPrepared;
		FCortexCommandResult MissingError;
		TestFalse(TEXT("a replay with a missing parent call is refused"),
			FCortexGraphPatchOps::Preflight(Fixture.Blueprint, MissingParent, MissingPrepared, MissingError));
		TestTrue(FString::Printf(TEXT("a missing parent call names the parent call [%s]"), *MissingError.ErrorMessage),
			MissingError.ErrorMessage.Contains(TEXT("parent call")));
		TestTrue(TEXT("a missing parent call is reported as missing"),
			MissingError.ErrorMessage.Contains(TEXT("missing")));
	}

	// The resolver accepts the call-kind spelling case-insensitively, so the comparator has to decide
	// on that same contract: a "PARENT" replay whose parent call is gone is not a complete replay.
	{
		FFixture Spelled;
		TestTrue(TEXT("call-kind spelling fixture created"),
			Spelled.Create(TEXT("BP_PatchPersistParentSpelling_T09"), AGameMode::StaticClass()));
		if (!Spelled.Blueprint)
		{
			Spelled.Cleanup();
		}
		else
		{
			auto MakeSpelledIntent = [&](const TCHAR* CallKind)
			{
				TSharedPtr<FJsonObject> Request = BaseRequest(
					Spelled.Blueprint, TEXT("00000000-0000-0000-0000-0000000e0917"));
				Request->RemoveField(TEXT("target"));
				TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
				Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.GameMode"));
				Implementation->SetStringField(TEXT("function_name"), TEXT("ReadyToStartMatch"));
				Implementation->SetStringField(TEXT("call_kind"), CallKind);
				Target->SetObjectField(TEXT("implementation"), Implementation);
				Request->SetObjectField(TEXT("target"), Target);
				TSharedPtr<FJsonObject> NoteDefaults = MakeShared<FJsonObject>();
				NoteDefaults->SetObjectField(TEXT("bPrintToScreen"), NoteDefaultLiteral());
				TSharedPtr<FJsonObject> Note = AddNode(Request, TEXT("note"), TEXT("CallFunction"), PrintParams(), NoteDefaults);
				SetPosition(Note, 480, 160);
				return Request;
			};

			TSharedPtr<FJsonObject> UpperFirst = MakeSpelledIntent(TEXT("PARENT"));
			FCortexGraphPreparedPatch UpperPrepared;
			TestTrue(FString::Printf(TEXT("upper-case call kind preview succeeds: %s"), *Error.ErrorMessage),
				PreviewForApply(Spelled.Blueprint, UpperFirst, UpperPrepared, Error));
			FCortexGraphPatchOutcome UpperOutcome;
			TestTrue(FString::Printf(TEXT("upper-case call kind applies and verifies: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Execute(Spelled.Blueprint, UpperFirst, UpperOutcome, Error));
			TestEqual(TEXT("upper-case call kind compiles after apply"),
				UpperOutcome.CompileStatus, FString(TEXT("compiled")));
			UEdGraphNode* UpperEntry = FindNodeByGuid(Spelled.Blueprint, UpperOutcome.Locators.EntryNodeGuid);
			UK2Node_CallParentFunction* UpperParent = FindParentCall(UpperEntry);
			TestNotNull(TEXT("the upper-case call kind really created a parent call"), UpperParent);

			// Both accepted spellings must verify identically on an exact replay.
			TSharedPtr<FJsonObject> LowerReplay = MakeSpelledIntent(TEXT("parent"));
			FCortexGraphPreparedPatch LowerPrepared;
			FCortexCommandResult LowerError;
			TestTrue(FString::Printf(TEXT("lower-case call kind replay preview succeeds: %s"), *LowerError.ErrorMessage),
				FCortexGraphPatchOps::Preflight(Spelled.Blueprint, LowerReplay, LowerPrepared, LowerError));
			TestFalse(TEXT("a lower-case replay of an upper-case request reports no prospective change"),
				LowerPrepared.bChanged);

			if (UpperParent)
			{
				UpperParent->DestroyNode();
			}
			TestNull(TEXT("the parent call is removed again"), FindParentCall(UpperEntry));
			TSharedPtr<FJsonObject> MissingUpper = MakeSpelledIntent(TEXT("PARENT"));
			FCortexGraphPreparedPatch MissingUpperPrepared;
			FCortexCommandResult MissingUpperError;
			TestFalse(TEXT("an upper-case call kind replay with a missing parent call is refused"),
				FCortexGraphPatchOps::Preflight(Spelled.Blueprint, MissingUpper, MissingUpperPrepared, MissingUpperError));
			TestEqual(TEXT("an upper-case missing parent call reports an invalid operation"),
				MissingUpperError.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestTrue(FString::Printf(TEXT("an upper-case missing parent call names the parent call [%s]"), *MissingUpperError.ErrorMessage),
				MissingUpperError.ErrorMessage.Contains(TEXT("parent call")));
		}
		Spelled.Cleanup();
	}

	Fixture.Cleanup();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
