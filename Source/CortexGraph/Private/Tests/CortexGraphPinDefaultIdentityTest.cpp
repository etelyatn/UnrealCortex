#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "CortexGraphTestContentRoot.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_CallFunction.h"
#include "K2Node_GenericCreateObject.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphPinDefaultIdentityTest
{
static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static UK2Node_CallFunction* MakeCallNode(UEdGraph* Graph, UFunction* Function)
{
	if (!Graph || !Function) return nullptr;
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(Function);
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	Graph->AddNode(Node, true, false);
	return Node;
}

static void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPinDefaultIdentityTestCleanup")));
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
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

static void AddNode(
	const TSharedPtr<FJsonObject>& Request,
	const TCHAR* ClientId,
	const TCHAR* NodeClass,
	const TCHAR* FunctionName,
	const TCHAR* PinName,
	const TSharedPtr<FJsonObject>& Literal)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), ClientId);
	Node->SetStringField(TEXT("node_class"), NodeClass);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("function_name"), FunctionName);
	Node->SetObjectField(TEXT("params"), Params);
	TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	Defaults->SetObjectField(PinName, Literal);
	Node->SetObjectField(TEXT("defaults"), Defaults);
	TArray<TSharedPtr<FJsonValue>> Nodes = Request->GetArrayField(TEXT("nodes"));
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
}

static TSharedPtr<FJsonObject> RealLiteral(const double Value)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("real"));
	Literal->SetNumberField(TEXT("value"), Value);
	return Literal;
}

static TSharedPtr<FJsonObject> TextLiteral(const TCHAR* Value)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("text"));
	Literal->SetStringField(TEXT("literal"), Value);
	return Literal;
}

static TSharedPtr<FJsonObject> TextTableLiteral(const TCHAR* TableId, const TCHAR* Key)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("text"));
	Literal->SetStringField(TEXT("table"), TableId);
	Literal->SetStringField(TEXT("key"), Key);
	return Literal;
}

static TSharedPtr<FJsonObject> ReferenceLiteral(const TCHAR* Kind, const TCHAR* Path)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), Kind);
	Literal->SetStringField(TEXT("path"), Path);
	return Literal;
}

/** Runs a preview and marks the request as an apply request carrying its validation token. */
static bool PrepareForApply(
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

static UEdGraphNode* FindCreatedNode(UBlueprint* Blueprint, const FCortexGraphPatchOutcome& Outcome, const TCHAR* ClientId)
{
	const FGuid* NodeGuid = Outcome.Locators.NodeGuidByClientId.Find(ClientId);
	return NodeGuid ? FindNodeByGuid(Blueprint, *NodeGuid) : nullptr;
}

/** Finds the single created call-function node of a fixture Blueprint. */
static UK2Node_CallFunction* FindSingleCallNode(UBlueprint* Blueprint)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				return Call;
			}
		}
	}
	return nullptr;
}

/** Registered transient string table used as string-table identity and soft-object target. */
struct FStringTableFixture
{
	UPackage* Package = nullptr;
	UStringTable* Table = nullptr;

	void Create(const TCHAR* PackageName, const TCHAR* TableName, const TCHAR* SourceString)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), PackageName));
		Table = NewObject<UStringTable>(Package, FName(TableName), RF_Public | RF_Standalone | RF_Transactional);
		Table->GetMutableStringTable()->SetSourceString(FTextKey(TEXT("Key")), FString(SourceString), FString());
		// A second key with the same display text keeps negative cases resolvable.
		Table->GetMutableStringTable()->SetSourceString(FTextKey(TEXT("KeyAlt")), FString(SourceString), FString());
		FStringTableRegistry::Get().RegisterStringTable(Table->GetStringTableId(), Table->GetMutableStringTable());
	}

	void Reset()
	{
		if (Table)
		{
			FStringTableRegistry::Get().UnregisterStringTable(Table->GetStringTableId());
			Table->ClearFlags(RF_Standalone);
			Table->MarkAsGarbage();
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
		}
	}
};

