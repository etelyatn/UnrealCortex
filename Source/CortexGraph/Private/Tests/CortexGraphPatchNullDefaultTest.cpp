#include "Misc/AutomationTest.h"
#include "CortexGraphTestContentRoot.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * A null reference default must be the representation the engine schema actually accepts: an empty
 * `DefaultValue` with a null `DefaultObject`. The candidate wrote the literal `"None"` into
 * `DefaultValue`, which the engine's own default validity rejects for every object/class category
 * (`UEdGraphSchema_K2::IsCurrentPinDefaultValid` reports "Default value 'None' ... is invalid"), so a
 * `compile=true` patch could fail *after* the mutation on a default the preflight had advertised as
 * valid, and a `compile=false` patch left a malformed default behind.
 *
 * These cases assert on three independent oracles: the native pin storage (empty value, null object),
 * the engine's own schema validity call, and a real target compile. The negative case asserts that a
 * reference default the engine refuses is refused at preflight with nothing mutated.
 */
namespace CortexGraphPatchNullDefaultTest
{
UBlueprint* NullDefaultBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	EnsureCortexGraphTestTempContentRoot();
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

void NullDefaultCleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchNullDefaultTestCleanup")));
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

UEdGraph* NullDefaultGraph(UBlueprint* Blueprint)
{
	return Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0].Get() : nullptr;
}

/** A real, resolvable call node on the engine system library, so the pins are production pins. */
UK2Node_CallFunction* NullDefaultCallNode(UEdGraph* Graph, const TCHAR* FunctionName)
{
	if (!Graph) return nullptr;
	UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(FunctionName));
	if (!Function) return nullptr;
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(Function);
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	Graph->AddNode(Node, true, false);
	return Node;
}

TSharedPtr<FJsonObject> NullDefaultRequest(UBlueprint* Blueprint, const TCHAR* PatchId, const bool bCompile, const bool bDryRun)
{
	EnsureCortexGraphTestTempContentRoot();
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), NullDefaultGraph(Blueprint) ? NullDefaultGraph(Blueprint)->GraphGuid.ToString() : FString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	Request->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), bDryRun);
	Request->SetBoolField(TEXT("compile"), bCompile);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

TSharedPtr<FJsonObject> NullDefaultLiteral()
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("null"));
	return Literal;
}

TSharedPtr<FJsonObject> NullDefaultReferenceLiteral(const TCHAR* Kind, const TCHAR* Path)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), Kind);
	Literal->SetStringField(TEXT("path"), Path);
	return Literal;
}

/** Appends a planned node carrying one tagged default. */
void NullDefaultAddNode(
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

/** Appends one pin_updates entry rewriting an existing node's input default. */
void NullDefaultAddPinUpdate(
	const TSharedPtr<FJsonObject>& Request,
	const UEdGraphNode* Node,
	const TCHAR* PinName,
	const TSharedPtr<FJsonObject>& Literal)
{
	TSharedPtr<FJsonObject> Update = MakeShared<FJsonObject>();
	Update->SetStringField(TEXT("node_guid"), Node ? Node->NodeGuid.ToString() : FString());
	Update->SetStringField(TEXT("pin"), PinName);
	Update->SetObjectField(TEXT("default"), Literal);
	TArray<TSharedPtr<FJsonValue>> Updates = Request->GetArrayField(TEXT("pin_updates"));
	Updates.Add(MakeShared<FJsonValueObject>(Update));
	Request->SetArrayField(TEXT("pin_updates"), Updates);
}

UEdGraphNode* NullDefaultFindNode(UBlueprint* Blueprint, const FGuid& Guid)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid) return Node;
		}
	}
	return nullptr;
}

UEdGraphNode* NullDefaultFindCreatedNode(UBlueprint* Blueprint, const FCortexGraphPatchOutcome& Outcome, const TCHAR* ClientId)
{
	const FGuid* Guid = Outcome.Locators.NodeGuidByClientId.Find(ClientId);
	return Guid ? NullDefaultFindNode(Blueprint, *Guid) : nullptr;
}

/** The engine's own verdict on the pin's current default; empty means valid. */
FString NullDefaultSchemaVerdict(const UEdGraphPin* Pin)
{
	const UEdGraphSchema* Schema = Pin ? Pin->GetSchema() : nullptr;
	return Schema ? Schema->IsCurrentPinDefaultValid(Pin) : FString(TEXT("no schema"));
}

