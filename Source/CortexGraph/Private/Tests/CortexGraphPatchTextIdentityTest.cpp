#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextKey.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * The authoring fingerprint must identify an FText pin default by its canonical text identity, not by
 * the string it currently displays.
 *
 * A string table entry and a literal (and two different tables) can display the same text while being
 * different defaults. When only the display string reached the hash, an already-dirty asset could have
 * that pin changed between a preview and its apply without the hash moving, so the stale request was
 * accepted and overwrote the intervening edit instead of reporting STALE_PRECONDITION.
 *
 * These cases assert on the real hash AND on the underlying text identity, so the test cannot pass by
 * accident when both identities happen to serialize the same way.
 */
namespace CortexGraphPatchTextIdentityTest
{
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;

	bool Create(const TCHAR* Name)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint) return false;
		Graph = Blueprint->UbergraphPages.Num() > 0
			? Blueprint->UbergraphPages[0].Get()
			: FBlueprintEditorUtils::FindEventGraph(Blueprint);
		return Graph != nullptr;
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
};

/** A registered transient string table used as a real string-table identity. */
struct FTable
{
	UPackage* Package = nullptr;
	UStringTable* Table = nullptr;

	bool Create(const TCHAR* PackageName, const TCHAR* TableName, const TCHAR* SourceString)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), PackageName));
		Table = NewObject<UStringTable>(Package, FName(TableName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Table) return false;
		Table->GetMutableStringTable()->SetSourceString(FTextKey(TEXT("Key")), FString(SourceString), FString());
		FStringTableRegistry::Get().RegisterStringTable(Table->GetStringTableId(), Table->GetMutableStringTable());
		return true;
	}

	void Reset()
	{
		if (Table)
		{
			FStringTableRegistry::Get().UnregisterStringTable(Table->GetStringTableId());
			Table->ClearFlags(RF_Standalone);
			Table->MarkAsGarbage();
			Table = nullptr;
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
			Package = nullptr;
		}
	}
};

/** An FText input pin on a real call node, so the fingerprint sees it as production code does. */
UEdGraphPin* AddTextPin(UEdGraph* Graph, UEdGraphNode*& OutNode)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(
		FName(TEXT("Conv_TextToString")), UKismetTextLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = 300;
	Call->NodePosY = 0;
	Graph->AddNode(Call, true, false);
	OutNode = Call;
	return Call->FindPin(FName(TEXT("InText")));
}

FString GraphHash(UBlueprint* Blueprint)
{
	const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchTextIdentityFingerprintTest,
	"Cortex.Graph.Authoring.Patch.TextIdentityFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchTextIdentityFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchTextIdentityTest;

	FFixture Fixture;
	FTable First;
	FTable Second;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchTextIdentity_T18")));
	TestTrue(TEXT("first string table created"), First.Create(TEXT("ST_PatchTextIdentityA_T18"), TEXT("ST_PatchTextIdentityA_T18"), TEXT("Shared")));
	TestTrue(TEXT("second string table created"), Second.Create(TEXT("ST_PatchTextIdentityB_T18"), TEXT("ST_PatchTextIdentityB_T18"), TEXT("Shared")));
	if (!Fixture.Blueprint || !First.Table || !Second.Table)
	{
		First.Reset();
		Second.Reset();
		Fixture.Cleanup();
		return false;
	}

	UEdGraphNode* Node = nullptr;
	UEdGraphPin* Pin = AddTextPin(Fixture.Graph, Node);
	TestNotNull(TEXT("the FText input pin exists"), Pin);
	if (!Pin)
	{
		First.Reset();
		Second.Reset();
		Fixture.Cleanup();
		return false;
	}

	const FText TableText = FText::FromStringTable(First.Table->GetStringTableId(), FString(TEXT("Key")));
	const FText SecondTableText = FText::FromStringTable(Second.Table->GetStringTableId(), FString(TEXT("Key")));
	const FText LiteralText = FText::FromString(TEXT("Shared"));

	// The premise of the defect: all three render identically, so display text cannot distinguish them.
	TestEqual(TEXT("the string table entry displays as the literal"),
		TableText.ToString(), LiteralText.ToString());
	TestEqual(TEXT("the second table entry displays identically too"),
		SecondTableText.ToString(), LiteralText.ToString());

	Pin->DefaultTextValue = TableText;
	const FString TableHash = GraphHash(Fixture.Blueprint);
	Pin->DefaultTextValue = LiteralText;
	const FString LiteralHash = GraphHash(Fixture.Blueprint);
	TestTrue(TEXT("the fingerprint moves when the default changes from a table entry to a literal"),
		TableHash != LiteralHash);

	Pin->DefaultTextValue = SecondTableText;
	const FString SecondTableHash = GraphHash(Fixture.Blueprint);
	TestTrue(TEXT("the fingerprint moves between two tables with the same key and display text"),
		SecondTableHash != TableHash);
	TestTrue(TEXT("the fingerprint distinguishes the second table from the literal"),
		SecondTableHash != LiteralHash);

	// A no-op must stay stable, or the token would be unusable.
	Pin->DefaultTextValue = TableText;
	const FString TableHashAgain = GraphHash(Fixture.Blueprint);
	TestEqual(TEXT("re-applying the same identity hashes identically"), TableHashAgain, TableHash);

	First.Reset();
	Second.Reset();
	Fixture.Cleanup();
	return true;
}