static FString CanonicalDouble(const double Value)
{
	return FString::Printf(TEXT("%.17g"), Value);
}
}

// ---------------------------------------------------------------------------
// 1. Real pin defaults are stored and compared losslessly
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultRealPrecisionTest,
	"Cortex.Graph.Authoring.Defaults.RealPrecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultRealPrecisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultReal_T08"));
	TestNotNull(TEXT("real-precision fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* Node = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Abs")));
	TestNotNull(TEXT("real pin fixture node created"), Node);
	UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("A")) : nullptr;
	TestNotNull(TEXT("real input pin resolved"), Pin);
	if (!Pin)
	{
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		return false;
	}

	const double Requested = 1.0000004;
	const TSharedPtr<FJsonObject> Literal = CortexGraphPinDefaultIdentityTest::RealLiteral(Requested);
	FCortexCommandResult Error;
	TestTrue(TEXT("real default applies"), FCortexGraphPinDefaults::ApplyDefault(Pin, Literal, Error));
	TestEqual(TEXT("native real default round-trips the requested double exactly"),
		CortexGraphPinDefaultIdentityTest::CanonicalDouble(FCString::Atod(*Pin->DefaultValue)),
		CortexGraphPinDefaultIdentityTest::CanonicalDouble(Requested));

	FString Expected;
	FString Actual;
	FString Failure;
	TestTrue(FString::Printf(TEXT("equal real default compares: %s"), *Failure),
		FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, Literal, Expected, Actual, Failure));
	TestEqual(TEXT("equal real defaults compare equal"), Actual, Expected);

	const TSharedPtr<FJsonObject> Divergent = CortexGraphPinDefaultIdentityTest::RealLiteral(1.0000005);
	Expected.Reset();
	Actual.Reset();
	Failure.Reset();
	TestTrue(FString::Printf(TEXT("divergent real default still compares: %s"), *Failure),
		FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, Divergent, Expected, Actual, Failure));
	TestNotEqual(TEXT("a divergent real default is not reported as a match"), Actual, Expected);
	TestNotEqual(TEXT("the canonical identity keeps more than six fraction digits"),
		Actual, FString(TEXT("1.000000")));

	CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 2. Readback detects a divergent native real after a lossless apply
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultRealReadbackTest,
	"Cortex.Graph.Authoring.Defaults.RealReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultRealReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double Requested = 1.0000004;

	// (a) an equal native real is a lossless match
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultRealMatch_T08"));
		TestNotNull(TEXT("real-match fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-000000000a01"));
			CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("abs"), TEXT("CallFunction"),
				TEXT("KismetMathLibrary.Abs"), TEXT("A"), CortexGraphPinDefaultIdentityTest::RealLiteral(Requested));

			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("real-match preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
			FCortexGraphPatchOutcome Outcome;
			TestTrue(FString::Printf(TEXT("real-match patch applies and verifies: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			TestEqual(TEXT("real-match readback is authoritative"),
				Outcome.ReadbackStatus, FString(TEXT("matched")));
			TestEqual(TEXT("real-match leaves no rollback"), Outcome.RollbackStatus, FString(TEXT("not_requested")));

			UEdGraphNode* Created = CortexGraphPinDefaultIdentityTest::FindCreatedNode(Blueprint, Outcome, TEXT("abs"));
			UEdGraphPin* Pin = Created ? Created->FindPin(TEXT("A")) : nullptr;
			TestNotNull(TEXT("created real pin resolved"), Pin);
			if (Pin)
			{
				TestEqual(TEXT("persisted native real stays lossless"),
					CortexGraphPinDefaultIdentityTest::CanonicalDouble(FCString::Atod(*Pin->DefaultValue)),
					CortexGraphPinDefaultIdentityTest::CanonicalDouble(Requested));
			}
			CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) a native real truncated to six fraction digits is a mismatch
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultRealDiverge_T08"));
		TestNotNull(TEXT("real-divergence fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-000000000a02"));
			CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("abs"), TEXT("CallFunction"),
				TEXT("KismetMathLibrary.Abs"), TEXT("A"), CortexGraphPinDefaultIdentityTest::RealLiteral(Requested));

			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("real-divergence preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(
				[](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPinDefaultIdentityTest::FindSingleCallNode(Mutated);
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("A")) : nullptr)
					{
						// Mirrors an engine-authored default truncated to six fraction digits.
						Pin->DefaultValue = TEXT("1.000000");
					}
				});
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("native real divergence is rejected"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
			TestEqual(TEXT("divergent real reports a readback mismatch"),
				Outcome.ReadbackStatus, FString(TEXT("mismatched")));
			TestEqual(TEXT("divergent real restores exactly"), Outcome.RollbackStatus, FString(TEXT("restored")));
			TestFalse(TEXT("divergent real does not block the asset"), Outcome.bBlocked);
			TestTrue(TEXT("divergent real reports the canonical values"),
				Error.ErrorMessage.Contains(TEXT("default readback mismatch")));
			CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// 3. Text identity includes string table id and key, not just display text
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultTextIdentityTest,
	"Cortex.Graph.Authoring.Defaults.TextIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultTextIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultText_T08"));
	TestNotNull(TEXT("text-identity fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	CortexGraphPinDefaultIdentityTest::FStringTableFixture TableA;
	CortexGraphPinDefaultIdentityTest::FStringTableFixture TableB;
	TableA.Create(TEXT("ST_PinIdentityA_T08"), TEXT("ST_IdentityA"), TEXT("Hello"));
	TableB.Create(TEXT("ST_PinIdentityB_T08"), TEXT("ST_IdentityB"), TEXT("Hello"));
	const FString TableAId = TableA.Table->GetStringTableId().ToString();
	const FString TableBId = TableB.Table->GetStringTableId().ToString();
	TestNotEqual(TEXT("string table ids are distinct"), TableAId, TableBId);

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* Node = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetTextLibrary::StaticClass()->FindFunctionByName(TEXT("TextIsEmpty")));
	TestNotNull(TEXT("text pin fixture node created"), Node);
	UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("InText")) : nullptr;
	TestNotNull(TEXT("text input pin resolved"), Pin);
	if (!Pin)
	{
		TableB.Reset();
		TableA.Reset();
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		return false;
	}

	Pin->DefaultTextValue = FText::FromStringTable(TableA.Table->GetStringTableId(), FTextKey(TEXT("Key")));
	TestEqual(TEXT("fixture string table resolves the shared display text"),
		Pin->DefaultTextValue.ToString(), FString(TEXT("Hello")));

	FString Expected;
	FString Actual;
	FString Failure;
	auto Compare = [&](const TSharedPtr<FJsonObject>& Literal) -> bool
	{
		Expected.Reset();
		Actual.Reset();
		Failure.Reset();
		return FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, Literal, Expected, Actual, Failure);
	};

	TestTrue(TEXT("exact table and key compare"), Compare(
		CortexGraphPinDefaultIdentityTest::TextTableLiteral(*TableAId, TEXT("Key"))));
	TestEqual(TEXT("exact table and key match"), Actual, Expected);

	TestTrue(TEXT("same table with another key still compares"), Compare(
		CortexGraphPinDefaultIdentityTest::TextTableLiteral(*TableAId, TEXT("KeyAlt"))));
	TestNotEqual(TEXT("same table with another key mismatches"), Actual, Expected);

	TestTrue(TEXT("another table with the same display text still compares"), Compare(
		CortexGraphPinDefaultIdentityTest::TextTableLiteral(*TableBId, TEXT("Key"))));
	TestNotEqual(TEXT("another table with the same display text mismatches"), Actual, Expected);
	TestTrue(TEXT("the canonical identity reports the native table id"), Actual.Contains(TableAId));
	TestTrue(TEXT("the canonical identity reports the planned table id"), Expected.Contains(TableBId));

	TestTrue(TEXT("a literal plan for a string-table value still compares"), Compare(
		CortexGraphPinDefaultIdentityTest::TextLiteral(TEXT("Hello"))));
	TestNotEqual(TEXT("a literal plan for a string-table value mismatches"), Actual, Expected);

	Pin->DefaultTextValue = FText::FromString(TEXT("Hello"));
	TestTrue(TEXT("an equal literal still compares"), Compare(
		CortexGraphPinDefaultIdentityTest::TextLiteral(TEXT("Hello"))));
	TestEqual(TEXT("an equal literal matches"), Actual, Expected);

	TableB.Reset();
	TableA.Reset();
	CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 4. Readback keeps string table identity, so an equal display text is not enough
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultTextReadbackTest,
	"Cortex.Graph.Authoring.Defaults.TextReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultTextReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPinDefaultIdentityTest::FStringTableFixture TableA;
	CortexGraphPinDefaultIdentityTest::FStringTableFixture TableB;
	TableA.Create(TEXT("ST_PinReadbackA_T08"), TEXT("ST_ReadbackA"), TEXT("Hello"));
	TableB.Create(TEXT("ST_PinReadbackB_T08"), TEXT("ST_ReadbackB"), TEXT("Hello"));
	const FString TableAId = TableA.Table->GetStringTableId().ToString();
	const FString TableBId = TableB.Table->GetStringTableId().ToString();

	// (a) an exact table and key is an authoritative match
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultTextMatch_T08"));
		TestNotNull(TEXT("text-match fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-000000000b01"));
			CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("text"), TEXT("CallFunction"),
				TEXT("KismetTextLibrary.TextIsEmpty"), TEXT("InText"),
				CortexGraphPinDefaultIdentityTest::TextTableLiteral(*TableAId, TEXT("Key")));

			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("text-match preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
			FCortexGraphPatchOutcome Outcome;
			TestTrue(FString::Printf(TEXT("string-table text default applies and verifies: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			TestEqual(TEXT("string-table text readback matches"),
				Outcome.ReadbackStatus, FString(TEXT("matched")));

			UEdGraphNode* Created = CortexGraphPinDefaultIdentityTest::FindCreatedNode(Blueprint, Outcome, TEXT("text"));
			UEdGraphPin* Pin = Created ? Created->FindPin(TEXT("InText")) : nullptr;
			TestNotNull(TEXT("created text pin resolved"), Pin);
			if (Pin)
			{
				FName NativeTableId;
				FTextKey NativeKey;
				TestTrue(TEXT("persisted text keeps string-table identity"),
					FTextInspector::GetTableIdAndKey(Pin->DefaultTextValue, NativeTableId, NativeKey));
				TestEqual(TEXT("persisted string table id"), NativeTableId.ToString(), TableAId);
				TestEqual(TEXT("persisted string table key"), NativeKey.ToString(), FString(TEXT("Key")));
			}
			CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) a same-text value from another table is a mismatch
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultTextDiverge_T08"));
		TestNotNull(TEXT("text-divergence fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-000000000b02"));
			CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("text"), TEXT("CallFunction"),
				TEXT("KismetTextLibrary.TextIsEmpty"), TEXT("InText"),
				CortexGraphPinDefaultIdentityTest::TextTableLiteral(*TableAId, TEXT("Key")));

			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("text-divergence preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
			const FName TableBName = TableB.Table->GetStringTableId();
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(
				[TableBName](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPinDefaultIdentityTest::FindSingleCallNode(Mutated);
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("InText")) : nullptr)
					{
						// Same display text, different string table identity.
						Pin->DefaultTextValue = FText::FromStringTable(TableBName, FTextKey(TEXT("Key")));
					}
				});
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("text identity divergence is rejected"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
			TestEqual(TEXT("divergent text reports a readback mismatch"),
				Outcome.ReadbackStatus, FString(TEXT("mismatched")));
			TestEqual(TEXT("divergent text restores exactly"), Outcome.RollbackStatus, FString(TEXT("restored")));
			TestFalse(TEXT("divergent text does not block the asset"), Outcome.bBlocked);
			TestTrue(TEXT("divergent text diagnostics report canonical identities"),
				Error.ErrorMessage.Contains(TEXT("default readback mismatch")));
			CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		}
	}

	TableB.Reset();
	TableA.Reset();
	return true;
}

