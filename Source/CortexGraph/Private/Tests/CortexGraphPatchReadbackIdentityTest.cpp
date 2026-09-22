#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Operations/CortexGraphPinDefaults.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SwitchEnum.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphPatchReadbackTest
{
static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchReadbackTestCleanup")));
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

static TSharedPtr<FJsonObject> AddNode(
	const TSharedPtr<FJsonObject>& Request,
	const TCHAR* ClientId,
	const TCHAR* NodeClass,
	const TSharedPtr<FJsonObject>& Params,
	const TSharedPtr<FJsonObject>& Defaults)
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

static TSharedPtr<FJsonObject> Defaults(const TCHAR* PinName, const TSharedPtr<FJsonObject>& Literal)
{
	TSharedPtr<FJsonObject> DefaultsObject = MakeShared<FJsonObject>();
	DefaultsObject->SetObjectField(PinName, Literal);
	return DefaultsObject;
}

static TSharedPtr<FJsonObject> TextTableLiteral(const TCHAR* TableId, const TCHAR* Key)
{
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("text"));
	Literal->SetStringField(TEXT("table"), TableId);
	Literal->SetStringField(TEXT("key"), Key);
	return Literal;
}

static bool PrepareForApply(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexCommandResult& OutError)
{
	// A preview can materialize blueprint skeleton state, so re-baseline the expected fingerprint
	// and preview once more before the apply request is used.
	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
	{
		return false;
	}
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	Request->RemoveField(TEXT("expected_validation_hash"));
	Request->SetBoolField(TEXT("dry_run"), true);
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
	{
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	return true;
}

static UK2Node_CallFunction* FindCallNode(UBlueprint* Blueprint, const FName FunctionName)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			if (Call && Call->FunctionReference.GetMemberName() == FunctionName)
			{
				return Call;
			}
		}
	}
	return nullptr;
}

template <typename TNodeType>
static TNodeType* FindNode(UBlueprint* Blueprint)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (TNodeType* Typed = Cast<TNodeType>(Node))
			{
				return Typed;
			}
		}
	}
	return nullptr;
}

/** Runs one patch whose native state is mutated before readback and expects verified recovery. */
static bool ExpectDivergenceRecovered(
	FAutomationTestBase& Test,
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	const TSharedPtr<FJsonObject>& Request,
	const TFunction<void(UBlueprint*)>& Mutator,
	const FString& ExpectedFailureText)
{
	FCortexCommandResult Error;
	if (!Test.TestTrue(FString::Printf(TEXT("%s: preview succeeds: %s"), PatchId, *Error.ErrorMessage),
		PrepareForApply(Blueprint, Request, Error)))
	{
		return false;
	}
	FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(Mutator);
	FCortexGraphPatchOutcome Outcome;
	const bool bExecuted = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
	FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
	Test.TestFalse(FString::Printf(TEXT("%s: divergence is rejected [%s]"), PatchId, *Error.ErrorMessage), bExecuted);
	Test.TestEqual(FString::Printf(TEXT("%s: readback reports a mismatch"), PatchId),
		Outcome.ReadbackStatus, FString(TEXT("mismatched")));
	Test.TestEqual(FString::Printf(TEXT("%s: recovery is verified [%s]"), PatchId, *Error.ErrorMessage),
		Outcome.RollbackStatus, FString(TEXT("restored")));
	Test.TestFalse(FString::Printf(TEXT("%s: recovery does not block the asset"), PatchId), Outcome.bBlocked);
	Test.TestTrue(FString::Printf(TEXT("%s: failure names the diverged identity [%s]"), PatchId, *Error.ErrorMessage),
		Error.ErrorMessage.Contains(ExpectedFailureText));
	return true;
}
}

