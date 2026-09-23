#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "CortexGraphTestContentRoot.h"
#include "CortexAssetMutationGuard.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "K2Node_GenericCreateObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"

#include "Curves/CurveFloat.h"
#include "Components/SceneComponent.h"
#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphPatchCompileTest
{
/** Observations around real coordinator operations, installed per test. */
struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;
	TFunction<void(FName, UBlueprint*)> BeforeOperation;

	void Begin()
	{
		TFunction<void(FName, UBlueprint*)> SavedCallback = MoveTemp(BeforeOperation);
		*this = FOperations();
		BeforeOperation = MoveTemp(SavedCallback);
		Active = this;
		FCortexGraphPatchOps::SetOperationObserverForTesting(
			[](const FName Operation, UBlueprint* Observed)
			{
				if (!Active) return;
				if (Active->BeforeOperation) Active->BeforeOperation(Operation, Observed);
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

static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint, const TCHAR* PatchId)
{
	EnsureCortexGraphTestTempContentRoot();
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

static TSharedPtr<FJsonObject> StringLiteral(const TCHAR* Value)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();

	Literal->SetStringField(TEXT("kind"), TEXT("string"));
	Literal->SetStringField(TEXT("value"), Value);
	return Literal;
}

static TSharedPtr<FJsonObject> ClassParams(const TCHAR* ClassPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class"), ClassPath);
	return Params;
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

static FString GraphHash(UBlueprint* Blueprint)
{
	const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
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

static void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchCompileTestCleanup")));
	}
	if (Blueprint)
	{
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
	}
}

/** Adds a named custom event to the fixture so generated state carries a stable symbol. */
static UK2Node_CustomEvent* AddNamedCustomEvent(UBlueprint* Blueprint, const TCHAR* Name)
{
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CreateNewGuid();
	Event->CustomFunctionName = FName(Name);
	Graph->AddNode(Event, true, false);
	Event->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return Event;
}

/** Implementation-target adapter patch: override an inherited event and wire one call. */
static TSharedPtr<FJsonObject> AdapterRequest(UBlueprint* Blueprint, const TCHAR* PatchId)
{
	TSharedPtr<FJsonObject> Request = BaseRequest(Blueprint, PatchId);
	Request->RemoveField(TEXT("target"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);

	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	Defaults->SetObjectField(TEXT("InString"), StringLiteral(TEXT("t08 adapter")));
	AddNode(Request, TEXT("note"), TEXT("CallFunction"), CallParams, Defaults);
	AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From) { From->SetBoolField(TEXT("entry"), true); From->SetStringField(TEXT("pin"), TEXT("then")); },
		[](TSharedPtr<FJsonObject>& To) { To->SetStringField(TEXT("client_id"), TEXT("note")); To->SetStringField(TEXT("pin"), TEXT("execute")); });
	return Request;
}

/** Implementation-target adapter chain: a conversion call feeding a print call. */
static TSharedPtr<FJsonObject> AdapterChainRequest(UBlueprint* Blueprint, const TCHAR* PatchId, bool bIncludeDefault)
{
	TSharedPtr<FJsonObject> Request = BaseRequest(Blueprint, PatchId);
	Request->RemoveField(TEXT("target"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);

	TSharedPtr<FJsonObject> ConvertParams = MakeShared<FJsonObject>();
	ConvertParams->SetStringField(TEXT("function_name"), TEXT("KismetStringLibrary.Conv_IntToString"));
	TSharedPtr<FJsonObject> ConvertDefaults;
	if (bIncludeDefault)
	{
		ConvertDefaults = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> IntLiteral = MakeShared<FJsonObject>();
		IntLiteral->SetStringField(TEXT("kind"), TEXT("int"));
		IntLiteral->SetNumberField(TEXT("value"), 42);
		ConvertDefaults->SetObjectField(TEXT("InInt"), IntLiteral);
	}
	AddNode(Request, TEXT("convert"), TEXT("CallFunction"), ConvertParams, ConvertDefaults);
	TSharedPtr<FJsonObject> NoteParams = MakeShared<FJsonObject>();
	NoteParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	AddNode(Request, TEXT("note"), TEXT("CallFunction"), NoteParams);
	AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From) { From->SetBoolField(TEXT("entry"), true); From->SetStringField(TEXT("pin"), TEXT("then")); },
		[](TSharedPtr<FJsonObject>& To) { To->SetStringField(TEXT("client_id"), TEXT("note")); To->SetStringField(TEXT("pin"), TEXT("execute")); });
	AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From) { From->SetStringField(TEXT("client_id"), TEXT("convert")); From->SetStringField(TEXT("pin"), TEXT("ReturnValue")); },
		[](TSharedPtr<FJsonObject>& To) { To->SetStringField(TEXT("client_id"), TEXT("note")); To->SetStringField(TEXT("pin"), TEXT("InString")); });
	return Request;
}

static UBlueprint* MakeBlueprintWithParent(UPackage*& OutPackage, const TCHAR* Name, UClass* ParentClass)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		ParentClass, OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

/** Adds a blueprint member variable marked as exposed on spawn, which class-driven pin rebuilds expose. */
static void AddExposeOnSpawnVariable(UBlueprint* Blueprint, const FName VariableName, const FEdGraphPinType& PinType)
{
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType);
	for (FBPVariableDescription& Description : Blueprint->NewVariables)
	{
		if (Description.VarName == VariableName)
		{
			Description.PropertyFlags &= ~CPF_DisableEditOnInstance;
			Description.PropertyFlags |= CPF_ExposeOnSpawn;
			break;
		}
	}
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(
		Blueprint, VariableName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
}

static UK2Node_CallFunction* AddCallNode(UEdGraph* Graph, UFunction* Function)
{
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(Function);
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	Graph->AddNode(Node, true, false);
	return Node;
}