// ---------------------------------------------------------------------------
// 5. Soft reference storage modes are validated, applied and compared exactly
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultSoftReferenceTest,
	"Cortex.Graph.Authoring.Defaults.SoftReferenceModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultSoftReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultSoft_T08"));
	TestNotNull(TEXT("soft-reference fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	CortexGraphPinDefaultIdentityTest::FStringTableFixture SoftTarget;
	SoftTarget.Create(TEXT("ST_PinSoft_T08"), TEXT("ST_Soft"), TEXT("Hello"));
	const FString SoftObjectPath = SoftTarget.Table->GetPathName();
	const FString SoftClassPath = USceneComponent::StaticClass()->GetPathName();

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* SoftObjectNode = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsValidSoftObjectReference")));
	UK2Node_CallFunction* SoftClassNode = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsValidSoftClassReference")));
	UK2Node_CallFunction* HardObjectNode = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsValid")));
	UK2Node_GenericCreateObject* HardClassNode = NewObject<UK2Node_GenericCreateObject>(Graph);
	HardClassNode->CreateNewGuid();
	Graph->AddNode(HardClassNode, true, false);
	HardClassNode->AllocateDefaultPins();

	UEdGraphPin* SoftObjectPin = SoftObjectNode ? SoftObjectNode->FindPin(TEXT("SoftObjectReference")) : nullptr;
	UEdGraphPin* SoftClassPin = SoftClassNode ? SoftClassNode->FindPin(TEXT("SoftClassReference")) : nullptr;
	UEdGraphPin* HardObjectPin = HardObjectNode ? HardObjectNode->FindPin(TEXT("Object")) : nullptr;
	UEdGraphPin* HardClassPin = HardClassNode->GetClassPin();
	TestNotNull(TEXT("soft object pin resolved"), SoftObjectPin);
	TestNotNull(TEXT("soft class pin resolved"), SoftClassPin);
	TestNotNull(TEXT("hard object pin resolved"), HardObjectPin);
	TestNotNull(TEXT("hard class pin resolved"), HardClassPin);
	if (!SoftObjectPin || !SoftClassPin || !HardObjectPin || !HardClassPin)
	{
		SoftTarget.Reset();
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		return false;
	}
	TestEqual(TEXT("soft object pin stores soft object references"),
		SoftObjectPin->PinType.PinCategory.ToString(), UEdGraphSchema_K2::PC_SoftObject.ToString());
	TestEqual(TEXT("soft class pin stores soft class references"),
		SoftClassPin->PinType.PinCategory.ToString(), UEdGraphSchema_K2::PC_SoftClass.ToString());

	const TSharedPtr<FJsonObject> SoftObjectLiteral =
		CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("soft_object"), *SoftObjectPath);
	const TSharedPtr<FJsonObject> SoftClassLiteral =
		CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("soft_class"), *SoftClassPath);

	FCortexCommandResult Error;
	TestTrue(TEXT("soft object default applies"), FCortexGraphPinDefaults::ApplyDefault(SoftObjectPin, SoftObjectLiteral, Error));
	TestNull(TEXT("soft object default keeps DefaultObject empty"), SoftObjectPin->DefaultObject.Get());
	TestEqual(TEXT("soft object default persists the path in DefaultValue"), SoftObjectPin->DefaultValue, SoftObjectPath);

	TestTrue(TEXT("soft class default applies"), FCortexGraphPinDefaults::ApplyDefault(SoftClassPin, SoftClassLiteral, Error));
	TestNull(TEXT("soft class default keeps DefaultObject empty"), SoftClassPin->DefaultObject.Get());
	TestEqual(TEXT("soft class default persists the path in DefaultValue"), SoftClassPin->DefaultValue, SoftClassPath);

	FString Expected;
	FString Actual;
	FString Failure;
	TestTrue(FString::Printf(TEXT("soft object default compares: %s"), *Failure),
		FCortexGraphPinDefaults::CompareAppliedLiteral(SoftObjectPin, SoftObjectLiteral, Expected, Actual, Failure));
	TestEqual(TEXT("soft object default matches native storage"), Actual, Expected);
	TestTrue(TEXT("soft object canonical identity includes the storage mode"),
		Actual.StartsWith(TEXT("soft_object:")));

	Expected.Reset();
	Actual.Reset();
	Failure.Reset();
	TestTrue(FString::Printf(TEXT("soft class default compares: %s"), *Failure),
		FCortexGraphPinDefaults::CompareAppliedLiteral(SoftClassPin, SoftClassLiteral, Expected, Actual, Failure));
	TestEqual(TEXT("soft class default matches native storage"), Actual, Expected);
	TestTrue(TEXT("soft class canonical identity includes the storage mode"),
		Actual.StartsWith(TEXT("soft_class:")));

	// Requested modes that contradict the pin's storage fail closed.
	TestFalse(TEXT("hard object literal on a soft object pin is refused"),
		FCortexGraphPinDefaults::Validate(SoftObjectPin,
			CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("object"), *SoftObjectPath), Error));
	TestFalse(TEXT("hard class literal on a soft class pin is refused"),
		FCortexGraphPinDefaults::Validate(SoftClassPin,
			CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("class"), *SoftClassPath), Error));
	TestFalse(TEXT("soft object literal on a hard object pin is refused"),
		FCortexGraphPinDefaults::Validate(HardObjectPin,
			CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("soft_object"), *SoftObjectPath), Error));
	TestFalse(TEXT("soft class literal on a hard class pin is refused"),
		FCortexGraphPinDefaults::Validate(HardClassPin,
			CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("soft_class"), *SoftClassPath), Error));

	Expected.Reset();
	Actual.Reset();
	Failure.Reset();
	TestFalse(TEXT("comparison refuses a storage mode the pin cannot hold"),
		FCortexGraphPinDefaults::CompareAppliedLiteral(SoftObjectPin,
			CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("object"), *SoftObjectPath),
			Expected, Actual, Failure));
	TestFalse(TEXT("storage mode refusal reports a reason"), Failure.IsEmpty());

	SoftTarget.Reset();
	CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 6. Soft reference readback survives the coordinator and rejects coercion
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultSoftReferenceReadbackTest,
	"Cortex.Graph.Authoring.Defaults.SoftReferenceReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultSoftReferenceReadbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPinDefaultIdentityTest::FStringTableFixture SoftTarget;
	SoftTarget.Create(TEXT("ST_PinSoftReadback_T08"), TEXT("ST_SoftReadback"), TEXT("Hello"));
	const FString SoftObjectPath = SoftTarget.Table->GetPathName();
	const FString SoftClassPath = USceneComponent::StaticClass()->GetPathName();

	struct FCase
	{
		const TCHAR* ClientId;
		const TCHAR* FunctionName;
		const TCHAR* PinName;
		const TCHAR* Kind;
		FString Path;
	};
	const FCase Cases[] =
	{
		{ TEXT("softobj"), TEXT("KismetSystemLibrary.IsValidSoftObjectReference"), TEXT("SoftObjectReference"), TEXT("soft_object"), SoftObjectPath },
		{ TEXT("softcls"), TEXT("KismetSystemLibrary.IsValidSoftClassReference"), TEXT("SoftClassReference"), TEXT("soft_class"), SoftClassPath },
	};

	int32 PatchIndex = 0;
	for (const FCase& Case : Cases)
	{
		++PatchIndex;
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(
			Package, *FString::Printf(TEXT("BP_PinDefaultSoftReadback%02d_T08"), PatchIndex));
		TestNotNull(TEXT("soft readback fixture Blueprint created"), Blueprint);
		if (!Blueprint) continue;

		TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
			Blueprint, *FString::Printf(TEXT("00000000-0000-0000-0000-000000000c%02d"), PatchIndex));
		CortexGraphPinDefaultIdentityTest::AddNode(Request, Case.ClientId, TEXT("CallFunction"), Case.FunctionName,
			Case.PinName, CortexGraphPinDefaultIdentityTest::ReferenceLiteral(Case.Kind, *Case.Path));

		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("%s preview succeeds: %s"), Case.Kind, *Error.ErrorMessage),
			CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
		FCortexGraphPatchOutcome Outcome;
		TestTrue(FString::Printf(TEXT("%s default applies and verifies: %s"), Case.Kind, *Error.ErrorMessage),
			FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
		TestEqual(FString::Printf(TEXT("%s readback matches native soft storage"), Case.Kind),
			Outcome.ReadbackStatus, FString(TEXT("matched")));

		UEdGraphNode* Created = CortexGraphPinDefaultIdentityTest::FindCreatedNode(Blueprint, Outcome, Case.ClientId);
		UEdGraphPin* Pin = Created ? Created->FindPin(FName(Case.PinName)) : nullptr;
		TestNotNull(FString::Printf(TEXT("%s created pin resolved"), Case.Kind), Pin);
		if (Pin)
		{
			TestNull(FString::Printf(TEXT("%s persisted as a soft reference"), Case.Kind), Pin->DefaultObject.Get());
			TestEqual(FString::Printf(TEXT("%s persists the requested path"), Case.Kind), Pin->DefaultValue, Case.Path);
		}
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	}

	// Coercing a soft pin into hard object storage must not verify.
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultSoftCoerce_T08"));
		TestNotNull(TEXT("soft coercion fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-000000000c10"));
			CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("softobj"),
				TEXT("CallFunction"), TEXT("KismetSystemLibrary.IsValidSoftObjectReference"),
				TEXT("SoftObjectReference"), CortexGraphPinDefaultIdentityTest::ReferenceLiteral(TEXT("soft_object"), *SoftObjectPath));

			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("soft coercion preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPinDefaultIdentityTest::PrepareForApply(Blueprint, Request, Prepared, Error));
			UObject* const Coerced = SoftTarget.Table;
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(
				[Coerced](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPinDefaultIdentityTest::FindSingleCallNode(Mutated);
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("SoftObjectReference")) : nullptr)
					{
						// Hard object storage on a soft pin: the readback must not accept it.
						Pin->DefaultObject = Coerced;
						Pin->DefaultValue.Empty();
					}
				});
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("coerced soft default is rejected"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
			TestEqual(TEXT("coerced soft default reports a readback mismatch"),
				Outcome.ReadbackStatus, FString(TEXT("mismatched")));
			TestEqual(TEXT("coerced soft default restores exactly"),
				Outcome.RollbackStatus, FString(TEXT("restored")));
			TestFalse(TEXT("coerced soft default does not block the asset"), Outcome.bBlocked);
			CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		}
	}

	SoftTarget.Reset();
	return true;
}

