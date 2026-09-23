#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * The compact patch outcome must never replace the machine-readable cause the native route produced.
 *
 * Every case drives the real registered command (`Router.Execute("graph.apply_patch", ...)`) so the
 * command registration, the handler's refusal envelopes and the native preflight all run exactly as
 * a connected editor would run them. What is asserted is that the error code, the structured cause
 * fields and the phase outcomes survive *together* — not a message substring, and not the private
 * preflight method, which already returned the cause intact before the handler discarded it.
 *
 * The reserved key is `error_details`: the compact outcome owns the top level (`apply_status`,
 * `compile_status`, phase statuses, `blocked`, `saved`, `diagnostics`, inventories), and the
 * structured cause is nested under its own key so a cause field can never overwrite phase truth.
 */
namespace CortexGraphPatchErrorDetailTest
{
const TCHAR* ReservedCauseKey = TEXT("error_details");

FCortexCommandRouter MakeRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	return Router;
}

struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;

	bool Create(const TCHAR* Name)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Blueprint != nullptr;
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

UEdGraph* EnsureEventGraph(UBlueprint* Blueprint)
{
	if (Blueprint->UbergraphPages.Num() > 0) return Blueprint->UbergraphPages[0];
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	return Graph;
}

/** A real, resolvable call node: `PrintString` on the engine's system library. */
UK2Node_CallFunction* AddPrintNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetExternalMember(FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
	Call->CreateNewGuid();
	Call->AllocateDefaultPins();
	Call->NodePosX = X;
	Call->NodePosY = Y;
	Graph->AddNode(Call, true, false);
	return Call;
}

TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint, UEdGraph* Graph)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-00000000e001"));
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Graph ? Graph->GraphGuid.ToString() : FString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	Request->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), true);
	return Request;
}