static UK2Node_GenericCreateObject* AddGenericCreateObject(UEdGraph* Graph)
{
	UK2Node_GenericCreateObject* Node = NewObject<UK2Node_GenericCreateObject>(Graph);
	Node->CreateNewGuid();
	Graph->AddNode(Node, true, false);
	Node->AllocateDefaultPins();
	return Node;
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
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

static TArray<FString> SortedPinNames(UEdGraphNode* Node)
{
	TArray<FString> Names;
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) Names.Add(Pin->PinName.ToString());
		}
	}
	Names.Sort();
	return Names;
}

static TSharedPtr<FJsonObject> ClassLiteral(const TCHAR* ClassPath)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("class"));
	Literal->SetStringField(TEXT("path"), ClassPath);
	return Literal;
}

/** Appends one pin_updates entry that assigns a class default to an existing node's pin. */
static void AddClassPinUpdate(
	const TSharedPtr<FJsonObject>& Request,
	const FGuid& NodeGuid,
	const TCHAR* PinName,
	const TCHAR* ClassPath)
{
	TSharedPtr<FJsonObject> Update = MakeShared<FJsonObject>();
	Update->SetStringField(TEXT("node_guid"), NodeGuid.ToString());
	Update->SetStringField(TEXT("pin"), PinName);
	Update->SetObjectField(TEXT("default"), ClassLiteral(ClassPath));
	TArray<TSharedPtr<FJsonValue>> Updates = Request->HasField(TEXT("pin_updates"))
		? Request->GetArrayField(TEXT("pin_updates"))
		: TArray<TSharedPtr<FJsonValue>>();
	Updates.Add(MakeShared<FJsonValueObject>(Update));
	Request->SetArrayField(TEXT("pin_updates"), Updates);
}

/** Canonical class identity stored by a construct-object node's class pin. */
static FString ConstructedClassPath(UK2Node_GenericCreateObject* Node)
{
	UEdGraphPin* ClassPin = Node ? Node->GetClassPin() : nullptr;
	return ClassPin && ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : FString(TEXT("None"));
}
}