// ---------------------------------------------------------------------------
// 7. A struct literal is refused before mutation instead of accepting then rolling back
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultStructRefusalTest,
	"Cortex.Graph.Authoring.Defaults.StructLiteralRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultStructRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultStruct_T08"));
	TestNotNull(TEXT("struct fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* Node = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("BreakVector")));
	TestNotNull(TEXT("struct fixture node created"), Node);
	UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("InVec")) : nullptr;
	TestNotNull(TEXT("struct input pin resolved"), Pin);
	if (!Pin)
	{
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		return false;
	}
	TestEqual(TEXT("fixture pin is a struct pin"), Pin->PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
	TestNotNull(TEXT("fixture pin carries a real script struct pin"),
		Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()));

	const FString NativeBefore = Pin->DefaultValue;
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("struct"));
	Literal->SetStringField(TEXT("value"), TEXT("(X=1.000000,Y=2.000000,Z=3.000000)"));

	FCortexCommandResult Error;
	TestFalse(TEXT("a struct literal is refused by validation"),
		FCortexGraphPinDefaults::Validate(Pin, Literal, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the unsupported kind [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("struct")));
	TestFalse(TEXT("a struct literal is never applied"),
		FCortexGraphPinDefaults::ApplyDefault(Pin, Literal, Error));
	TestEqual(TEXT("a refused struct literal does not touch the native pin"), Pin->DefaultValue, NativeBefore);

	TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000d01"));
	CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("break"), TEXT("CallFunction"),
		TEXT("KismetMathLibrary.BreakVector"), TEXT("InVec"), Literal);

	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	FCortexGraphPreparedPatch Preview;
	TestFalse(TEXT("a planned struct default is refused before mutation"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestTrue(FString::Printf(TEXT("a planned struct default is refused as an unsupported kind [%s|%s]"), *Error.ErrorCode, *Error.ErrorMessage),
		Error.ErrorCode == CortexErrorCodes::InvalidField
			&& Error.ErrorMessage.Contains(TEXT("Unsupported pin default kind: struct")));
	TestEqual(TEXT("a refused struct default leaves the graph untouched"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);

	CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 8. A scalar literal is never a default of a container pin
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinDefaultContainerScalarTest,
	"Cortex.Graph.Authoring.Defaults.ContainerScalarRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPinDefaultContainerScalarTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPinDefaultIdentityTest::MakeBlueprint(Package, TEXT("BP_PinDefaultContainer_T08"));
	TestNotNull(TEXT("container fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* Node = CortexGraphPinDefaultIdentityTest::MakeCallNode(
		Graph, UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("JoinStringArray")));
	TestNotNull(TEXT("container fixture node created"), Node);
	UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("SourceArray")) : nullptr;
	TestNotNull(TEXT("array input pin resolved"), Pin);
	if (!Pin)
	{
		CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
		return false;
	}
	TestEqual(TEXT("fixture pin keeps the element category"), Pin->PinType.PinCategory, UEdGraphSchema_K2::PC_String);
	TestEqual(TEXT("fixture pin is an array pin"),
		static_cast<int32>(Pin->PinType.ContainerType), static_cast<int32>(EPinContainerType::Array));

	const FString NativeBefore = Pin->DefaultValue;
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("string"));
	Literal->SetStringField(TEXT("value"), TEXT("scalar-on-array"));

	FCortexCommandResult Error;
	TestFalse(TEXT("a scalar literal on an array pin is refused by validation"),
		FCortexGraphPinDefaults::Validate(Pin, Literal, Error));
	TestTrue(FString::Printf(TEXT("the container refusal is a type mismatch on the container pin [%s|%s]"), *Error.ErrorCode, *Error.ErrorMessage),
		Error.ErrorCode == CortexErrorCodes::TypeMismatch
			&& Error.ErrorMessage.Contains(TEXT("container pin 'SourceArray'")));
	TestFalse(TEXT("a scalar literal on an array pin is never applied"),
		FCortexGraphPinDefaults::ApplyDefault(Pin, Literal, Error));
	TestEqual(TEXT("a refused container default does not touch the native pin"), Pin->DefaultValue, NativeBefore);

	FString Expected;
	FString Actual;
	FString Failure;
	TestFalse(TEXT("a scalar literal is never reported as a matching container default"),
		FCortexGraphPinDefaults::CompareAppliedLiteral(Pin, Literal, Expected, Actual, Failure));

	TSharedPtr<FJsonObject> Request = CortexGraphPinDefaultIdentityTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-000000000d02"));
	CortexGraphPinDefaultIdentityTest::AddNode(Request, TEXT("join"), TEXT("CallFunction"),
		TEXT("KismetStringLibrary.JoinStringArray"), TEXT("SourceArray"), Literal);

	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	FCortexGraphPreparedPatch Preview;
	TestFalse(TEXT("a planned container default is refused before mutation"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestTrue(FString::Printf(TEXT("a planned container default is refused as a container [%s|%s]"), *Error.ErrorCode, *Error.ErrorMessage),
		Error.ErrorCode == CortexErrorCodes::TypeMismatch
			&& Error.ErrorMessage.Contains(TEXT("container pin 'SourceArray'")));
	TestEqual(TEXT("a refused container default leaves the graph untouched"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);

	CortexGraphPinDefaultIdentityTest::Cleanup(Package, Blueprint);
	return true;
}

#endif