/** Asserts the corrected null representation, the engine oracle and the pin storage outcome. */
void NullDefaultCheck(FAutomationTestBase& Test, const UEdGraphPin* Pin, const TCHAR* Context)
{
	Test.TestNotNull(FString::Printf(TEXT("%s: pin resolved"), Context), Pin);
	if (!Pin) return;
	Test.TestTrue(FString::Printf(TEXT("%s: DefaultValue is empty, not the 'None' literal [%s]"), Context, *Pin->DefaultValue),
		Pin->DefaultValue.IsEmpty());
	Test.TestFalse(FString::Printf(TEXT("%s: DefaultValue never keeps the malformed 'None' literal"), Context),
		Pin->DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase));
	Test.TestNull(FString::Printf(TEXT("%s: DefaultObject is null"), Context), Pin->DefaultObject.Get());
	Test.TestTrue(FString::Printf(TEXT("%s: the engine accepts the current default [%s]"), Context, *NullDefaultSchemaVerdict(Pin)),
		NullDefaultSchemaVerdict(Pin).IsEmpty());
}

/** Counts real coordinator compiles through the shared operation observer. */
struct FNullDefaultCompileCounter
{
	int32 Compiles = 0;

	void Begin()
	{
		Compiles = 0;
		Active = this;
		FCortexGraphPatchOps::SetOperationObserverForTesting(
			[](const FName Operation, UBlueprint*)
			{
				if (Active && (Operation == TEXT("target_compile") || Operation == TEXT("recovery_compile")))
				{
					++Active->Compiles;
				}
			});
	}