// ---------------------------------------------------------------------------
// 1. Working adapter patch with compile: one target compile, verified readback
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileAdapterTest,
	"Cortex.Graph.Authoring.Compile.AdapterPatchWithCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchCompileAdapter_T08"));
	TestNotNull(TEXT("adapter fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const EBlueprintStatus StatusBefore = Blueprint->Status;
	TestEqual(TEXT("adapter fixture starts compiled"), static_cast<int32>(StatusBefore), static_cast<int32>(BS_UpToDate));
	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::AdapterRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000108"));
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("adapter preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestEqual(TEXT("preview performs no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("preview performs no dependency/save write"), Operations.Saves, 0);
	TestEqual(TEXT("preview keeps compile status"), static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
	TestEqual(TEXT("preview keeps authoring hash"), CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);

	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("adapter patch applies and verifies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("one target compile for changed success"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("observed one real target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("no recovery compile on success"), Outcome.RecoveryCompileCount, 0);
	TestEqual(TEXT("observed no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("no save before verified result"), Operations.Saves, 0);
	TestFalse(TEXT("outcome does not claim a save"), Outcome.bSaved);
	TestEqual(TEXT("apply status is honest"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("compile status reports the real compile"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("readback is authoritative"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("no rollback on success"), Outcome.RollbackStatus, FString(TEXT("not_requested")));
	TestFalse(TEXT("verified success does not block the asset"), Outcome.bBlocked);
	FString BlockReason;
	TestFalse(TEXT("verified success leaves the asset mutable"),
		FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
	TestEqual(TEXT("compiled success is up to date"), static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));
	TestEqual(TEXT("compiled success added exactly the planned node"),
		CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore + 1);
	TestNotEqual(TEXT("compiled success changed authoring hash"), CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 2. Structurally valid patch that fails compilation: restore generated state
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileFailureTest,
	"Cortex.Graph.Authoring.Compile.CompileFailureRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchCompileFailure_T08"));
	TestNotNull(TEXT("compile-failure fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UK2Node_CustomEvent* ExistingEvent = CortexGraphPatchCompileTest::AddNamedCustomEvent(Blueprint, TEXT("T08PreexistingEvent"));
	TestNotNull(TEXT("pre-existing generated symbol fixture created"), ExistingEvent);
	const EBlueprintStatus StatusBefore = Blueprint->Status;
	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint);
	const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
	TestTrue(TEXT("pre-existing generated state is non-trivial"), GeneratedBefore.Contains(TEXT("T08PreexistingEvent")));

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000208"));
	CortexGraphPatchCompileTest::AddNode(
		Request, TEXT("badconstruct"), TEXT("GenericCreateObject"),
		CortexGraphPatchCompileTest::ClassParams(TEXT("/Script/Engine.CurveFloat")));
	CortexGraphPatchCompileTest::AddNode(Request, TEXT("compile_error"), TEXT("CustomEvent"));
	CortexGraphPatchCompileTest::AddConnection(Request,
		[](TSharedPtr<FJsonObject>& From)
		{
			From->SetStringField(TEXT("client_id"), TEXT("compile_error"));
			From->SetStringField(TEXT("pin"), TEXT("then"));
		},
		[](TSharedPtr<FJsonObject>& To)
		{
			To->SetStringField(TEXT("client_id"), TEXT("badconstruct"));
			To->SetStringField(TEXT("pin"), TEXT("execute"));
		});

	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.BeforeOperation = [](const FName Operation, UBlueprint* Target)
	{
		if (Operation != TEXT("target_compile") || !Target) return;
		// This is a real UE compiler-only failure: CustomEvent rejects shadowing a native
		// parent function during ValidateNodeDuringCompilation. The test hook configures the
		// newly added event immediately before the real target compile; it does not fake a result.
		TArray<UEdGraph*> Graphs;
		Target->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
				if (Event && Event->CustomFunctionName.IsNone())
				{
					Event->CustomFunctionName = FName(TEXT("ReceiveBeginPlay"));
					return;
				}
			}
		}
	};
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("compile-failure patch passes structural preflight: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

	AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("structurally valid patch that fails compilation is rejected"),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("apply phase did run"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("compile status reports the failure"), Outcome.CompileStatus, FString(TEXT("failed")));
	TestEqual(TEXT("failed attempt still counts one target compile"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("observed one real target compile attempt"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("recovery compilation is counted separately"), Outcome.RecoveryCompileCount, 1);
	TestEqual(TEXT("observed one real recovery compile"), Operations.RecoveryCompiles, 1);
	TestEqual(TEXT("restoration is verified after a compiler failure"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("verified restoration does not block the asset"), Outcome.bBlocked);
	TestTrue(TEXT("compiler diagnostics are preserved"), Outcome.Diagnostics.Num() > 0);
	TestTrue(TEXT("compiler diagnostics keep the engine message"),
		FString::Join(Outcome.Diagnostics, TEXT(" | ")).Contains(TEXT("name conflicts with a native")));
	TestEqual(TEXT("no save during a failed patch"), Operations.Saves, 0);
	TestFalse(TEXT("failed patch does not claim a save"), Outcome.bSaved);
	TestEqual(TEXT("compiler failure restores exact authoring state"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	TestEqual(TEXT("compiler failure restores exact generated state"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint), GeneratedBefore);
	TestEqual(TEXT("compiler failure restores compile status"), static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
	TestEqual(TEXT("compiler failure leaves no residual nodes"),
		CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 3. Injected readback mismatch per canonical dimension: rollback + recovery
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileReadbackTest,
	"Cortex.Graph.Authoring.Compile.ReadbackMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TCHAR* FaultFields[] = { TEXT("readback_class"), TEXT("readback_symbol"), TEXT("readback_default"), TEXT("readback_edge") };
	int32 FaultIndex = 0;
	for (const TCHAR* FaultField : FaultFields)
	{
		++FaultIndex;
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(
			Package, *FString::Printf(TEXT("BP_PatchReadback%02d_T08"), FaultIndex));
		TestNotNull(FString::Printf(TEXT("%s fixture Blueprint created"), FaultField), Blueprint);
		if (!Blueprint) continue;

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const EBlueprintStatus StatusBefore = Blueprint->Status;
		const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
		const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint);

		TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::AdapterChainRequest(
			Blueprint, *FString::Printf(TEXT("00000000-0000-0000-0000-0000000003%02d"), FaultIndex), true);

		CortexGraphPatchCompileTest::FOperations Operations;
		Operations.Begin();
		FCortexGraphPatchOps::SetReadbackFaultForTesting(FName(FaultField));

		FCortexGraphPreparedPatch Preview;
		FCortexCommandResult Error;
		const bool bPreviewReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error);
		TestTrue(FString::Printf(TEXT("%s: preview succeeds: %s"), FaultField, *Error.ErrorMessage), bPreviewReady);
		if (!bPreviewReady)
		{
			FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
			Operations.End();
			CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
			continue;
		}
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		FCortexGraphPatchOutcome Outcome;
		const bool bExecute = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
		FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
		TestFalse(FString::Printf(TEXT("%s: injected readback mismatch is rejected"), FaultField), bExecute);
		TestEqual(FString::Printf(TEXT("%s: readback reports a mismatch"), FaultField),
			Outcome.ReadbackStatus, FString(TEXT("mismatched")));
		TestEqual(FString::Printf(TEXT("%s: one target compile happened"), FaultField), Outcome.TargetCompileCount, 1);
		TestEqual(FString::Printf(TEXT("%s: recovery compilation is counted separately"), FaultField), Outcome.RecoveryCompileCount, 1);
		TestEqual(FString::Printf(TEXT("%s: observed recovery compile"), FaultField), Operations.RecoveryCompiles, 1);
		TestEqual(FString::Printf(TEXT("%s: restoration verified [%s]"), FaultField, *FString::Join(Outcome.Diagnostics, TEXT(" | "))), Outcome.RollbackStatus, FString(TEXT("restored")));
		TestFalse(FString::Printf(TEXT("%s: verified restoration does not block"), FaultField), Outcome.bBlocked);
		TestEqual(FString::Printf(TEXT("%s: readback mismatch restores authoring state"), FaultField),
			CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
		TestEqual(FString::Printf(TEXT("%s: readback mismatch restores generated state"), FaultField),
			FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint), GeneratedBefore);
		TestEqual(FString::Printf(TEXT("%s: readback mismatch restores compile status"), FaultField),
			static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
		Operations.End();
		CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 4. Preconditions that must fail before modification
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompilePreconditionTest,
	"Cortex.Graph.Authoring.Compile.Preconditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompilePreconditionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCommandResult Error;

	// (a) missing class context refuses before modification
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchPreconditionMissing_T08"));
		TestNotNull(TEXT("missing-context fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000401"));
			TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
			CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
			CortexGraphPatchCompileTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), CallParams);
			FCortexGraphPreparedPatch Preview;
			TestTrue(TEXT("missing-context preview succeeds"),
				FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
			Request->SetBoolField(TEXT("dry_run"), false);
			Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

			const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
			const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
			const int32 TransactionsBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
			UClass* GeneratedBefore = Blueprint->GeneratedClass;
			CortexGraphPatchCompileTest::FOperations Operations;
			Operations.Begin();
			Blueprint->GeneratedClass = nullptr;
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("missing class context refuses the patch"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			Blueprint->GeneratedClass = GeneratedBefore;
			TestEqual(TEXT("missing class context reports an invalid operation"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestEqual(TEXT("missing class context performs no compile"), Operations.TargetCompiles, 0);
			TestEqual(TEXT("missing class context applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("missing class context leaves nodes untouched"),
				CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
			TestEqual(TEXT("missing class context leaves authoring state untouched"),
				CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
			TestEqual(TEXT("missing class context opens no transaction"),
				(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionsBefore);
			Operations.End();
			CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) an active play/simulate session refuses before modification
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchPreconditionPIE_T08"));
		TestNotNull(TEXT("PIE fixture Blueprint created"), Blueprint);
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (Blueprint && EditorWorld)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000402"));
			TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
			CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
			CortexGraphPatchCompileTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), CallParams);
			FCortexGraphPreparedPatch Preview;
			TestTrue(TEXT("PIE preview succeeds"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
			Request->SetBoolField(TEXT("dry_run"), false);
			Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

			const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
			const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
			CortexGraphPatchCompileTest::FOperations Operations;
			Operations.Begin();
			UWorld* PlayWorldBefore = GEditor->PlayWorld;
			GEditor->PlayWorld = EditorWorld;
			FCortexGraphPatchOutcome Outcome;
			const bool bApplied = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
			GEditor->PlayWorld = PlayWorldBefore;
			TestFalse(TEXT("active play session refuses the patch"), bApplied);
			TestEqual(TEXT("active play session reports an invalid operation"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestEqual(TEXT("active play session performs no compile"), Operations.TargetCompiles, 0);
			TestEqual(TEXT("active play session applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("active play session leaves nodes untouched"),
				CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
			TestEqual(TEXT("active play session leaves authoring state untouched"),
				CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
			Operations.End();
		}
		else
		{
			TestNotNull(TEXT("editor world available for the PIE precondition case"), EditorWorld);
		}
		if (Blueprint) CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	}

	// (c) a pre-existing compiler error refuses before modification
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchPreconditionError_T08"));
		TestNotNull(TEXT("pre-existing-error fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			UEdGraph* Graph = Blueprint->UbergraphPages[0];
			UK2Node_CustomEvent* BadEvent = NewObject<UK2Node_CustomEvent>(Graph);
			BadEvent->CreateNewGuid();
			BadEvent->CustomFunctionName = FName(TEXT("ReceiveBeginPlay"));
			Graph->AddNode(BadEvent, true, false);
			BadEvent->AllocateDefaultPins();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			AddExpectedError(TEXT("name conflicts with a native"), EAutomationExpectedErrorFlags::Contains, 1);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			TestEqual(TEXT("fixture carries a pre-existing compiler error"),
				static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_Error));

			TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000403"));
			TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
			CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
			CortexGraphPatchCompileTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), CallParams);
			const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
			const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
			CortexGraphPatchCompileTest::FOperations Operations;
			Operations.Begin();
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("pre-existing compiler error refuses the patch"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			TestEqual(TEXT("pre-existing compiler error reports an invalid operation"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestEqual(TEXT("pre-existing compiler error performs no new compile"), Operations.TargetCompiles, 0);
			TestEqual(TEXT("pre-existing compiler error applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
			TestEqual(TEXT("pre-existing compiler error leaves nodes untouched"),
				CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
			TestEqual(TEXT("pre-existing compiler error leaves authoring state untouched"),
				CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
			Operations.End();
			CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		}
	}

	// (d) stale reflection between preview and apply refuses before modification
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchPreconditionStale_T08"));
		UPackage* ExternalPackage = nullptr;
		UBlueprint* External = CortexGraphPatchCompileTest::MakeBlueprint(ExternalPackage, TEXT("BP_PatchPreconditionStaleSource_T08"));
		TestNotNull(TEXT("stale fixture Blueprints created"), Blueprint);
		TestNotNull(TEXT("stale external fixture Blueprint created"), External);
		if (Blueprint && External)
		{
			UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
				External, TEXT("T08SignatureFixture"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(External, FunctionGraph, false, nullptr);
			GetDefault<UEdGraphSchema_K2>()->AddExtraFunctionFlags(FunctionGraph, FUNC_BlueprintCallable | FUNC_Public);
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* Node : FunctionGraph->Nodes)
			{
				Entry = Cast<UK2Node_FunctionEntry>(Node);
				if (Entry) break;
			}
			TestNotNull(TEXT("stale fixture external function entry created"), Entry);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External);
			FKismetEditorUtilities::CompileBlueprint(External);
			if (Entry)
			{
				TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000404"));
				TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
				CallParams->SetStringField(TEXT("owner_class"), External->GeneratedClass->GetPathName());
				CallParams->SetStringField(TEXT("function_name"), TEXT("T08SignatureFixture"));
				CortexGraphPatchCompileTest::AddNode(Request, TEXT("external"), TEXT("CallFunction"), CallParams);
				FCortexGraphPreparedPatch Preview;
				TestTrue(FString::Printf(TEXT("stale-reflection preview succeeds: %s"), *Error.ErrorMessage),
					FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
				const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
				const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);

				FEdGraphPinType AddedType;
				AddedType.PinCategory = UEdGraphSchema_K2::PC_Int;
				Entry->CreateUserDefinedPin(TEXT("DriftedT08"), AddedType, EGPD_Output, false);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External);
				FKismetEditorUtilities::CompileBlueprint(External);

				Request->SetBoolField(TEXT("dry_run"), false);
				Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
				CortexGraphPatchCompileTest::FOperations Operations;
				Operations.Begin();
				FCortexGraphPatchOutcome Outcome;
				TestFalse(TEXT("stale reflected symbol rejects the patch"),
					FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
				TestEqual(TEXT("stale reflection reports a stale precondition"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
				TestEqual(TEXT("stale reflection performs no compile"), Operations.TargetCompiles, 0);
				TestEqual(TEXT("stale reflection applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
				TestEqual(TEXT("stale reflection leaves nodes untouched"),
					CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
				TestEqual(TEXT("stale reflection leaves authoring state untouched"),
					CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
				Operations.End();
			}
		}
		if (Blueprint) CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		if (External) CortexGraphPatchCompileTest::Cleanup(ExternalPackage, External);
	}
	return true;
}

// ---------------------------------------------------------------------------
// 5. compile=false stays honestly uncompiled
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileUncompiledTest,
	"Cortex.Graph.Authoring.Compile.UncompiledWhenNotRequested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileUncompiledTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchUncompiled_T08"));
	TestNotNull(TEXT("uncompiled fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestEqual(TEXT("uncompiled fixture starts up to date"), static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000508"));
	Request->SetBoolField(TEXT("compile"), false);
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	CortexGraphPatchCompileTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), CallParams);
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("uncompiled patch preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("uncompiled patch applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("compile=false performs no target compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("observed no target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("compile=false reports compilation was not requested"),
		Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestNotEqual(TEXT("compile=false reports an uncompiled asset"),
		static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));
	TestEqual(TEXT("compile=false still performs authoritative readback"),
		Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("compile=false performs no recovery compile"), Operations.RecoveryCompiles, 0);
	TestEqual(TEXT("compile=false performs no save"), Operations.Saves, 0);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 6. A no-op request neither mutates nor compiles
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileNoOpTest,
	"Cortex.Graph.Authoring.Compile.NoOpDoesNotCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileNoOpTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchNoOp_T08"));
	TestNotNull(TEXT("no-op fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const EBlueprintStatus StatusBefore = Blueprint->Status;
	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const int32 TransactionsBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000608"));
	Request->SetBoolField(TEXT("allow_noop"), true);
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("no-op preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestFalse(TEXT("no-op preview reports no prospective change"), Preview.bChanged);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPatchOutcome Outcome;
	TestTrue(FString::Printf(TEXT("no-op apply succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	TestEqual(TEXT("no-op does not compile"), Outcome.TargetCompileCount, 0);
	TestEqual(TEXT("observed no compile for a no-op"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("no-op reports unchanged"), Outcome.ApplyStatus, FString(TEXT("unchanged")));
	TestEqual(TEXT("no-op reports a change-free compile status"), Outcome.CompileStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("no-op reports a change-free readback status"), Outcome.ReadbackStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("no-op leaves authoring state untouched"), CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	TestEqual(TEXT("no-op leaves compile status untouched"), static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
	TestEqual(TEXT("no-op opens no undo transaction"),
		(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionsBefore);
	TestEqual(TEXT("no-op performs no save"), Operations.Saves, 0);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 7. Recovery that cannot be verified blocks the asset
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileUnverifiedRecoveryTest,
	"Cortex.Graph.Authoring.Compile.UnverifiedRecoveryBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileUnverifiedRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchUnverifiedRecovery_T08"));
	TestNotNull(TEXT("unverified-recovery fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::AdapterRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000708"));
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(TEXT("readback_edge"));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("verification_failure"));

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("unverified-recovery preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("unverifiable recovery fails the patch"),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	TestEqual(TEXT("unverifiable recovery reports the unverified rollback"),
		Outcome.RollbackStatus, FString(TEXT("unverified")));
	TestTrue(TEXT("unverifiable recovery blocks the asset"), Outcome.bBlocked);
	FString BlockReason;
	TestTrue(TEXT("asset is blocked from further mutation"),
		FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
	TestEqual(TEXT("readback mismatch triggered the recovery path"), Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	TestEqual(TEXT("recovery compilation is still reported"), Outcome.RecoveryCompileCount, 1);
	TestEqual(TEXT("observed recovery compile before the failed verification"), Operations.RecoveryCompiles, 1);
	TSharedPtr<FJsonObject> Retry = CortexGraphPatchCompileTest::AdapterRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000709"));
	FCortexGraphPreparedPatch RetryPreview;
	TestTrue(TEXT("retry request can still be previewed while blocked"),
		FCortexGraphPatchOps::Preflight(Blueprint, Retry, RetryPreview, Error));
	Retry->SetBoolField(TEXT("dry_run"), false);
	Retry->SetStringField(TEXT("expected_validation_hash"), RetryPreview.ValidationHash);
	FCortexGraphPatchOutcome RetryOutcome;
	TestFalse(TEXT("blocked asset refuses a later patch"),
		FCortexGraphPatchOps::Execute(Blueprint, Retry, RetryOutcome, Error));
	TestEqual(TEXT("blocked asset performs no further compile"), Operations.TargetCompiles, 1);
	TestTrue(TEXT("blocked asset remains readable"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint).IsValid());
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 8. Preview and describe never compile or save
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompilePreviewTest,
	"Cortex.Graph.Authoring.Compile.PreviewNeverCompilesOrSaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompilePreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_PatchPreviewCompile_T08"));
	UPackage* ExternalPackage = nullptr;
	UBlueprint* External = CortexGraphPatchCompileTest::MakeBlueprint(ExternalPackage, TEXT("BP_PatchPreviewCompileSource_T08"));
	TestNotNull(TEXT("preview fixture Blueprints created"), Blueprint);
	TestNotNull(TEXT("preview external fixture Blueprint created"), External);
	if (!Blueprint || !External)
	{
		if (Blueprint) CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		if (External) CortexGraphPatchCompileTest::Cleanup(ExternalPackage, External);
		return false;
	}

	UEdGraph* ExternalGraph = External->UbergraphPages[0];
	UK2Node_CustomEvent* ExternalEvent = NewObject<UK2Node_CustomEvent>(ExternalGraph);
	ExternalEvent->CreateNewGuid();
	ExternalEvent->CustomFunctionName = FName(TEXT("T08DependencyEvent"));
	ExternalGraph->AddNode(ExternalEvent, true, false);
	ExternalEvent->AllocateDefaultPins();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External);
	FKismetEditorUtilities::CompileBlueprint(External);
	const EBlueprintStatus ExternalStatusBefore = External->Status;
	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::AdapterRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000000808"));
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("preview of an adapter patch succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestTrue(TEXT("preview records compiled apply intent"), Preview.bCompile);
	TestEqual(TEXT("preview compiles nothing"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("preview saves nothing"), Operations.Saves, 0);
	TestEqual(TEXT("preview leaves dependency compile status untouched"),
		static_cast<int32>(External->Status), static_cast<int32>(ExternalStatusBefore));
	TestEqual(TEXT("preview leaves target authoring state untouched"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);

	FCortexGraphPatchOutcome PreviewOutcome;
	TestFalse(TEXT("Execute refuses a preview-prepared request"),
		FCortexGraphPatchOps::Execute(Blueprint, Request, PreviewOutcome, Error));
	TestEqual(TEXT("refused preview performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("refused preview applies nothing"), PreviewOutcome.ApplyStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("refused preview leaves target authoring state untouched"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	CortexGraphPatchCompileTest::Cleanup(ExternalPackage, External);
	return true;
}

// ---------------------------------------------------------------------------
// 9. A class-pin default that reconstructs the node is fully reversible
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileClassPinRecoveryTest,
	"Cortex.Graph.Authoring.Compile.ClassPinReconstructionRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileClassPinRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_ClassPinRecovery_T08"));
	TestNotNull(TEXT("class-pin recovery fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CustomEvent* Event = CortexGraphPatchCompileTest::AddNamedCustomEvent(Blueprint, TEXT("T08ClassPinEvent"));
	TestNotNull(TEXT("class-pin fixture event created"), Event);
	UK2Node_GenericCreateObject* Create = CortexGraphPatchCompileTest::AddGenericCreateObject(Graph);
	UK2Node_CallFunction* Consumer = CortexGraphPatchCompileTest::AddCallNode(
		Graph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsValid")));
	TestNotNull(TEXT("class-pin fixture construct node created"), Create);
	TestNotNull(TEXT("class-pin fixture consumer created"), Consumer);
	if (!Create || !Consumer || !Event)
	{
		CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		return false;
	}

	FCortexCommandResult Error;
	UEdGraphPin* ClassPin = Create->GetClassPin();
	TestNotNull(TEXT("class pin allocated"), ClassPin);
	TestTrue(TEXT("fixture class default applies"),
		FCortexGraphPinDefaults::ApplyDefault(ClassPin, CortexGraphPatchCompileTest::ClassLiteral(TEXT("/Script/Engine.StaticMesh")), Error));
	TestEqual(TEXT("fixture class pin holds the initial class"),
		CortexGraphPatchCompileTest::ConstructedClassPath(Create), FString(TEXT("/Script/Engine.StaticMesh")));

	UEdGraphPin* ResultPin = Create->GetResultPin();
	UEdGraphPin* ConsumerPin = Consumer->FindPin(TEXT("Object"));
	const UEdGraphSchema* Schema = Graph->GetSchema();
	TestTrue(TEXT("fixture result link created"),
		ResultPin && ConsumerPin && Schema && Schema->TryCreateConnection(ResultPin, ConsumerPin));
	TestTrue(TEXT("fixture exec link created"),
		Schema && Schema->TryCreateConnection(Event->FindPin(TEXT("then")), Create->FindPin(TEXT("execute"))));

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestEqual(TEXT("class-pin fixture compiles"), static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));
	if (Blueprint->Status != BS_UpToDate)
	{
		CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		return false;
	}

	const EBlueprintStatus StatusBefore = Blueprint->Status;
	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const FString GeneratedBefore = FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint);
	const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
	const TArray<FString> PinsBefore = CortexGraphPatchCompileTest::SortedPinNames(Create);
	const FString ResultTypeBefore = ResultPin->PinType.PinSubCategoryObject.IsValid()
		? ResultPin->PinType.PinSubCategoryObject->GetPathName() : FString();

	TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000908"));
	CortexGraphPatchCompileTest::AddClassPinUpdate(Request, Create->NodeGuid, TEXT("Class"), TEXT("/Script/Engine.SkeletalMesh"));
	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);

	FCortexGraphPreparedPatch Preview;
	TestTrue(FString::Printf(TEXT("class-pin recovery preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

	FCortexGraphPatchOps::SetReadbackFaultForTesting(TEXT("readback_default"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("induced readback failure rejects the class-pin patch"),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	TestEqual(TEXT("class-pin apply phase ran"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("class-pin patch compiled once"), Outcome.TargetCompileCount, 1);
	TestEqual(TEXT("class-pin readback reported the mismatch"),
		Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	TestEqual(TEXT("class-pin recovery is verified after the compile"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestEqual(TEXT("class-pin recovery counts its own compile"), Outcome.RecoveryCompileCount, 1);
	TestEqual(TEXT("class-pin recovery observed one recovery compile"), Operations.RecoveryCompiles, 1);
	TestFalse(TEXT("class-pin recovery does not block the asset"), Outcome.bBlocked);
	FString BlockReason;
	TestFalse(TEXT("class-pin recovery leaves the asset mutable"),
		FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
	TestEqual(TEXT("class-pin recovery restores the authoring fingerprint"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	TestEqual(TEXT("class-pin recovery restores the generated digest"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint), GeneratedBefore);
	TestEqual(TEXT("class-pin recovery restores the compile status"),
		static_cast<int32>(Blueprint->Status), static_cast<int32>(StatusBefore));
	TestEqual(TEXT("class-pin recovery leaves no residual nodes"),
		CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);

	UK2Node_GenericCreateObject* Restored = Cast<UK2Node_GenericCreateObject>(
		CortexGraphPatchCompileTest::FindNodeByGuid(Blueprint, Create->NodeGuid));
	TestNotNull(TEXT("class-pin node re-resolves after recovery"), Restored);
	if (Restored)
	{
		TestEqual(TEXT("class-pin recovery restores the constructed class"),
			CortexGraphPatchCompileTest::ConstructedClassPath(Restored), FString(TEXT("/Script/Engine.StaticMesh")));
		TestEqual(TEXT("class-pin recovery restores the pin set"),
			FString::Join(CortexGraphPatchCompileTest::SortedPinNames(Restored), TEXT(",")),
			FString::Join(PinsBefore, TEXT(",")));
		UEdGraphPin* RestoredResult = Restored->GetResultPin();
		const FString RestoredResultType = RestoredResult && RestoredResult->PinType.PinSubCategoryObject.IsValid()
			? RestoredResult->PinType.PinSubCategoryObject->GetPathName() : FString();
		TestEqual(TEXT("class-pin recovery restores the reconstructed result pin type"),
			RestoredResultType, ResultTypeBefore);
		TestEqual(TEXT("class-pin recovery restores the pre-existing result link"),
			RestoredResult ? RestoredResult->LinkedTo.Num() : 0, 1);
	}
	TestEqual(TEXT("class-pin recovery performs no save"), Operations.Saves, 0);
	Operations.End();
	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 10. Class-driven exposed-on-spawn pins are restored by rollback
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompileClassPinExposedPinRecoveryTest,
	"Cortex.Graph.Authoring.Compile.ClassPinExposedPinRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompileClassPinExposedPinRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* BasePackage = nullptr;
	UBlueprint* Base = CortexGraphPatchCompileTest::MakeBlueprintWithParent(
		BasePackage, TEXT("BP_ClassPinExposedBase_T08"), UObject::StaticClass());
	TestNotNull(TEXT("exposed-pin base Blueprint created"), Base);
	if (!Base) return false;
	FEdGraphPinType TextType;
	TextType.PinCategory = UEdGraphSchema_K2::PC_Text;
	CortexGraphPatchCompileTest::AddExposeOnSpawnVariable(Base, FName(TEXT("Title")), TextType);

	UPackage* ChildPackage = nullptr;
	UBlueprint* Child = CortexGraphPatchCompileTest::MakeBlueprintWithParent(
		ChildPackage, TEXT("BP_ClassPinExposedChild_T08"), Base->GeneratedClass);
	TestNotNull(TEXT("exposed-pin child Blueprint created"), Child);
	if (!Child)
	{
		CortexGraphPatchCompileTest::Cleanup(BasePackage, Base);
		return false;
	}
	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = UObject::StaticClass();
	CortexGraphPatchCompileTest::AddExposeOnSpawnVariable(Child, FName(TEXT("ConfigObj")), ObjectType);

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(Package, TEXT("BP_ClassPinExposedRecovery_T08"));
	TestNotNull(TEXT("exposed-pin recovery fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphPatchCompileTest::Cleanup(ChildPackage, Child);
		CortexGraphPatchCompileTest::Cleanup(BasePackage, Base);
		return false;
	}

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_GenericCreateObject* Create = CortexGraphPatchCompileTest::AddGenericCreateObject(Graph);
	TestNotNull(TEXT("exposed-pin construct node created"), Create);
	if (!Create)
	{
		CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		CortexGraphPatchCompileTest::Cleanup(ChildPackage, Child);
		CortexGraphPatchCompileTest::Cleanup(BasePackage, Base);
		return false;
	}

	FCortexCommandResult Error;
	const FString ChildClassPath = Child->GeneratedClass->GetPathName();
	const FString BaseClassPath = Base->GeneratedClass->GetPathName();
	TestTrue(TEXT("fixture class default applies to the child class"),
		FCortexGraphPinDefaults::ApplyDefault(Create->GetClassPin(),
			CortexGraphPatchCompileTest::ClassLiteral(*ChildClassPath), Error));
	TestNotNull(TEXT("child-class exposed Title pin exists"), Create->FindPin(TEXT("Title")));
	TestNotNull(TEXT("child-class exposed ConfigObj pin exists"), Create->FindPin(TEXT("ConfigObj")));

	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
	const TArray<FString> PinsBefore = CortexGraphPatchCompileTest::SortedPinNames(Create);

	auto MakeRequest = [&](const TCHAR* PatchId)
	{
		TSharedPtr<FJsonObject> Request = CortexGraphPatchCompileTest::BaseRequest(Blueprint, PatchId);
		CortexGraphPatchCompileTest::AddClassPinUpdate(Request, Create->NodeGuid, TEXT("Class"), *BaseClassPath);
		return Request;
	};

	// (a) an induced readback failure recovers the class-driven pin set exactly
	TSharedPtr<FJsonObject> Request = MakeRequest(TEXT("00000000-0000-0000-0000-000000000a08"));
	FCortexGraphPreparedPatch Preview;
	TestTrue(FString::Printf(TEXT("exposed-pin recovery preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

	FCortexGraphPatchOps::SetReadbackFaultForTesting(TEXT("readback_default"));
	FCortexGraphPatchOutcome Outcome;
	TestFalse(TEXT("induced readback failure rejects the exposed-pin class patch"),
		FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
	FCortexGraphPatchOps::SetReadbackFaultForTesting(NAME_None);
	TestEqual(TEXT("exposed-pin readback reported the mismatch"),
		Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	TestEqual(TEXT("exposed-pin recovery is verified"), Outcome.RollbackStatus, FString(TEXT("restored")));
	TestFalse(TEXT("exposed-pin recovery does not block the asset"), Outcome.bBlocked);
	TestEqual(TEXT("exposed-pin recovery restores the authoring fingerprint"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	TestEqual(TEXT("exposed-pin recovery leaves no residual nodes"),
		CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);

	UK2Node_GenericCreateObject* Restored = Cast<UK2Node_GenericCreateObject>(
		CortexGraphPatchCompileTest::FindNodeByGuid(Blueprint, Create->NodeGuid));
	TestNotNull(TEXT("exposed-pin node re-resolves after recovery"), Restored);
	if (Restored)
	{
		TestEqual(TEXT("exposed-pin recovery restores the constructed class"),
			CortexGraphPatchCompileTest::ConstructedClassPath(Restored), ChildClassPath);
		TestNotNull(TEXT("exposed-pin recovery restores the removed spawn pin"), Restored->FindPin(TEXT("ConfigObj")));
		TestNotNull(TEXT("exposed-pin recovery keeps the shared spawn pin"), Restored->FindPin(TEXT("Title")));
		TestEqual(TEXT("exposed-pin recovery restores the full pin set"),
			FString::Join(CortexGraphPatchCompileTest::SortedPinNames(Restored), TEXT(",")),
			FString::Join(PinsBefore, TEXT(",")));
	}

	// (b) the same patch now succeeds, proving the class change really rebuilds the pins
	TSharedPtr<FJsonObject> Second = MakeRequest(TEXT("00000000-0000-0000-0000-000000000a09"));
	FCortexGraphPreparedPatch SecondPreview;
	TestTrue(FString::Printf(TEXT("exposed-pin reapply preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Second, SecondPreview, Error));
	Second->SetBoolField(TEXT("dry_run"), false);
	Second->SetStringField(TEXT("expected_validation_hash"), SecondPreview.ValidationHash);
	FCortexGraphPatchOutcome SecondOutcome;
	TestTrue(FString::Printf(TEXT("exposed-pin class change applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Execute(Blueprint, Second, SecondOutcome, Error));
	TestEqual(TEXT("exposed-pin class change verifies"),
		SecondOutcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("exposed-pin class change keeps the base class"),
		CortexGraphPatchCompileTest::ConstructedClassPath(Create), BaseClassPath);
	TestNull(TEXT("exposed-pin class change drops the child-only spawn pin"), Create->FindPin(TEXT("ConfigObj")));
	TestNotNull(TEXT("exposed-pin class change keeps the shared spawn pin"), Create->FindPin(TEXT("Title")));

	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	CortexGraphPatchCompileTest::Cleanup(ChildPackage, Child);
	CortexGraphPatchCompileTest::Cleanup(BasePackage, Base);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchConstructObjectEligibilityTest,
	"Cortex.Graph.Authoring.Construction.ConstructObjectEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchConstructObjectEligibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchCompileTest::MakeBlueprint(
		Package, TEXT("BP_ConstructObjectEligibility"));
	TestNotNull(TEXT("constructibility fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
		return false;
	}

	const FString FingerprintBefore = CortexGraphPatchCompileTest::GraphHash(Blueprint);
	const int32 NodesBefore = CortexGraphPatchCompileTest::CountNativeNodes(Blueprint);
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	const int32 StatusBefore = static_cast<int32>(Blueprint->Status);

	TSharedPtr<FJsonObject> Forbidden = CortexGraphPatchCompileTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000a10"));
	CortexGraphPatchCompileTest::AddNode(
		Forbidden, TEXT("component"), TEXT("ConstructObject"),
		CortexGraphPatchCompileTest::ClassParams(TEXT("/Script/Engine.SceneComponent")));
	Forbidden->SetBoolField(TEXT("dry_run"), false);
	Forbidden->SetStringField(TEXT("expected_validation_hash"), TEXT("stale-token"));

	CortexGraphPatchCompileTest::FOperations Operations;
	Operations.Begin();
	FCortexGraphPatchOutcome Outcome;
	FCortexCommandResult Error;
	TestFalse(TEXT("SceneComponent is rejected by ConstructObject preflight"),
		FCortexGraphPatchOps::Execute(Blueprint, Forbidden, Outcome, Error));
	TestEqual(TEXT("forbidden class returns INVALID_FIELD"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("forbidden class is refused before apply"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
	TestEqual(TEXT("forbidden class never reaches target compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("forbidden class leaves native nodes untouched"),
		CortexGraphPatchCompileTest::CountNativeNodes(Blueprint), NodesBefore);
	TestEqual(TEXT("forbidden class leaves the graph fingerprint unchanged"),
		CortexGraphPatchCompileTest::GraphHash(Blueprint), FingerprintBefore);
	TestEqual(TEXT("forbidden class leaves dirty state unchanged"),
		Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("forbidden class leaves compile status unchanged"),
		static_cast<int32>(Blueprint->Status), StatusBefore);

	TSharedPtr<FJsonObject> ActorClass = CortexGraphPatchCompileTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000a12"));
	CortexGraphPatchCompileTest::AddNode(
		ActorClass, TEXT("actor"), TEXT("ConstructObject"),
		CortexGraphPatchCompileTest::ClassParams(TEXT("/Script/Engine.Actor")));
	FCortexGraphPatchOutcome ActorOutcome;
	Error = FCortexCommandResult();
	TestFalse(TEXT("AActor itself is rejected by ConstructObject preflight"),
		FCortexGraphPatchOps::Execute(Blueprint, ActorClass, ActorOutcome, Error));
	TestEqual(TEXT("AActor returns INVALID_FIELD"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("AActor never reaches target compile"), Operations.TargetCompiles, 0);


	TSharedPtr<FJsonObject> Allowed = CortexGraphPatchCompileTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000a11"));
	CortexGraphPatchCompileTest::AddNode(
		Allowed, TEXT("curve"), TEXT("ConstructObject"),
		CortexGraphPatchCompileTest::ClassParams(TEXT("/Script/Engine.CurveFloat")));
	FCortexGraphPreparedPatch Prepared;
	Error = FCortexCommandResult();
	TestTrue(FString::Printf(TEXT("CurveFloat remains constructible: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Allowed, Prepared, Error));
	TestEqual(TEXT("allowed-class preflight does not compile"), Operations.TargetCompiles, 0);
	Operations.End();

	CortexGraphPatchCompileTest::Cleanup(Package, Blueprint);
	return true;
}

#endif