/**
 * The routed consequence of the same defect: a preview whose approval set was taken before the text
 * identity moved must be refused as stale, even though the displayed text is unchanged. The asset is
 * already dirty, so a "saved bytes did not change" oracle could not detect the intervening edit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchTextIdentityStaleApplyTest,
	"Cortex.Graph.Authoring.Patch.TextIdentityStaleApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchTextIdentityStaleApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchTextIdentityTest;

	FFixture Fixture;
	FTable First;
	FTable Second;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchTextIdentityStale_T18")));
	TestTrue(TEXT("first string table created"), First.Create(TEXT("ST_PatchTextStaleA_T18"), TEXT("ST_PatchTextStaleA_T18"), TEXT("Shared")));
	TestTrue(TEXT("second string table created"), Second.Create(TEXT("ST_PatchTextStaleB_T18"), TEXT("ST_PatchTextStaleB_T18"), TEXT("Shared")));
	if (!Fixture.Blueprint || !First.Table || !Second.Table)
	{
		First.Reset();
		Second.Reset();
		Fixture.Cleanup();
		return false;
	}

	UEdGraphNode* Node = nullptr;
	UEdGraphPin* Pin = AddTextPin(Fixture.Graph, Node);
	TestNotNull(TEXT("the FText input pin exists"), Pin);
	if (!Pin)
	{
		First.Reset();
		Second.Reset();
		Fixture.Cleanup();
		return false;
	}
	Pin->DefaultTextValue = FText::FromStringTable(First.Table->GetStringTableId(), FString(TEXT("Key")));
	Fixture.Package->SetDirtyFlag(true);
	TestTrue(TEXT("the asset starts dirty"), Fixture.Package->IsDirty());

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-00000000e018"));
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Fixture.Graph->GraphGuid.ToString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	// Exactly the fingerprint a caller would have retained from the preview.
	const TSharedPtr<FJsonObject> PreviewFingerprint = FCortexGraphPatchState::ComputeFingerprint(Fixture.Blueprint);
	Request->SetObjectField(TEXT("expected_fingerprint"), PreviewFingerprint);
	Request->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), true);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	const FCortexCommandResult Preview = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestTrue(FString::Printf(TEXT("preview succeeds on the dirty asset: %s"), *Preview.ErrorMessage), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid())
	{
		First.Reset();
		Second.Reset();
		Fixture.Cleanup();
		return false;
	}
	const FString Token = Preview.Data->GetStringField(TEXT("validation_hash"));

	// The intervening edit: a different table identity that still displays the same text, on the same
	// dirty package, with no save in between.
	Pin->DefaultTextValue = FText::FromStringTable(Second.Table->GetStringTableId(), FString(TEXT("Key")));
	TestTrue(TEXT("the intervening identity edit moved the fingerprint"),
		GraphHash(Fixture.Blueprint)
			!= PreviewFingerprint->GetStringField(TEXT("graph_authoring_hash")));

	TSharedPtr<FJsonObject> ApplyRequest = MakeShared<FJsonObject>();
	ApplyRequest->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	ApplyRequest->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-00000000e018"));
	ApplyRequest->SetObjectField(TEXT("target"), Target);
	ApplyRequest->SetObjectField(TEXT("expected_fingerprint"), PreviewFingerprint);
	ApplyRequest->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
	ApplyRequest->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	ApplyRequest->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	ApplyRequest->SetBoolField(TEXT("dry_run"), false);
	ApplyRequest->SetBoolField(TEXT("compile"), true);
	ApplyRequest->SetBoolField(TEXT("save"), false);
	ApplyRequest->SetBoolField(TEXT("allow_noop"), true);
	ApplyRequest->SetStringField(TEXT("expected_validation_hash"), Token);

	const FCortexCommandResult Stale = Router.Execute(TEXT("graph.apply_patch"), ApplyRequest);
	TestFalse(TEXT("the stale approval is refused"), Stale.bSuccess);
	TestEqual(TEXT("the stale approval reports STALE_PRECONDITION"), Stale.ErrorCode,
		FString(CortexErrorCodes::StalePrecondition));

	First.Reset();
	Second.Reset();
	Fixture.Cleanup();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
