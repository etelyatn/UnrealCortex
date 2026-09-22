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
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextKey.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
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
		TEXT("no longer validates against the pin"));
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

#endif