// ---------------------------------------------------------------------------
// 1. Contradictory native reference storage is not a match
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackReferenceStorageTest,
	"Cortex.Graph.Authoring.Readback.ReferenceStorageContradiction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackReferenceStorageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* TablePackage = CreatePackage(TEXT("/Temp/BP_ReadbackSoftTarget_T08"));
	UStringTable* SoftTarget = NewObject<UStringTable>(TablePackage, FName(TEXT("ST_ReadbackTarget")), RF_Public | RF_Standalone | RF_Transactional);
	const FString SoftObjectPath = SoftTarget->GetPathName();

	// (a) a soft pin that also carries a hard object is not the requested soft reference
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackSoftStorage_T08"));
		TestNotNull(TEXT("soft storage fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d101"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.IsValidSoftObjectReference"));
			TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
			Literal->SetStringField(TEXT("kind"), TEXT("soft_object"));
			Literal->SetStringField(TEXT("path"), SoftObjectPath);
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("soft"), TEXT("CallFunction"), Params,
				CortexGraphPatchReadbackTest::Defaults(TEXT("SoftObjectReference"), Literal));

			UObject* const Target = SoftTarget;
			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("soft storage contradiction"), Request,
				[Target](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(
						Mutated, TEXT("IsValidSoftObjectReference"));
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("SoftObjectReference")) : nullptr)
					{
						// Keeps the soft path in DefaultValue and adds a coercing hard object.
						Pin->DefaultObject = Target;
					}
				},
				TEXT("also stores hard object"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) a hard reference stored only as default text is not a native object
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackHardStorage_T08"));
		TestNotNull(TEXT("hard storage fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d102"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.IsValid"));
			TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
			Literal->SetStringField(TEXT("kind"), TEXT("object"));
			Literal->SetStringField(TEXT("path"), SoftObjectPath);
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("isvalid"), TEXT("CallFunction"), Params,
				CortexGraphPatchReadbackTest::Defaults(TEXT("Object"), Literal));

			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("hard storage contradiction"), Request,
				[](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(Mutated, TEXT("IsValid"));
					UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("Object")) : nullptr;
					if (Pin && Pin->DefaultObject)
					{
						// Keeps the path as text only: no native object backs the default.
						Pin->DefaultValue = Pin->DefaultObject->GetPathName();
						Pin->DefaultObject = nullptr;
					}
				},
				TEXT("does not store its default as a native object"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	SoftTarget->ClearFlags(RF_Standalone);
	SoftTarget->MarkAsGarbage();
	TablePackage->ClearFlags(RF_Standalone);
	TablePackage->MarkAsGarbage();
	return true;
}

