#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "CortexAssetMutationGuard.h"
#include "Operations/CortexGraphPatchState.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "UObject/GarbageCollection.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR
namespace CortexGraphPatchApplyTest
{
static UBlueprint* MakeBlueprint(UPackage*& OutPackage)
{
	OutPackage = CreatePackage(TEXT("/Temp/BP_PatchApply_T07"));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, TEXT("BP_PatchApply_T07"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static TSharedPtr<FJsonObject> MakeRequest(UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000000007"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("self"));
	Node->SetStringField(TEXT("node_class"), TEXT("Self"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

static void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchApplyBasicTest,
	"Cortex.Graph.Authoring.Apply.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchApplyBasicTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchApplyTest::MakeBlueprint(Package);
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchApplyTest::MakeRequest(Blueprint);
	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPreparedPatch Prepared;
	TestTrue(FString::Printf(TEXT("apply preflight succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	const int32 NodesBefore = Blueprint->UbergraphPages[0]->Nodes.Num();
	TestTrue(FString::Printf(TEXT("prepared patch applies: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	TestEqual(TEXT("apply adds exactly the planned node"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore + 1);
	TestTrue(TEXT("apply transaction undoes"), GEditor && GEditor->UndoTransaction());
	TestEqual(TEXT("undo restores graph node count"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore);
	TestTrue(TEXT("apply transaction redoes"), GEditor && GEditor->RedoTransaction());
	TestEqual(TEXT("redo reapplies graph node count"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore + 1);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("applied graph survives garbage collection"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore + 1);

	FCortexAssetMutationGuard::Block(Blueprint, TEXT("forced verification failure"));
	TestFalse(TEXT("blocked asset refuses a second graph mutation before side effects"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	TestEqual(TEXT("blocked mutation preserves graph nodes"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore + 1);
	TestTrue(TEXT("blocked asset remains readable"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint).IsValid());
	CortexGraphPatchApplyTest::Cleanup(Package, Blueprint);
	return true;
}
#endif