/** The compact outcome keys the envelope owns; a cause must never be mistaken for one of these. */
const TCHAR* OutcomeKeys[] = {
	TEXT("patch_id"), TEXT("changed"), TEXT("dry_run"), TEXT("apply_status"), TEXT("compile_status"),
	TEXT("readback_status"), TEXT("rollback_status"), TEXT("save_status"), TEXT("post_save_status"),
	TEXT("blocked"), TEXT("saved"), TEXT("diagnostics")
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchErrorDetailScanLimitTest,
	"Cortex.Graph.Authoring.Patch.ErrorDetail.ScanLimitPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchErrorDetailScanLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchErrorDetailTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchErrorDetailScan_T17")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);

	// Above the published node bound the graph-wide scan refuses instead of planning on an
	// incompletely scanned asset. The refusal carries the observed count, the limit and completeness.
	const int32 OverLimit = FCortexGraphPatchOps::MaxScannedNodes + 2;
	for (int32 Index = 0; Index < OverLimit; ++Index)
	{
		AddPrintNode(Graph, 600 + (Index % 40) * 200, (Index / 40) * 60);
	}
	TestTrue(TEXT("the fixture exceeds the published node bound"),
		Graph->Nodes.Num() > FCortexGraphPatchOps::MaxScannedNodes);

	FCortexCommandRouter Router = MakeRouter();
	const FCortexCommandResult Result = Router.Execute(
		TEXT("graph.apply_patch"), BaseRequest(Fixture.Blueprint, Graph));
	TestFalse(TEXT("the routed scan-limit refusal is refused"), Result.bSuccess);
	TestEqual(TEXT("the scan-limit refusal keeps its error code"), Result.ErrorCode,
		FString(CortexErrorCodes::LimitExceeded));

	TestTrue(TEXT("the refusal still carries the compact outcome"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>* CausePtr = nullptr;
		const bool bHasCause = Result.ErrorDetails->TryGetObjectField(ReservedCauseKey, CausePtr)
			&& CausePtr && CausePtr->IsValid();
		TestTrue(TEXT("the structured cause survives under the reserved key"), bHasCause);
		if (bHasCause)
		{
			const TSharedPtr<FJsonObject>& Cause = *CausePtr;
			TestEqual(TEXT("the cause keeps the published scan limit"),
				Cause->GetIntegerField(TEXT("scan_limit")), FCortexGraphPatchOps::MaxScannedNodes);
			TestTrue(TEXT("the cause keeps the observed node count"),
				Cause->GetIntegerField(TEXT("scanned_nodes")) > FCortexGraphPatchOps::MaxScannedNodes);
			TestFalse(TEXT("the cause never claims completeness"), Cause->GetBoolField(TEXT("complete")));
		}
		for (const TCHAR* Key : OutcomeKeys)
		{
			TestTrue(FString::Printf(TEXT("the outcome key '%s' is still present"), Key),
				Result.ErrorDetails->HasField(Key));
		}
		// A refusal that happened before any patch work reports no phase as run, which is what lets a
		// caller distinguish "refused" from "ran and failed".
		TestEqual(TEXT("phase truth still reports no apply"),
			Result.ErrorDetails->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));
		TestEqual(TEXT("phase truth still reports no compile"),
			Result.ErrorDetails->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchErrorDetailSelectorTest,
	"Cortex.Graph.Authoring.Patch.ErrorDetail.SelectorPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchErrorDetailSelectorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchErrorDetailTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchErrorDetailSelector_T17")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	AddPrintNode(Graph, 300, 0);

	// A node selector that cannot resolve: the construction contract publishes the offending field,
	// the message and the node description, and those machine-readable details must reach the caller.
	TSharedPtr<FJsonObject> Request = BaseRequest(Fixture.Blueprint, Graph);
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("unresolvable"));
	Node->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("NoSuchFunction_CortexErrorDetail"));
	Node->SetObjectField(TEXT("params"), NodeParams);
	Request->SetArrayField(TEXT("nodes"), { MakeShared<FJsonValueObject>(Node) });

	FCortexCommandRouter Router = MakeRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestFalse(TEXT("the unresolvable selector is refused"), Result.bSuccess);
	TestEqual(TEXT("the selector refusal keeps its error code"), Result.ErrorCode,
		FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("the selector refusal still carries the compact outcome"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>* CausePtr = nullptr;
		const bool bHasCause = Result.ErrorDetails->TryGetObjectField(ReservedCauseKey, CausePtr)
			&& CausePtr && CausePtr->IsValid();
		TestTrue(TEXT("the selector cause survives under the reserved key"), bHasCause);
		if (bHasCause)
		{
			const TSharedPtr<FJsonObject>& Cause = *CausePtr;
			TestTrue(TEXT("the cause names the offending field"), Cause->HasField(TEXT("field")));
			TestTrue(TEXT("the cause keeps the node description"), Cause->HasField(TEXT("describe_node")));
		}
		TestEqual(TEXT("the selector refusal reports no apply"),
			Result.ErrorDetails->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchErrorDetailApplyOutcomeTest,
	"Cortex.Graph.Authoring.Patch.ErrorDetail.ApplyFailureKeepsOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchErrorDetailApplyOutcomeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchErrorDetailTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchErrorDetailApply_T17")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);
	AddPrintNode(Graph, 300, 0);

	// A real node to add, so the apply mutates before the injected failure.
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("added"));
	Node->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Node->SetObjectField(TEXT("params"), NodeParams);
	// The injected fault fires after authored layout is applied, so the planned node must carry a
	// position; otherwise the post-mutation failure point is never reached.
	TSharedPtr<FJsonObject> NodePosition = MakeShared<FJsonObject>();
	NodePosition->SetNumberField(TEXT("x"), 640);
	NodePosition->SetNumberField(TEXT("y"), 160);
	Node->SetObjectField(TEXT("position"), NodePosition);

	FCortexCommandRouter Router = MakeRouter();
	TSharedPtr<FJsonObject> PreviewRequest = BaseRequest(Fixture.Blueprint, Graph);
	PreviewRequest->SetArrayField(TEXT("nodes"), { MakeShared<FJsonValueObject>(Node) });
	const FCortexCommandResult Preview = Router.Execute(TEXT("graph.apply_patch"), PreviewRequest);
	TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) { Fixture.Cleanup(); return false; }
	const FString Token = Preview.Data->GetStringField(TEXT("validation_hash"));
	TestTrue(TEXT("preview publishes an approval token"), !Token.IsEmpty());

	// The failure happens after the mutation, so the refusal must still report the phase outcome:
	// an apply that ran and failed, no save, and rollback truth.
	TSharedPtr<FJsonObject> ApplyRequest = BaseRequest(Fixture.Blueprint, Graph);
	ApplyRequest->SetArrayField(TEXT("nodes"), { MakeShared<FJsonValueObject>(Node) });
	ApplyRequest->SetBoolField(TEXT("dry_run"), false);
	ApplyRequest->SetStringField(TEXT("expected_validation_hash"), Token);

	FCortexGraphPatchOps::SetApplyFaultPointForTesting(TEXT("layout"));
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), ApplyRequest);
	FCortexGraphPatchOps::ClearApplyFaultPointForTesting();

	TestFalse(TEXT("the injected post-mutation failure is refused"), Result.bSuccess);
	TestTrue(TEXT("the failure carries the compact outcome"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>& Details = Result.ErrorDetails;
		TestTrue(TEXT("the outcome reports the apply phase"),
			Details->HasField(TEXT("apply_status")) && !Details->GetStringField(TEXT("apply_status")).IsEmpty());
		TestTrue(TEXT("the outcome reports rollback truth"), Details->HasField(TEXT("rollback_status")));
		TestFalse(TEXT("the failed apply never claims a save"), Details->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("the failure is kept in the bounded diagnostics"),
			Details->GetArrayField(TEXT("diagnostics")).Num() > 0);
	}
	TestFalse(TEXT("the failed apply left the package clean"), Fixture.Package->IsDirty());

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchErrorDetailApplyCauseTest,
	"Cortex.Graph.Authoring.Patch.ErrorDetail.FailedApplyKeepsCause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchErrorDetailApplyCauseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchErrorDetailTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchErrorDetailApplyCause_T17")));
	if (!Fixture.Blueprint) { Fixture.Cleanup(); return false; }
	UEdGraph* Graph = EnsureEventGraph(Fixture.Blueprint);

	// Above the published node bound the graph-wide scan refuses inside the apply itself, so the
	// refusal leaves through the failed-apply wrapper with a structured cause, not through the
	// preview wrapper that the sibling ScanLimitPreserved case already covers.
	const int32 OverLimit = FCortexGraphPatchOps::MaxScannedNodes + 2;
	for (int32 Index = 0; Index < OverLimit; ++Index)
	{
		AddPrintNode(Graph, 600 + (Index % 40) * 200, (Index / 40) * 60);
	}
	TestTrue(TEXT("the fixture exceeds the published node bound"),
		Graph->Nodes.Num() > FCortexGraphPatchOps::MaxScannedNodes);

	TSharedPtr<FJsonObject> Request = BaseRequest(Fixture.Blueprint, Graph);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"),
		TEXT("0000000000000000000000000000000000000000000000000000000000000000"));

	FCortexCommandRouter Router = MakeRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), Request);

	TestFalse(TEXT("the apply is refused"), Result.bSuccess);
	TestEqual(TEXT("the scan refusal keeps its error code"), Result.ErrorCode,
		FString(CortexErrorCodes::LimitExceeded));
	TestTrue(TEXT("the failed apply still carries the compact outcome"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>& Outcome = Result.ErrorDetails;
		// The outcome is the apply envelope, not the preview one, so this case really exercises the
		// failed-apply wrapper's cause preservation.
		TestFalse(TEXT("the outcome reports an apply request, not a preview"),
			Outcome->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("phase truth still reports no apply"),
			Outcome->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));
		TestEqual(TEXT("phase truth still reports no compile"),
			Outcome->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
		TestFalse(TEXT("the refused apply never claims a save"), Outcome->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("the refusal is named in the bounded diagnostics"),
			Outcome->GetArrayField(TEXT("diagnostics")).Num() > 0);
		for (const TCHAR* Key : OutcomeKeys)
		{
			TestTrue(FString::Printf(TEXT("the outcome key '%s' is still present"), Key),
				Outcome->HasField(Key));
		}

		const TSharedPtr<FJsonObject>* CausePtr = nullptr;
		const bool bHasCause = Outcome->TryGetObjectField(ReservedCauseKey, CausePtr)
			&& CausePtr && CausePtr->IsValid();
		TestTrue(TEXT("the structured cause survives the failed-apply wrapper"), bHasCause);
		if (bHasCause)
		{
			const TSharedPtr<FJsonObject>& Cause = *CausePtr;
			TestEqual(TEXT("the cause keeps the published scan limit"),
				Cause->GetIntegerField(TEXT("scan_limit")), FCortexGraphPatchOps::MaxScannedNodes);
			TestTrue(TEXT("the cause keeps the observed node count"),
				Cause->GetIntegerField(TEXT("scanned_nodes")) > FCortexGraphPatchOps::MaxScannedNodes);
			TestFalse(TEXT("the cause never claims completeness"), Cause->GetBoolField(TEXT("complete")));
		}
	}

	Fixture.Cleanup();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