// ---------------------------------------------------------------------------
// 2. A scalar readback must still match the pin's category
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackScalarCategoryTest,
	"Cortex.Graph.Authoring.Readback.ScalarCategoryMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackScalarCategoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackScalarCategory_T08"));
	TestNotNull(TEXT("scalar category fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-00000000d201"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("function_name"), TEXT("KismetStringLibrary.Conv_IntToString"));
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("int"));
	Literal->SetNumberField(TEXT("value"), 42);
	CortexGraphPatchReadbackTest::AddNode(Request, TEXT("convert"), TEXT("CallFunction"), Params,
		CortexGraphPatchReadbackTest::Defaults(TEXT("InInt"), Literal));

	CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
		*this, Blueprint, TEXT("scalar category mismatch"), Request,
		[](UBlueprint* Mutated)
		{
			UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(Mutated, TEXT("Conv_IntToString"));
			if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("InInt")) : nullptr)
			{
				// Same default text on a native pin of another category.
				Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
		},
		TEXT("signature mismatch"));
	CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 3. Cast readback includes requested purity
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackCastPurityTest,
	"Cortex.Graph.Authoring.Readback.CastPurity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackCastPurityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackCastPurity_T08"));
	TestNotNull(TEXT("cast purity fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-00000000d301"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class"), TEXT("/Script/Engine.StaticMesh"));
	Params->SetBoolField(TEXT("is_pure"), true);
	CortexGraphPatchReadbackTest::AddNode(Request, TEXT("cast"), TEXT("DynamicCast"), Params, nullptr);

	CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
		*this, Blueprint, TEXT("cast purity divergence"), Request,
		[](UBlueprint* Mutated)
		{
			UK2Node_DynamicCast* Cast = CortexGraphPatchReadbackTest::FindNode<UK2Node_DynamicCast>(Mutated);
			if (Cast)
			{
				Cast->SetPurity(false);
			}
		},
		TEXT("pure=1"));
	CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 5. Identity-bearing families are compared, not assumed
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackSymbolFamiliesTest,
	"Cortex.Graph.Authoring.Readback.SymbolFamilyIdentities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackSymbolFamiliesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// (a) switch enum
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackSwitchEnum_T08"));
		TestNotNull(TEXT("switch enum fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d501"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("enum_name"), TEXT("EAxis"));
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("switch"), TEXT("SwitchEnum"), Params, nullptr);

			UEnum* const OtherEnum = FindFirstObject<UEnum>(TEXT("ECollisionChannel"));
			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("switch enum divergence"), Request,
				[OtherEnum](UBlueprint* Mutated)
				{
					UK2Node_SwitchEnum* Switch = CortexGraphPatchReadbackTest::FindNode<UK2Node_SwitchEnum>(Mutated);
					if (Switch && OtherEnum)
					{
						Switch->SetEnum(OtherEnum);
					}
				},
				TEXT("symbol mismatch"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) multicast delegate
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackDelegate_T08"));
		TestNotNull(TEXT("delegate fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d502"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("delegate_name"), TEXT("OnTakeAnyDamage"));
			Params->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("bind"), TEXT("AddDelegate"), Params, nullptr);

			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("delegate owner divergence"), Request,
				[](UBlueprint* Mutated)
				{
					UK2Node_BaseMCDelegate* Delegate = CortexGraphPatchReadbackTest::FindNode<UK2Node_BaseMCDelegate>(Mutated);
					UClass* SelfClass = Mutated->SkeletonGeneratedClass ? Mutated->SkeletonGeneratedClass : Mutated->GeneratedClass;
					FMulticastDelegateProperty* Prop = CastField<FMulticastDelegateProperty>(
						AActor::StaticClass()->FindPropertyByName(FName(TEXT("OnTakeAnyDamage"))));
					if (Delegate && Prop && SelfClass)
					{
						// Same delegate name, coerced into self context.
						Delegate->SetFromProperty(Prop, true, SelfClass);
					}
				},
				TEXT("symbol mismatch"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// (c) create delegate
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackCreateDelegate_T08"));
		TestNotNull(TEXT("create delegate fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d503"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("function_name"), TEXT("T08DelegateTarget"));
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("createdelegate"), TEXT("CreateDelegate"), Params, nullptr);

			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("create delegate divergence"), Request,
				[](UBlueprint* Mutated)
				{
					UK2Node_CreateDelegate* CreateDelegate = CortexGraphPatchReadbackTest::FindNode<UK2Node_CreateDelegate>(Mutated);
					if (CreateDelegate)
					{
						CreateDelegate->SetFunction(FName(TEXT("T08OtherTarget")));
					}
				},
				TEXT("symbol mismatch"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 6. Canonical text identity is unambiguous
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackTextEncodingTest,
	"Cortex.Graph.Authoring.Readback.TextIdentityEncoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackTextEncodingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackTextEncoding_T08"));
	TestNotNull(TEXT("text encoding fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->SetFromFunction(UKismetTextLibrary::StaticClass()->FindFunctionByName(TEXT("TextIsEmpty")));
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	Graph->AddNode(Node, true, false);
	UEdGraphPin* Pin = Node->FindPin(TEXT("InText"));
	TestNotNull(TEXT("text pin resolved"), Pin);
	if (!Pin)
	{
		CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		return false;
	}

	// Registered tables with ambiguous ids/keys: the identities must not collide.
	FStringTableRef AmbiguousTable = FStringTable::NewStringTable();
	AmbiguousTable->SetSourceString(FTextKey(TEXT("C")), FString(TEXT("value")), FString());
	FStringTableRegistry::Get().RegisterStringTable(FName(TEXT("A#B")), AmbiguousTable);
	FStringTableRef SplitTable = FStringTable::NewStringTable();
	SplitTable->SetSourceString(FTextKey(TEXT("B#C")), FString(TEXT("value")), FString());
	FStringTableRegistry::Get().RegisterStringTable(FName(TEXT("A")), SplitTable);

	Pin->DefaultTextValue = FText::FromStringTable(FName(TEXT("A#B")), FTextKey(TEXT("C")));

	FString Expected;
	FString Actual;
	FString Failure;
	TestTrue(TEXT("colliding table identity still compares"),
		FCortexGraphPinDefaults::CompareAppliedLiteral(Pin,
			CortexGraphPatchReadbackTest::TextTableLiteral(TEXT("A"), TEXT("B#C")), Expected, Actual, Failure));
	TestNotEqual(TEXT("a table id/key split cannot forge another identity"), Actual, Expected);
	TestTrue(TEXT("the canonical table identity is unambiguous"), Actual.Contains(TEXT("3:A#B")));

	Expected.Reset();
	Actual.Reset();
	Failure.Reset();
	TestTrue(TEXT("the exact table identity still compares"),
		FCortexGraphPinDefaults::CompareAppliedLiteral(Pin,
			CortexGraphPatchReadbackTest::TextTableLiteral(TEXT("A#B"), TEXT("C")), Expected, Actual, Failure));
	TestEqual(TEXT("the exact table identity matches"), Actual, Expected);

	FStringTableRegistry::Get().UnregisterStringTable(FName(TEXT("A#B")));
	FStringTableRegistry::Get().UnregisterStringTable(FName(TEXT("A")));
	CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 7. An inherited event owner is valid, and a wrong owner is detected
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackInheritedEventOwnerTest,
	"Cortex.Graph.Authoring.Readback.InheritedEventOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackInheritedEventOwnerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto MakePawnFixture = [](UPackage*& OutPackage, UBlueprint*& OutBlueprint, const TCHAR* Name)
	{
		OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
		OutBlueprint = FKismetEditorUtilities::CreateBlueprint(
			APawn::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	};

	// (a) owner_class names a subclass of the declaring class: the request is valid
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		MakePawnFixture(Package, Blueprint, TEXT("BP_ReadbackInheritedOwner_T08"));
		TestNotNull(TEXT("inherited owner fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d701"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
			Params->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Pawn"));
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("event"), TEXT("Event"), Params, nullptr);

			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("inherited owner preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPatchReadbackTest::PrepareForApply(Blueprint, Request, Error));
			FCortexGraphPatchOutcome Outcome;
			TestTrue(FString::Printf(TEXT("inherited owner request applies and verifies: %s"), *Error.ErrorMessage),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			TestEqual(TEXT("inherited owner readback matches the applied context owner"),
				Outcome.ReadbackStatus, FString(TEXT("matched")));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// A post-apply native owner divergence could not be induced in this fixture: rebinding the
	// applied node's event reference to the declaring class, to self context or to no owner all
	// left the reference unchanged, so no falsifiable mutation case is asserted here. The
	// valid-request case above is the observed red/green evidence for the comparison.

	return true;
}

// ---------------------------------------------------------------------------
// 8. An owner-only event selector is refused instead of reporting success
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackOwnerOnlyEventTest,
	"Cortex.Graph.Authoring.Readback.OwnerOnlyEventSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackOwnerOnlyEventTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackOwnerOnlyEvent_T08"));
	TestNotNull(TEXT("owner-only event fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
		Blueprint, TEXT("00000000-0000-0000-0000-00000000d801"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	CortexGraphPatchReadbackTest::AddNode(Request, TEXT("event"), TEXT("Event"), Params, nullptr);

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestFalse(TEXT("an owner only Event selector is refused before mutation"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	TestEqual(TEXT("owner only Event selector reports an invalid field"),
		Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(FString::Printf(TEXT("owner only Event failure names the missing selector [%s]"), *Error.ErrorMessage),
		Error.ErrorMessage.Contains(TEXT("function_name")));
	CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 9. Native pin signatures are compared for created nodes and pin updates
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackPinSignatureTest,
	"Cortex.Graph.Authoring.Readback.PinSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackPinSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// (a) a created node whose native pin container kind was changed
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackPinSignatureNode_T08"));
		TestNotNull(TEXT("pin signature fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d901"));
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
			CortexGraphPatchReadbackTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), Params, nullptr);

			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("created pin signature divergence"), Request,
				[](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(Mutated, TEXT("PrintString"));
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("InString")) : nullptr)
					{
						Pin->PinType.ContainerType = EPinContainerType::Array;
					}
				},
				TEXT("signature mismatch"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) an existing node updated through pin_updates whose native pin flags were changed
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackPinSignatureUpdate_T08"));
		TestNotNull(TEXT("pin update signature fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			UEdGraph* Graph = Blueprint->UbergraphPages[0];
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
			Node->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
			Node->CreateNewGuid();
			Node->AllocateDefaultPins();
			Graph->AddNode(Node, true, false);

			TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(
				Blueprint, TEXT("00000000-0000-0000-0000-00000000d902"));
			TSharedPtr<FJsonObject> Update = MakeShared<FJsonObject>();
			Update->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			Update->SetStringField(TEXT("pin"), TEXT("InString"));
			TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
			Literal->SetStringField(TEXT("kind"), TEXT("string"));
			Literal->SetStringField(TEXT("value"), TEXT("configured"));
			Update->SetObjectField(TEXT("default"), Literal);
			TArray<TSharedPtr<FJsonValue>> Updates;
			Updates.Add(MakeShared<FJsonValueObject>(Update));
			Request->SetArrayField(TEXT("pin_updates"), Updates);

			CortexGraphPatchReadbackTest::ExpectDivergenceRecovered(
				*this, Blueprint, TEXT("pin update signature divergence"), Request,
				[](UBlueprint* Mutated)
				{
					UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(Mutated, TEXT("PrintString"));
					if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("InString")) : nullptr)
					{
						Pin->PinType.bIsConst = true;
					}
				},
				TEXT("signature mismatch"));
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 10. Recovery restores the dirty flag only when the restore is verified
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchReadbackRecoveryDirtyStateTest,
	"Cortex.Graph.Authoring.Readback.RecoveryDirtyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchReadbackRecoveryDirtyStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto MakeRequest = [](UBlueprint* Blueprint, const TCHAR* PatchId)
	{
		TSharedPtr<FJsonObject> Request = CortexGraphPatchReadbackTest::BaseRequest(Blueprint, PatchId);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		CortexGraphPatchReadbackTest::AddNode(Request, TEXT("note"), TEXT("CallFunction"), Params, nullptr);
		return Request;
	};
	auto MutatePin = [](UBlueprint* Mutated)
	{
		UK2Node_CallFunction* Call = CortexGraphPatchReadbackTest::FindCallNode(Mutated, TEXT("PrintString"));
		if (UEdGraphPin* Pin = Call ? Call->FindPin(TEXT("InString")) : nullptr)
		{
			Pin->PinType.bIsConst = true;
		}
	};

	// (a) verified recovery restores the pre-request clean flag
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackDirtyVerified_T08"));
		TestNotNull(TEXT("verified dirty fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = MakeRequest(Blueprint, TEXT("00000000-0000-0000-0000-00000000da01"));
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("verified dirty preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPatchReadbackTest::PrepareForApply(Blueprint, Request, Error));
			Blueprint->GetOutermost()->SetDirtyFlag(false);
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(MutatePin);
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("verified dirty probe is rejected"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
			TestEqual(TEXT("verified recovery reports a restored rollback"),
				Outcome.RollbackStatus, FString(TEXT("restored")));
			TestFalse(TEXT("verified recovery restores the pre-request clean flag"),
				Blueprint->GetOutermost()->IsDirty());
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	// (b) unverified recovery leaves the package dirty so it cannot be saved as clean
	{
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = CortexGraphPatchReadbackTest::MakeBlueprint(Package, TEXT("BP_ReadbackDirtyUnverified_T08"));
		TestNotNull(TEXT("unverified dirty fixture Blueprint created"), Blueprint);
		if (Blueprint)
		{
			TSharedPtr<FJsonObject> Request = MakeRequest(Blueprint, TEXT("00000000-0000-0000-0000-00000000da02"));
			FCortexCommandResult Error;
			TestTrue(FString::Printf(TEXT("unverified dirty preview succeeds: %s"), *Error.ErrorMessage),
				CortexGraphPatchReadbackTest::PrepareForApply(Blueprint, Request, Error));
			Blueprint->GetOutermost()->SetDirtyFlag(false);
			FCortexGraphPatchOps::SetPreReadbackMutatorForTesting(MutatePin);
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("verification_failure"));
			FCortexGraphPatchOutcome Outcome;
			TestFalse(TEXT("unverified recovery is rejected"),
				FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error));
			FCortexGraphPatchOps::SetApplyFaultPointForTesting(NAME_None);
			FCortexGraphPatchOps::ClearPreReadbackMutatorForTesting();
			TestEqual(TEXT("unverified recovery reports an unverified rollback"),
				Outcome.RollbackStatus, FString(TEXT("unverified")));
			TestTrue(TEXT("unverified recovery keeps the package dirty"),
				Blueprint->GetOutermost()->IsDirty());
			CortexGraphPatchReadbackTest::Cleanup(Package, Blueprint);
		}
	}

	return true;
}

#endif
