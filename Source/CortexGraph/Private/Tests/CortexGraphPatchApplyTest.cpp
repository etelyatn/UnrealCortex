#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "CortexGraphCommandHandler.h"
#include "CortexAssetMutationGuard.h"
#include "Operations/CortexGraphPatchState.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameMode.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "UObject/GarbageCollection.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR
namespace CortexGraphPatchApplyTest
{
static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name = TEXT("BP_PatchApply_T07"))
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static TSharedPtr<FJsonObject> MakeRequest(UBlueprint* Blueprint, const int32 NodeCount = 1)
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
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (int32 Index = 0; Index < NodeCount; ++Index)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), FString::Printf(TEXT("self%d"), Index));
		Node->SetStringField(TEXT("node_class"), TEXT("Self"));
		if (Index == 0)
		{
			TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), 240);
			Position->SetNumberField(TEXT("y"), 120);
			Node->SetObjectField(TEXT("position"), Position);
		}
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
	}
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
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchApplyTestCleanup")));
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
	TSharedPtr<FJsonObject> GuardedParams = MakeShared<FJsonObject>();
	GuardedParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	FCortexGraphCommandHandler GraphHandler;
	TestFalse(TEXT("normal graph mutation dispatch refuses blocked asset"),
		GraphHandler.Execute(TEXT("add_node"), GuardedParams).bSuccess);
	TestTrue(TEXT("normal graph read dispatch remains available"),
		GraphHandler.Execute(TEXT("list_graphs"), GuardedParams).bSuccess);
	CortexGraphPatchApplyTest::Cleanup(Package, Blueprint);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchApplyRecoveryTest,
	"Cortex.Graph.Authoring.Apply.Recovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchApplyRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchApplyTest::MakeBlueprint(Package, TEXT("BP_PatchApplyRecovery_T07"));
	TestNotNull(TEXT("recovery fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UPackage* SentinelPackage = CreatePackage(TEXT("/Temp/BP_PatchApplyRecovery_Sentinel_T07"));
	SentinelPackage->MarkPackageDirty();
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	TSharedPtr<FJsonObject> Request = CortexGraphPatchApplyTest::MakeRequest(Blueprint, 2);
	const TArray<TSharedPtr<FJsonValue>>& RecoveryNodes = Request->GetArrayField(TEXT("nodes"));
	TSharedPtr<FJsonObject> RecoveryCall = RecoveryNodes[0]->AsObject();
	RecoveryCall->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	RecoveryCall->SetObjectField(TEXT("params"), CallParams);
	TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> StringLiteral = MakeShared<FJsonObject>();
	StringLiteral->SetStringField(TEXT("kind"), TEXT("string"));
	StringLiteral->SetStringField(TEXT("value"), TEXT("recovery"));
	Defaults->SetObjectField(TEXT("InString"), StringLiteral);
	RecoveryCall->SetObjectField(TEXT("defaults"), Defaults);
	TSharedPtr<FJsonObject> RecoverySecondCall = RecoveryNodes[1]->AsObject();
	RecoverySecondCall->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> SecondCallParams = MakeShared<FJsonObject>();
	SecondCallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	RecoverySecondCall->SetObjectField(TEXT("params"), SecondCallParams);
	TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("client_id"), TEXT("self0"));
	From->SetStringField(TEXT("pin"), TEXT("then"));
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("client_id"), TEXT("self1"));
	To->SetStringField(TEXT("pin"), TEXT("execute"));
	Connection->SetObjectField(TEXT("from"), From);
	Connection->SetObjectField(TEXT("to"), To);
	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeShared<FJsonValueObject>(Connection));
	Request->SetArrayField(TEXT("connections"), Connections);
	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("recovery preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPreparedPatch Prepared;
	TestTrue(FString::Printf(TEXT("recovery apply preflight succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("first_default"));
	TestFalse(TEXT("fault after first default fails application"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestEqual(TEXT("default fault restores exact authoring fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->RemoveField(TEXT("expected_validation_hash"));
	TestTrue(TEXT("post-default recovery preview succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	TestTrue(TEXT("post-default recovery apply preflight succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("layout"));
	TestFalse(TEXT("fault after layout fails application"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestEqual(TEXT("layout fault restores exact authoring fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	FCortexGraphPreparedPatch LayoutPreview;
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->RemoveField(TEXT("expected_validation_hash"));
	TestTrue(TEXT("post-layout recovery preview succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, LayoutPreview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), LayoutPreview.ValidationHash);
	TestTrue(TEXT("post-layout recovery apply preflight succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("first_link"));
	TestFalse(TEXT("fault after first link fails application"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestEqual(TEXT("link fault restores exact authoring fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->RemoveField(TEXT("expected_validation_hash"));
	TestTrue(TEXT("post-link recovery preview succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	TestTrue(TEXT("post-link recovery apply preflight succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));

	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("second_node"));
	TestFalse(TEXT("fault after second node fails application"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestEqual(TEXT("fault recovery restores exact authoring fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	TestTrue(TEXT("fault recovery preserves unrelated dirty sentinel"), SentinelPackage->IsDirty());
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->RemoveField(TEXT("expected_validation_hash"));
	TestTrue(TEXT("pre-verification-failure preview succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	TestTrue(TEXT("pre-verification-failure apply preflight succeeds"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("verification_failure"));
	TestFalse(TEXT("forced recovery verification failure blocks the asset"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestTrue(TEXT("blocked asset remains readable after verification failure"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint).IsValid());
	CortexGraphPatchApplyTest::Cleanup(Package, Blueprint);
	SentinelPackage->ClearFlags(RF_Standalone);
	SentinelPackage->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchApplyImplementationRecoveryTest,
	"Cortex.Graph.Authoring.Apply.ImplementationRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchApplyImplementationRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AGameMode::StaticClass(), Package = CreatePackage(TEXT("/Temp/BP_PatchImplementation_T07")),
		FName("BP_PatchImplementation_T07"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("implementation fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchApplyTest::MakeRequest(Blueprint);
	TSharedPtr<FJsonObject> Target = Request->GetObjectField(TEXT("target"));
	Target->RemoveField(TEXT("graph_ref"));
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.GameMode"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReadyToStartMatch"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));

	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult Error;
	TestTrue(FString::Printf(TEXT("implementation preview succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPreparedPatch Prepared;
	TestTrue(FString::Printf(TEXT("implementation apply preflight succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("implementation_graph"));
	TestFalse(TEXT("fault after implementation graph creation fails application"),
		FCortexGraphPatchOps::Apply(Blueprint, Prepared, Error));
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();
	TestEqual(TEXT("implementation graph recovery restores exact authoring fingerprint"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	CortexGraphPatchApplyTest::Cleanup(Package, Blueprint);
	return true;
}
#endif