	void End()
	{
		FCortexGraphPatchOps::ClearOperationObserverForTesting();
		Active = nullptr;
	}

private:
	static FNullDefaultCompileCounter* Active;
};
FNullDefaultCompileCounter* FNullDefaultCompileCounter::Active = nullptr;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchNullDefaultObjectPinTest,
	"Cortex.Graph.Authoring.Patch.NullDefault.ObjectPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchNullDefaultObjectPinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchNullDefaultTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = NullDefaultBlueprint(Package, TEXT("BP_PatchNullDefaultObject_T19"));
	TestNotNull(TEXT("null-default fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		NullDefaultCleanup(Package, Blueprint);
		return false;
	}

	TSharedPtr<FJsonObject> Request = NullDefaultRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000001901"), true, true);
	NullDefaultAddNode(Request, TEXT("nullkind"), TEXT("CallFunction"), TEXT("KismetSystemLibrary.IsValid"), TEXT("Object"), NullDefaultLiteral());
	NullDefaultAddNode(Request, TEXT("pathnone"), TEXT("CallFunction"), TEXT("KismetSystemLibrary.IsValid"), TEXT("Object"), NullDefaultReferenceLiteral(TEXT("object"), TEXT("None")));
	NullDefaultAddNode(Request, TEXT("pathempty"), TEXT("CallFunction"), TEXT("KismetSystemLibrary.IsValid"), TEXT("Object"), NullDefaultReferenceLiteral(TEXT("object"), TEXT("")));

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

	FCortexGraphPatchOutcome Outcome;
	const bool bExecuted = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
	TestTrue(FString::Printf(TEXT("the null defaults apply and verify: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		bExecuted);
	TestEqual(TEXT("the apply ran"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the target compiled after the null defaults"), Outcome.CompileStatus, FString(TEXT("compiled")));
	TestEqual(TEXT("exactly one target compile"), Outcome.TargetCompileCount, 1);
	TestFalse(TEXT("a valid null default is not blocked"), Outcome.bBlocked);

	struct FCase { const TCHAR* ClientId; const TCHAR* Context; };
	const FCase Cases[] =
	{
		{ TEXT("nullkind"), TEXT("null literal") },
		{ TEXT("pathnone"), TEXT("object path 'None'") },
		{ TEXT("pathempty"), TEXT("object path empty") },
	};
	for (const FCase& Case : Cases)
	{
		UEdGraphNode* Node = NullDefaultFindCreatedNode(Blueprint, Outcome, Case.ClientId);
		NullDefaultCheck(*this, Node ? Node->FindPin(TEXT("Object")) : nullptr, Case.Context);
	}

	NullDefaultCleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchNullDefaultCategoryModesTest,
	"Cortex.Graph.Authoring.Patch.NullDefault.ReferenceCategoryModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchNullDefaultCategoryModesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchNullDefaultTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = NullDefaultBlueprint(Package, TEXT("BP_PatchNullDefaultModes_T19"));
	TestNotNull(TEXT("category-modes fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		NullDefaultCleanup(Package, Blueprint);
		return false;
	}
	UEdGraph* Graph = NullDefaultGraph(Blueprint);
	UK2Node_CallFunction* HardObject = NullDefaultCallNode(Graph, TEXT("IsValid"));
	UK2Node_CallFunction* HardClass = NullDefaultCallNode(Graph, TEXT("IsValidClass"));
	UK2Node_CallFunction* SoftObject = NullDefaultCallNode(Graph, TEXT("IsValidSoftObjectReference"));
	UK2Node_CallFunction* SoftClass = NullDefaultCallNode(Graph, TEXT("IsValidSoftClassReference"));
	TestNotNull(TEXT("hard object fixture node created"), HardObject);
	TestNotNull(TEXT("hard class fixture node created"), HardClass);
	TestNotNull(TEXT("soft object fixture node created"), SoftObject);
	TestNotNull(TEXT("soft class fixture node created"), SoftClass);
	if (!HardObject || !HardClass || !SoftObject || !SoftClass)
	{
		NullDefaultCleanup(Package, Blueprint);
		return false;
	}

	// The declared categories are exactly the four reference storage modes, so the same null must be
	// written in each one's own storage.
	TestEqual(TEXT("hard object pin category"), HardObject->FindPin(TEXT("Object"))->PinType.PinCategory.ToString(),
		UEdGraphSchema_K2::PC_Object.ToString());
	TestEqual(TEXT("hard class pin category"), HardClass->FindPin(TEXT("Class"))->PinType.PinCategory.ToString(),
		UEdGraphSchema_K2::PC_Class.ToString());
	TestEqual(TEXT("soft object pin category"), SoftObject->FindPin(TEXT("SoftObjectReference"))->PinType.PinCategory.ToString(),
		UEdGraphSchema_K2::PC_SoftObject.ToString());
	TestEqual(TEXT("soft class pin category"), SoftClass->FindPin(TEXT("SoftClassReference"))->PinType.PinCategory.ToString(),
		UEdGraphSchema_K2::PC_SoftClass.ToString());

	TSharedPtr<FJsonObject> Request = NullDefaultRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000001902"), true, true);
	NullDefaultAddPinUpdate(Request, HardObject, TEXT("Object"), NullDefaultLiteral());
	NullDefaultAddPinUpdate(Request, HardClass, TEXT("Class"), NullDefaultLiteral());
	NullDefaultAddPinUpdate(Request, SoftObject, TEXT("SoftObjectReference"), NullDefaultReferenceLiteral(TEXT("soft_object"), TEXT("None")));
	NullDefaultAddPinUpdate(Request, SoftClass, TEXT("SoftClassReference"), NullDefaultReferenceLiteral(TEXT("soft_class"), TEXT("")));

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("category-modes preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

	FCortexGraphPatchOutcome Outcome;
	const bool bExecuted = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
	TestTrue(FString::Printf(TEXT("every category writes a valid null: %s [%s]"), *Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
		bExecuted);
	TestEqual(TEXT("the category apply ran"), Outcome.ApplyStatus, FString(TEXT("applied")));
	TestEqual(TEXT("the category readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
	TestEqual(TEXT("the target compiled after every category null"), Outcome.CompileStatus, FString(TEXT("compiled")));

	NullDefaultCheck(*this, HardObject->FindPin(TEXT("Object")), TEXT("object pin null"));
	NullDefaultCheck(*this, HardClass->FindPin(TEXT("Class")), TEXT("class pin null"));
	NullDefaultCheck(*this, SoftObject->FindPin(TEXT("SoftObjectReference")), TEXT("soft object pin null"));
	NullDefaultCheck(*this, SoftClass->FindPin(TEXT("SoftClassReference")), TEXT("soft class pin null"));

	NullDefaultCleanup(Package, Blueprint);
	return true;
}

/**
 * The engine-forbidden reference default: a resolved class/object the pin's declared type does not
 * accept. The refusal must happen at preflight, with nothing mutated, no dirty state change and no
 * compile, instead of being accepted and then failing the compile after the mutation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchNullDefaultProhibitedReferenceTest,
	"Cortex.Graph.Authoring.Patch.NullDefault.ProhibitedReferenceRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchNullDefaultProhibitedReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchNullDefaultTest;

	// (a) a class default whose resolved class does not derive from the pin's declared base class
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = NullDefaultBlueprint(Package, TEXT("BP_PatchNullDefaultProhibitedClass_T19"));
		TestNotNull(TEXT("prohibited-class fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			UEdGraph* Graph = NullDefaultGraph(Blueprint);
			UK2Node_CallFunction* Node = NullDefaultCallNode(Graph, TEXT("DoesImplementInterface"));
			UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("Interface")) : nullptr;
			TestNotNull(TEXT("declared-base class pin resolved"), Pin);
			if (Pin)
			{
				const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
				const int32 NodesBefore = Graph->Nodes.Num();
				Blueprint->GetOutermost()->SetDirtyFlag(false);

				TSharedPtr<FJsonObject> Request = NullDefaultRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000001903"), true, false);
				Request->SetStringField(TEXT("expected_validation_hash"), TEXT("0000000000000000000000000000000000000000"));
				NullDefaultAddPinUpdate(Request, Node, TEXT("Interface"), NullDefaultReferenceLiteral(TEXT("class"), TEXT("/Script/Engine.Actor")));

				FNullDefaultCompileCounter Counter;
				Counter.Begin();
				FCortexGraphPatchOutcome Outcome;
				FCortexCommandResult Error;
				const bool bApplied = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
				Counter.End();

				TestFalse(TEXT("a class the declared base refuses is not applied"), bApplied);
				TestEqual(TEXT("the refusal is a type mismatch"), Error.ErrorCode, FString(CortexErrorCodes::TypeMismatch));
				TestTrue(TEXT("the refusal names the declared base"),
					Error.ErrorMessage.Contains(TEXT("inherit")));
				TestEqual(TEXT("a refused reference default applies nothing"), Outcome.ApplyStatus, FString(TEXT("not_requested")));
				TestEqual(TEXT("a refused reference default never compiles"), Counter.Compiles, 0);
				TestEqual(TEXT("a refused reference default mutates no node"), Graph->Nodes.Num(), NodesBefore);
				TestNull(TEXT("a refused reference default leaves the pin untouched"), Pin->DefaultObject.Get());
				TestTrue(TEXT("a refused reference default keeps the pin's own default valid"), NullDefaultSchemaVerdict(Pin).IsEmpty());
				TestEqual(TEXT("a refused reference default leaves the authoring hash unchanged"),
					FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
				TestFalse(TEXT("a refused reference default leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
			}
			NullDefaultCleanup(Package, Blueprint);
		}
	}

	// (b) an object default the pin's declared object class refuses
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = NullDefaultBlueprint(Package, TEXT("BP_PatchNullDefaultProhibitedObject_T19"));
		TestNotNull(TEXT("prohibited-object fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			UEdGraph* Graph = NullDefaultGraph(Blueprint);
			UK2Node_CallFunction* Node = NullDefaultCallNode(Graph, TEXT("GetActorBounds"));
			UEdGraphPin* Pin = Node ? Node->FindPin(TEXT("Actor")) : nullptr;
			TestNotNull(TEXT("declared-class object pin resolved"), Pin);
			if (Pin)
			{
				const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
				const int32 NodesBefore = Graph->Nodes.Num();

				TSharedPtr<FJsonObject> Request = NullDefaultRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000001904"), true, true);
				NullDefaultAddPinUpdate(Request, Node, TEXT("Actor"), NullDefaultReferenceLiteral(TEXT("object"), TEXT("/Script/Engine.SceneComponent")));

				FCortexGraphPreparedPatch Preview;
				FCortexCommandResult Error;
				TestFalse(TEXT("an object the declared class refuses is refused at preview"),
					FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
				TestEqual(TEXT("the object refusal is a type mismatch"), Error.ErrorCode, FString(CortexErrorCodes::TypeMismatch));
				TestEqual(TEXT("an object refusal mutates no node"), Graph->Nodes.Num(), NodesBefore);
				TestNull(TEXT("an object refusal leaves the pin untouched"), Pin->DefaultObject.Get());
				TestEqual(TEXT("an object refusal leaves the authoring hash unchanged"),
					FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
				TestFalse(TEXT("an object refusal writes no bytes to disk"),
					FPackageName::DoesPackageExist(Blueprint->GetOutermost()->GetName()));
			}
			NullDefaultCleanup(Package, Blueprint);
		}
	}

	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
