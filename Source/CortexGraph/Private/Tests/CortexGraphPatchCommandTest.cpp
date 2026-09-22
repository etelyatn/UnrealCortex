#include "Misc/AutomationTest.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "ICortexDomainHandler.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "CortexGraphCommandHandler.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * `graph.apply_patch` handler coverage: dispatch, the published schema and the batch restriction.
 *
 * Every case drives the real TCP-namespace entry point (`FCortexCommandRouter::Execute`) so the
 * command registration, the blocked-asset guard and the Core batch preflight are exercised exactly
 * as a connected editor would exercise them. The compact result is asserted per phase, and the
 * refusal cases prove that no inner patch work ran at all.
 */
namespace CortexGraphPatchCommandTest
{
/** Observations around real coordinator operations, installed per test. */
struct FOperations
{
	int32 TargetCompiles = 0;
	int32 RecoveryCompiles = 0;
	int32 Saves = 0;

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
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchCommandTestCleanup")));
	}
}

static bool DeleteFixtureFile(const FString& Filename)
{
	if (Filename.IsEmpty()) return true;
	return IFileManager::Get().Delete(*Filename, false, true, true);
}

/** Real Blueprint fixture in a real mounted content path, so the handler can resolve it by path. */
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	FString Filename;

	bool Create(const TCHAR* Name)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Blueprint) return false;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		// A stale file from an interrupted run makes the engine refuse to save over it.
		DeleteFixtureFile(Filename);
		return true;
	}

	/** Establishes the clean starting package that save=true requires. */
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

static FCortexCommandRouter MakeRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	return Router;
}

static FString GraphHash(UBlueprint* Blueprint)
{
	const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
}

/** Identity comparison that does not depend on the published GUID representation. */
static bool SameGuid(const FString& Left, const FString& Right)
{
	FGuid LeftGuid;
	FGuid RightGuid;
	return FGuid::Parse(Left, LeftGuid) && FGuid::Parse(Right, RightGuid) && LeftGuid == RightGuid;
}

/** Resolves one published identity in the live asset, across every graph. */
static UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuid)
{
	FGuid Guid;
	if (!Blueprint || !FGuid::Parse(NodeGuid, Guid)) return nullptr;
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

static void AddNode(
	const TSharedPtr<FJsonObject>& Request,
	const TCHAR* ClientId,
	const TSharedPtr<FJsonObject>& Defaults)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), ClientId);
	Node->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	Node->SetObjectField(TEXT("params"), Params);
	if (Defaults.IsValid()) Node->SetObjectField(TEXT("defaults"), Defaults);
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TArray<TSharedPtr<FJsonValue>> Nodes = Request->TryGetArrayField(TEXT("nodes"), ExistingNodes) && ExistingNodes
		? *ExistingNodes
		: TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
}

/** One overridden ReceiveBeginPlay implementation entry with a print call reached from it. */
static TSharedPtr<FJsonObject> IntentRequest(UBlueprint* Blueprint, const TCHAR* PatchId, const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"),
		Fingerprint.IsValid() ? Fingerprint : FCortexGraphPatchState::ComputeFingerprint(Blueprint));

	TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("string"));
	Literal->SetStringField(TEXT("value"), TEXT("cortex patch command"));
	Defaults->SetObjectField(TEXT("InString"), Literal);
	AddNode(Request, TEXT("note"), Defaults);

	TArray<TSharedPtr<FJsonValue>> Connections;
	TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetBoolField(TEXT("entry"), true);
	From->SetStringField(TEXT("pin"), TEXT("then"));
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("client_id"), TEXT("note"));
	To->SetStringField(TEXT("pin"), TEXT("execute"));
	Connection->SetObjectField(TEXT("from"), From);
	Connection->SetObjectField(TEXT("to"), To);
	Connections.Add(MakeShared<FJsonValueObject>(Connection));
	Request->SetArrayField(TEXT("connections"), Connections);

	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("pin_updates"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

/** An empty, explicitly idempotent intent: the no-op path of the same contract. */
static TSharedPtr<FJsonObject> NoOpRequest(UBlueprint* Blueprint, const TCHAR* PatchId)
{
	TSharedPtr<FJsonObject> Request = IntentRequest(Blueprint, PatchId, nullptr);
	Request->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("allow_noop"), true);
	return Request;
}

/** Runs one preview through the router and returns its data, or null with the error reported. */
static TSharedPtr<FJsonObject> Preview(FAutomationTestBase& Test, FCortexCommandRouter& Router, const TSharedPtr<FJsonObject>& Request)
{
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), Request);
	Test.TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Result.ErrorMessage), Result.bSuccess);
	return Result.bSuccess && Result.Data.IsValid() ? Result.Data : nullptr;
}

static FString FingerprintHash(const TSharedPtr<FJsonObject>& Fingerprint)
{
	FString Hash;
	if (Fingerprint.IsValid()) Fingerprint->TryGetStringField(TEXT("graph_authoring_hash"), Hash);
	return Hash;
}

/**
 * A refusal that happens before any patch work still reports the compact result: the requested
 * identity, change-free phase statuses and the bounded diagnostics. Everything the handler cannot
 * honestly know yet is absent instead of fabricated.
 */
static void AssertCompactRefusal(
	FAutomationTestBase& Test,
	const FCortexCommandResult& Result,
	const TCHAR* ExpectedPatchId,
	const bool bExpectLiveState,
	const bool bExpectEmptyPlanKeys)
{
	Test.TestFalse(TEXT("refusal stays a failure"), Result.bSuccess);
	Test.TestTrue(TEXT("refusal carries the compact result"), Result.ErrorDetails.IsValid());
	if (!Result.ErrorDetails.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject>& Details = Result.ErrorDetails;
	Test.TestTrue(TEXT("refusal reports the requested patch id"),
		Details->HasField(TEXT("patch_id"))
			&& SameGuid(Details->GetStringField(TEXT("patch_id")), ExpectedPatchId));
	Test.TestFalse(TEXT("refusal reports no change"), Details->GetBoolField(TEXT("changed")));
	Test.TestEqual(TEXT("refusal reports no apply"), Details->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));
	Test.TestEqual(TEXT("refusal reports no compile"), Details->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
	Test.TestEqual(TEXT("refusal reports no readback"), Details->GetStringField(TEXT("readback_status")), FString(TEXT("not_requested")));
	Test.TestEqual(TEXT("refusal reports no rollback"), Details->GetStringField(TEXT("rollback_status")), FString(TEXT("not_requested")));
	Test.TestEqual(TEXT("refusal reports no save"), Details->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
	Test.TestEqual(TEXT("refusal reports no post-save verification"),
		Details->GetStringField(TEXT("post_save_status")), FString(TEXT("not_requested")));
	Test.TestFalse(TEXT("refusal never claims a save"), Details->GetBoolField(TEXT("saved")));
	Test.TestFalse(TEXT("refusal never claims a validation token"), Details->HasField(TEXT("validation_hash")));
	Test.TestEqual(TEXT("refusal reports zero target compiles"), Details->GetNumberField(TEXT("target_compile_count")), 0.0);
	Test.TestEqual(TEXT("refusal reports zero recovery compiles"), Details->GetNumberField(TEXT("recovery_compile_count")), 0.0);
	Test.TestEqual(TEXT("refusal reports the reused client ids it reconciled by"),
		Details->GetArrayField(TEXT("reused_client_ids")).Num(), 0);

	const TArray<TSharedPtr<FJsonValue>>& Diagnostics = Details->GetArrayField(TEXT("diagnostics"));
	Test.TestTrue(TEXT("refusal keeps the failure in its diagnostics"), Diagnostics.Num() > 0);
	Test.TestTrue(TEXT("refusal diagnostics are bounded"),
		Diagnostics.Num() <= 16);
	if (Diagnostics.Num() > 0)
	{
		Test.TestTrue(TEXT("refusal diagnostics name the error code"),
			Diagnostics[0]->AsString().Contains(Result.ErrorCode));
	}

	if (bExpectLiveState)
	{
		Test.TestTrue(TEXT("refusal reports the live before fingerprint"), Details->HasField(TEXT("fingerprint_before")));
		Test.TestTrue(TEXT("refusal reports the live after fingerprint"), Details->HasField(TEXT("fingerprint_after")));
		Test.TestTrue(TEXT("refusal reports the unchanged live fingerprint"),
			FingerprintHash(Details->GetObjectField(TEXT("fingerprint_before"))).Len() > 0
				&& FingerprintHash(Details->GetObjectField(TEXT("fingerprint_before")))
					== FingerprintHash(Details->GetObjectField(TEXT("fingerprint_after"))));
		Test.TestTrue(TEXT("refusal reports the live dirty state"),
			Details->HasField(TEXT("dirty_before")) && Details->HasField(TEXT("dirty_after")));
		Test.TestEqual(TEXT("refusal reports an unchanged dirty state"),
			Details->GetBoolField(TEXT("dirty_before")), Details->GetBoolField(TEXT("dirty_after")));
	}
	else
	{
		Test.TestFalse(TEXT("a guard refusal claims no before fingerprint"), Details->HasField(TEXT("fingerprint_before")));
		Test.TestFalse(TEXT("a guard refusal claims no after fingerprint"), Details->HasField(TEXT("fingerprint_after")));
		Test.TestFalse(TEXT("a guard refusal claims no dirty state"),
			Details->HasField(TEXT("dirty_before")) || Details->HasField(TEXT("dirty_after")));
	}

	if (bExpectEmptyPlanKeys)
	{
		Test.TestTrue(TEXT("an apply refusal reports an empty client-id mapping"),
			Details->GetObjectField(TEXT("node_mappings"))->Values.Num() == 0);
		Test.TestFalse(TEXT("an apply refusal reports no planned entry"),
			Details->GetObjectField(TEXT("locators"))->GetBoolField(TEXT("has_entry_node")));
	}
	else
	{
		Test.TestFalse(TEXT("a plan-free refusal publishes no mappings"), Details->HasField(TEXT("node_mappings")));
		Test.TestFalse(TEXT("a plan-free refusal publishes no locators"), Details->HasField(TEXT("locators")));
	}
}

static TArray<FString> ParamNames(const FCortexCommandInfo& Info, const bool bRequired)
{
	TArray<FString> Names;
	for (const FCortexParamInfo& Param : Info.Params)
	{
		if (Param.bRequired == bRequired)
		{
			Names.Add(Param.Name);
		}
	}
	return Names;
}
}

// ---------------------------------------------------------------------------
// 1. Preview publishes the planned identities, the validation token and no graph dump
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandPreviewTest,
	"Cortex.Graph.Authoring.PatchCommand.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandPreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandPreview_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	const FString HashBefore = GraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0001"), nullptr);
	const TSharedPtr<FJsonObject> Data = Preview(*this, Router, Request);
	Operations.End();
	if (!Data.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}

	TestTrue(TEXT("preview reports the requested patch id"),
		SameGuid(Data->GetStringField(TEXT("patch_id")), TEXT("00000000-0000-0000-0000-0000000f0001")));
	TestTrue(TEXT("preview reports a prospective change"), Data->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("preview reports itself as a dry run"), Data->GetBoolField(TEXT("dry_run")));
	TestTrue(TEXT("preview returns a validation token"), Data->HasField(TEXT("validation_hash")));
	TestFalse(TEXT("preview validation token is nonempty"), Data->GetStringField(TEXT("validation_hash")).IsEmpty());
	TestEqual(TEXT("preview never claims an apply"), Data->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("preview never claims a compile"), Data->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("preview never claims persistence"), Data->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("preview never claims post-save verification"), Data->GetStringField(TEXT("post_save_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("preview reports zero compiles"), Data->GetNumberField(TEXT("target_compile_count")), 0.0);
	TestFalse(TEXT("preview reports no save"), Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("preview does not block the asset"), Data->GetBoolField(TEXT("blocked")));

	const TSharedPtr<FJsonObject> Mappings = Data->GetObjectField(TEXT("node_mappings"));
	TestTrue(TEXT("preview maps the planned client id"), Mappings.IsValid() && Mappings->HasField(TEXT("note")));
	const TSharedPtr<FJsonObject> Locators = Data->GetObjectField(TEXT("locators"));
	TestTrue(TEXT("preview locators are present"), Locators.IsValid());
	TestTrue(TEXT("preview reports the planned entry node"), Locators->GetBoolField(TEXT("has_entry_node")));
	TestTrue(TEXT("preview reports a graph locator"), Locators->HasField(TEXT("graph_guid")));
	TestTrue(TEXT("preview reports the live fingerprint"), Data->HasField(TEXT("fingerprint_before")));
	TestEqual(TEXT("preview dirty flag matches the live package"),
		Data->GetBoolField(TEXT("dirty_before")), bDirtyBefore);

	TestFalse(TEXT("preview response carries no graph dump"), Data->HasField(TEXT("nodes")));
	TestFalse(TEXT("preview response carries no edge dump"), Data->HasField(TEXT("connections")));
	TestEqual(TEXT("preview performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("preview performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("preview leaves the authoring fingerprint untouched"), GraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("preview leaves the node count untouched"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestEqual(TEXT("preview leaves the dirty flag untouched"), Fixture.Package->IsDirty(), bDirtyBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 2. Apply publishes the verified outcome, then an idempotent replay reports unchanged
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandApplyTest,
	"Cortex.Graph.Authoring.PatchCommand.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandApply_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);
	const FString PatchId = TEXT("00000000-0000-0000-0000-0000000f0002");

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, *PatchId, nullptr);
	const TSharedPtr<FJsonObject> PreviewData = Preview(*this, Router, Request);
	if (!PreviewData.IsValid())
	{
		Operations.End();
		Fixture.Cleanup();
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
	const FCortexCommandResult Applied = Router.Execute(TEXT("graph.apply_patch"), Request);
	Operations.End();
	TestTrue(FString::Printf(TEXT("apply succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess || !Applied.Data.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}

	const TSharedPtr<FJsonObject>& Data = Applied.Data;
	TestTrue(TEXT("apply reports the requested patch id"),
		SameGuid(Data->GetStringField(TEXT("patch_id")), PatchId));
	TestTrue(TEXT("apply reports a real change"), Data->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("apply reports itself as an apply"), Data->GetBoolField(TEXT("dry_run")));
	TestEqual(TEXT("apply reports the reversible mutation"), Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
	TestEqual(TEXT("apply reports the single target compile"), Data->GetStringField(TEXT("compile_status")), FString(TEXT("compiled")));
	TestEqual(TEXT("apply reports the authoritative readback"), Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
	TestEqual(TEXT("apply never rolls back a verified result"), Data->GetStringField(TEXT("rollback_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("apply does not save when save=false"), Data->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("apply reports one target compile"), Data->GetNumberField(TEXT("target_compile_count")), 1.0);
	TestEqual(TEXT("apply reports no recovery compile"), Data->GetNumberField(TEXT("recovery_compile_count")), 0.0);
	TestFalse(TEXT("apply with save=false never claims a save"), Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("apply does not block the asset"), Data->GetBoolField(TEXT("blocked")));
	TestEqual(TEXT("apply reports one observed target compile"), Operations.TargetCompiles, 1);
	TestEqual(TEXT("apply performs no save event"), Operations.Saves, 0);

	const TSharedPtr<FJsonObject> Mappings = Data->GetObjectField(TEXT("node_mappings"));
	TestTrue(TEXT("apply maps the created client id"), Mappings.IsValid() && Mappings->HasField(TEXT("note")));
	const FString NodeGuid = Mappings.IsValid() ? Mappings->GetStringField(TEXT("note")) : FString();
	TestNotNull(TEXT("apply materializes the planned client id in the live asset"),
		FindNodeByGuid(Fixture.Blueprint, NodeGuid));
	const TSharedPtr<FJsonObject> Locators = Data->GetObjectField(TEXT("locators"));
	TestTrue(TEXT("apply reports the applied entry node"), Locators.IsValid() && Locators->GetBoolField(TEXT("has_entry_node")));
	TestTrue(TEXT("apply reports the applied graph"), Locators.IsValid() && Locators->HasField(TEXT("graph_guid")));
	const int32 NodesAfterApply = CountNativeNodes(Fixture.Blueprint);
	TestTrue(TEXT("apply grows the graph by the planned change"), NodesAfterApply > NodesBefore);
	TestTrue(TEXT("apply leaves the package dirty without save"), Fixture.Package->IsDirty());
	TestTrue(TEXT("apply reports a post-apply fingerprint"), Data->HasField(TEXT("fingerprint_after")));
	TestFalse(TEXT("apply response carries no graph dump"), Data->HasField(TEXT("nodes")));

	// Idempotent replay: the same patch id reconciles to the same deterministic identity.
	FOperations ReplayOperations;
	ReplayOperations.Begin();
	TSharedPtr<FJsonObject> ReplayRequest = IntentRequest(Fixture.Blueprint, *PatchId, nullptr);
	const TSharedPtr<FJsonObject> ReplayPreviewData = Preview(*this, Router, ReplayRequest);
	if (ReplayPreviewData.IsValid())
	{
		TestFalse(TEXT("replay preview reports an idempotent change-free intent"), ReplayPreviewData->GetBoolField(TEXT("changed")));
		TestTrue(TEXT("replay preview reports the reused client id"),
			ReplayPreviewData->GetObjectField(TEXT("node_mappings"))->HasField(TEXT("note")));
		ReplayRequest->SetBoolField(TEXT("dry_run"), false);
		ReplayRequest->SetStringField(TEXT("expected_validation_hash"), ReplayPreviewData->GetStringField(TEXT("validation_hash")));
		const FCortexCommandResult Replayed = Router.Execute(TEXT("graph.apply_patch"), ReplayRequest);
		TestTrue(FString::Printf(TEXT("replay applies: %s"), *Replayed.ErrorMessage), Replayed.bSuccess);
		if (Replayed.bSuccess && Replayed.Data.IsValid())
		{
			TestEqual(TEXT("replay reports unchanged"), Replayed.Data->GetStringField(TEXT("apply_status")), FString(TEXT("unchanged")));
			TestEqual(TEXT("replay never claims a compile"), Replayed.Data->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
			TestTrue(TEXT("replay reports the reused client id"),
				Replayed.Data->GetArrayField(TEXT("reused_client_ids")).Num() == 1);
		}
	}
	ReplayOperations.End();
	TestEqual(TEXT("replay performs no compile"), ReplayOperations.TargetCompiles, 0);
	TestEqual(TEXT("replay performs no save"), ReplayOperations.Saves, 0);
	TestEqual(TEXT("replay does not duplicate the planned nodes"),
		CountNativeNodes(Fixture.Blueprint), NodesAfterApply);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 3. An explicitly idempotent empty request is the no-op path
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandNoOpTest,
	"Cortex.Graph.Authoring.PatchCommand.NoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandNoOpTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandNoOp_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	const FString HashBefore = GraphHash(Fixture.Blueprint);
	const bool bDirtyBefore = Fixture.Package->IsDirty();

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = NoOpRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0003"));
	const TSharedPtr<FJsonObject> PreviewData = Preview(*this, Router, Request);
	TestNotNull(TEXT("no-op preview returns data"), PreviewData.Get());
	if (PreviewData.IsValid())
	{
		TestFalse(TEXT("no-op preview reports no change"), PreviewData->GetBoolField(TEXT("changed")));
		TestEqual(TEXT("no-op preview opens no transaction"), Fixture.Package->IsDirty(), bDirtyBefore);

		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
		const FCortexCommandResult NoOp = Router.Execute(TEXT("graph.apply_patch"), Request);
		TestTrue(FString::Printf(TEXT("no-op apply succeeds: %s"), *NoOp.ErrorMessage), NoOp.bSuccess);
		if (NoOp.bSuccess && NoOp.Data.IsValid())
		{
			TestEqual(TEXT("no-op reports unchanged"), NoOp.Data->GetStringField(TEXT("apply_status")), FString(TEXT("unchanged")));
			TestEqual(TEXT("no-op reports no compile"), NoOp.Data->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
			TestEqual(TEXT("no-op reports no readback verification"), NoOp.Data->GetStringField(TEXT("readback_status")), FString(TEXT("not_requested")));
			TestFalse(TEXT("no-op reports no saved state"), NoOp.Data->GetBoolField(TEXT("saved")));
			TestTrue(TEXT("no-op reports mapped planned identity"), NoOp.Data->GetObjectField(TEXT("node_mappings"))->Values.Num() == 0);
		}
	}
	Operations.End();

	TestEqual(TEXT("no-op performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("no-op performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("no-op leaves the authoring fingerprint untouched"), GraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("no-op leaves the dirtiness untouched"), Fixture.Package->IsDirty(), bDirtyBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 4. A validation failure and a stale precondition refuse before any work
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandRefusalTest,
	"Cortex.Graph.Authoring.PatchCommand.Refusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandRefusal_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	const FString HashBefore = GraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	FOperations Operations;
	Operations.Begin();

	// (a) unknown field: the envelope validator owns the shape and never guesses
	TSharedPtr<FJsonObject> UnknownField = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0004"), nullptr);
	UnknownField->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	const FCortexCommandResult UnknownResult = Router.Execute(TEXT("graph.apply_patch"), UnknownField);
	TestFalse(TEXT("an unknown envelope field is refused"), UnknownResult.bSuccess);
	TestEqual(TEXT("unknown field error code"), UnknownResult.ErrorCode, FString(CortexErrorCodes::InvalidField));
	AssertCompactRefusal(*this, UnknownResult, TEXT("00000000-0000-0000-0000-0000000f0004"), true, false);

	// (b) a flag that cannot be a boolean at all is refused. The engine's JSON DOM coerces
	// strings and numbers into bools, so only arrays, objects and null are refused here; the MCP
	// facade refuses every non-boolean flag for its callers before forwarding.
	TSharedPtr<FJsonObject> BadFlag = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0005"), nullptr);
	BadFlag->SetArrayField(TEXT("dry_run"), TArray<TSharedPtr<FJsonValue>>());
	const FCortexCommandResult BadFlagResult = Router.Execute(TEXT("graph.apply_patch"), BadFlag);
	TestFalse(TEXT("an array dry_run is refused"), BadFlagResult.bSuccess);
	TestEqual(TEXT("bad flag error code"), BadFlagResult.ErrorCode, FString(CortexErrorCodes::InvalidField));

	// (c) a stale expected fingerprint is refused as a stale precondition
	TSharedPtr<FJsonObject> Stale = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0006"), nullptr);
	TSharedPtr<FJsonObject> StaleFingerprint = Stale->GetObjectField(TEXT("expected_fingerprint"));
	StaleFingerprint->SetStringField(TEXT("graph_authoring_hash"), TEXT("stale_hash_0123456789"));
	Stale->SetObjectField(TEXT("expected_fingerprint"), StaleFingerprint);
	const FCortexCommandResult StaleResult = Router.Execute(TEXT("graph.apply_patch"), Stale);
	TestFalse(TEXT("a stale fingerprint is refused"), StaleResult.bSuccess);
	TestEqual(TEXT("stale fingerprint error code"), StaleResult.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
	AssertCompactRefusal(*this, StaleResult, TEXT("00000000-0000-0000-0000-0000000f0006"), true, false);

	// (d) an apply without a preview token is refused (the token is a precondition, not auth)
	TSharedPtr<FJsonObject> NoToken = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0007"), nullptr);
	NoToken->SetBoolField(TEXT("dry_run"), false);
	const FCortexCommandResult NoTokenResult = Router.Execute(TEXT("graph.apply_patch"), NoToken);
	TestFalse(TEXT("an apply without a validation token is refused"), NoTokenResult.bSuccess);
	TestEqual(TEXT("missing token error code"), NoTokenResult.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
	AssertCompactRefusal(*this, NoTokenResult, TEXT("00000000-0000-0000-0000-0000000f0007"), true, true);

	Operations.End();
	TestEqual(TEXT("refusals perform no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("refusals perform no save"), Operations.Saves, 0);
	TestEqual(TEXT("refusals leave the authoring fingerprint untouched"), GraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("refusals leave the node count untouched"), CountNativeNodes(Fixture.Blueprint), NodesBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 5. A missing asset is refused by the loader, not by a crash
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandMissingAssetTest,
	"Cortex.Graph.Authoring.PatchCommand.MissingAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandMissingAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchCommandTest;

	FCortexCommandRouter Router = MakeRouter();
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/BP_PatchCommand_NoSuchAsset.BP_PatchCommand_NoSuchAsset"));
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-0000000f0008"));
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestFalse(TEXT("a missing asset is refused"), Result.bSuccess);
	TestEqual(TEXT("missing asset error code"), Result.ErrorCode, FString(CortexErrorCodes::AssetNotFound));

	TSharedPtr<FJsonObject> NoAssetPath = MakeShared<FJsonObject>();
	const FCortexCommandResult NoPathResult = Router.Execute(TEXT("graph.apply_patch"), NoAssetPath);
	TestFalse(TEXT("a request without asset_path is refused"), NoPathResult.bSuccess);
	TestEqual(TEXT("missing asset_path error code"), NoPathResult.ErrorCode, FString(CortexErrorCodes::InvalidField));
	return true;
}

// ---------------------------------------------------------------------------
// 6. Save failure and post-save verification failure keep the applied outcome visible
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandSaveFailureTest,
	"Cortex.Graph.Authoring.PatchCommand.SaveFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandSaveFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandSaveFailure_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	TestFalse(TEXT("baseline save leaves a clean package"), Fixture.Package->IsDirty());
	FCortexCommandRouter Router = MakeRouter();

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f0009"), nullptr);
	const TSharedPtr<FJsonObject> PreviewData = Preview(*this, Router, Request);
	if (!PreviewData.IsValid())
	{
		Operations.End();
		Fixture.Cleanup();
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetBoolField(TEXT("save"), true);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
	FCortexGraphPatchOps::SetSaveFaultForTesting(true);
	const FCortexCommandResult SaveFailed = Router.Execute(TEXT("graph.apply_patch"), Request);
	FCortexGraphPatchOps::SetSaveFaultForTesting(false);
	Operations.End();

	TestFalse(TEXT("a save failure fails the patch response"), SaveFailed.bSuccess);
	TestEqual(TEXT("save failure error code"), SaveFailed.ErrorCode, FString(CortexErrorCodes::SaveFailed));
	TestTrue(TEXT("save failure still reports the compact applied outcome"), SaveFailed.ErrorDetails.IsValid());
	if (SaveFailed.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>& Details = SaveFailed.ErrorDetails;
		TestEqual(TEXT("save failure preserves the verified apply"), Details->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestEqual(TEXT("save failure preserves the authoritative readback"), Details->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("save failure reports the failed save"), Details->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
		TestEqual(TEXT("save failure never claims post-save verification"), Details->GetStringField(TEXT("post_save_status")), FString(TEXT("not_requested")));
		TestFalse(TEXT("save failure keeps saved false"), Details->GetBoolField(TEXT("saved")));
		TestNotEqual(TEXT("save failure is never reported as a rollback"), Details->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		TestTrue(TEXT("save failure reports the applied node mapping"), Details->GetObjectField(TEXT("node_mappings"))->HasField(TEXT("note")));
	}
	TestEqual(TEXT("save failure performs no real save event"), Operations.Saves, 0);
	TestEqual(TEXT("save failure performs one target compile"), Operations.TargetCompiles, 1);
	TestTrue(TEXT("save failure keeps the package dirty"), Fixture.Package->IsDirty());

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandPostSaveFailureTest,
	"Cortex.Graph.Authoring.PatchCommand.PostSaveFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandPostSaveFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandPostSave_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	FCortexCommandRouter Router = MakeRouter();

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f000a"), nullptr);
	const TSharedPtr<FJsonObject> PreviewData = Preview(*this, Router, Request);
	if (!PreviewData.IsValid())
	{
		Operations.End();
		Fixture.Cleanup();
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetBoolField(TEXT("save"), true);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(TEXT("clean_package"));
	const FCortexCommandResult PostSaveFailed = Router.Execute(TEXT("graph.apply_patch"), Request);
	FCortexGraphPatchOps::SetPostSaveVerificationFaultForTesting(NAME_None);
	Operations.End();

	TestFalse(TEXT("a post-save verification failure fails the patch response"), PostSaveFailed.bSuccess);
	TestEqual(TEXT("post-save failure error code"), PostSaveFailed.ErrorCode, FString(CortexErrorCodes::VerificationFailed));
	TestEqual(TEXT("post-save failure performs exactly one real save"), Operations.Saves, 1);
	TestTrue(TEXT("post-save failure still reports the committed outcome"), PostSaveFailed.ErrorDetails.IsValid());
	if (PostSaveFailed.ErrorDetails.IsValid())
	{
		const TSharedPtr<FJsonObject>& Details = PostSaveFailed.ErrorDetails;
		TestEqual(TEXT("post-save failure keeps the applied outcome"), Details->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestEqual(TEXT("post-save failure keeps the committed save"), Details->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
		TestTrue(TEXT("post-save failure keeps saved true"), Details->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("post-save failure reports the failed verification"), Details->GetStringField(TEXT("post_save_status")), FString(TEXT("failed")));
		TestTrue(TEXT("post-save failure keeps the bounded diagnostics"),
			Details->GetArrayField(TEXT("diagnostics")).Num() > 0);
		TestTrue(TEXT("post-save failure keeps the diagnostic bound"),
			Details->GetArrayField(TEXT("diagnostics")).Num() <= 16);
		TestNotEqual(TEXT("post-save failure is never reported as a rollback"), Details->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
	}

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 7. A blocked asset is refused before any inner patch work
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandBlockedAssetTest,
	"Cortex.Graph.Authoring.PatchCommand.BlockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandBlockedAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandBlocked_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	const FString HashBefore = GraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	// The guard is process-lifetime and fail-closed, so a uniquely named fixture is blocked.
	FCortexAssetMutationGuard::Block(Fixture.Blueprint, TEXT("unverified recovery in test"));

	FOperations Operations;
	Operations.Begin();
	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f000b"), nullptr);
	const FCortexCommandResult Blocked = Router.Execute(TEXT("graph.apply_patch"), Request);
	Operations.End();

	TestFalse(TEXT("a blocked asset is refused"), Blocked.bSuccess);
	TestEqual(TEXT("blocked asset error code"), Blocked.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the refusal names the blocked asset"), Blocked.ErrorMessage.Contains(TEXT("blocked after failed recovery")));
	AssertCompactRefusal(*this, Blocked, TEXT("00000000-0000-0000-0000-0000000f000b"), false, false);
	TestTrue(TEXT("the guard refusal keeps the guard reason in its diagnostics"),
		Blocked.ErrorDetails.IsValid()
			&& Blocked.ErrorDetails->GetArrayField(TEXT("diagnostics"))[0]->AsString().Contains(TEXT("blocked after failed recovery")));
	TestEqual(TEXT("a blocked asset performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("a blocked asset performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("a blocked asset leaves the authoring fingerprint untouched"), GraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("a blocked asset leaves the node count untouched"), CountNativeNodes(Fixture.Blueprint), NodesBefore);

	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 8. A rollback-enabled batch refuses apply_patch before the inner patch runs
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandBatchRefusalTest,
	"Cortex.Graph.Authoring.PatchCommand.RollbackBatchRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandBatchRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexGraphPatchCommandTest::ClearFaults();
	using namespace CortexGraphPatchCommandTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandBatchRefusal_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	TestTrue(TEXT("baseline fixture saved to disk"), Fixture.SaveToDisk());
	FCortexCommandRouter Router = MakeRouter();
	const FString HashBefore = GraphHash(Fixture.Blueprint);
	const int32 NodesBefore = CountNativeNodes(Fixture.Blueprint);

	TSharedPtr<FJsonObject> Request = IntentRequest(Fixture.Blueprint, TEXT("00000000-0000-0000-0000-0000000f000c"), nullptr);
	const TSharedPtr<FJsonObject> PreviewData = Preview(*this, Router, Request);
	if (!PreviewData.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	// A fully valid apply request: only the outer batch contract can refuse it.
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetBoolField(TEXT("save"), true);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));

	TSharedPtr<FJsonObject> Batch = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("command"), TEXT("graph.apply_patch"));
	Step->SetObjectField(TEXT("params"), Request);
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	Batch->SetArrayField(TEXT("commands"), Steps);
	Batch->SetBoolField(TEXT("stop_on_error"), true);
	Batch->SetBoolField(TEXT("rollback_on_error"), true);
	Batch->SetBoolField(TEXT("verify_rollback"), true);

	FOperations Operations;
	Operations.Begin();
	const FCortexCommandResult BatchResult = Router.Execute(TEXT("batch"), Batch);
	Operations.End();

	TestFalse(TEXT("a rollback-enabled batch containing apply_patch fails"), BatchResult.bSuccess);
	TestTrue(TEXT("the batch failure carries the per-step results"),
		BatchResult.ErrorDetails.IsValid() && BatchResult.ErrorDetails->HasField(TEXT("results")));
	if (BatchResult.ErrorDetails.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Results = BatchResult.ErrorDetails->GetArrayField(TEXT("results"));
		TestEqual(TEXT("the batch refused exactly one step"), Results.Num(), 1);
		if (Results.Num() == 1)
		{
			const TSharedPtr<FJsonObject> StepResult = Results[0]->AsObject();
			TestFalse(TEXT("the refused step did not succeed"), StepResult->GetBoolField(TEXT("success")));
			TestEqual(TEXT("the refusal is an invalid operation"),
				StepResult->GetStringField(TEXT("error_code")), FString(CortexErrorCodes::InvalidOperation));
			TestTrue(TEXT("the refusal names the rollback-enabled batch contract"),
				StepResult->GetStringField(TEXT("error_message")).Contains(TEXT("rollback-enabled batches")));
		}
	}
	TestEqual(TEXT("the refused inner patch performs no compile"), Operations.TargetCompiles, 0);
	TestEqual(TEXT("the refused inner patch performs no save"), Operations.Saves, 0);
	TestEqual(TEXT("the refused inner patch leaves the authoring fingerprint untouched"), GraphHash(Fixture.Blueprint), HashBefore);
	TestEqual(TEXT("the refused inner patch leaves the node count untouched"), CountNativeNodes(Fixture.Blueprint), NodesBefore);
	TestFalse(TEXT("the refused inner patch leaves the saved package clean"), Fixture.Package->IsDirty());

	DeleteFixtureFile(Fixture.Filename);
	Fixture.Cleanup();
	return true;
}

// ---------------------------------------------------------------------------
// 9. The published schema is the native registration, including its limits
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCommandSchemaTest,
	"Cortex.Graph.Authoring.PatchCommand.Schema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCommandSchemaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchCommandTest;

	const FCortexGraphCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	const FCortexCommandInfo* ApplyPatch = Commands.FindByPredicate(
		[](const FCortexCommandInfo& Info) { return Info.Name == TEXT("apply_patch"); });
	TestNotNull(TEXT("apply_patch is registered"), ApplyPatch);
	if (ApplyPatch == nullptr)
	{
		return false;
	}

	TestEqual(TEXT("required envelope fields"), FString::Join(ParamNames(*ApplyPatch, true), TEXT(",")),
		FString(TEXT("asset_path,target,patch_id,expected_fingerprint")));
	TestEqual(TEXT("optional envelope fields"), FString::Join(ParamNames(*ApplyPatch, false), TEXT(",")),
		FString(TEXT("nodes,connections,pin_updates,migration,dry_run,compile,save,allow_noop,expected_validation_hash")));
	TestFalse(TEXT("apply_patch never opts into the rollback-safe contract"),
		ApplyPatch->bRollbackSafe);
	TestTrue(TEXT("the published description declares the batch restriction"),
		ApplyPatch->Description.Contains(TEXT("rollback-enabled batch")));
	TestTrue(TEXT("the published description declares the standalone contract"),
		ApplyPatch->Description.Contains(TEXT("Standalone command")));
	TestTrue(TEXT("the published description declares the migration shell"),
		ApplyPatch->Description.Contains(TEXT("migration.op=\"replace_entry\"")));

	const FCortexParamInfo* NodesParam = ApplyPatch->Params.FindByPredicate(
		[](const FCortexParamInfo& Param) { return Param.Name == TEXT("nodes"); });
	TestNotNull(TEXT("the nodes parameter is published"), NodesParam);
	if (NodesParam != nullptr)
	{
		TestTrue(TEXT("the published node families are the authoring families"),
			NodesParam->Description.Contains(TEXT(
				"Supported families: CallFunction, VariableGet, VariableSet, Self, DynamicCast, ConstructObject, Event.")));
		TestTrue(TEXT("the published nodes parameter declares its conditional requirement"),
			NodesParam->Description.Contains(TEXT("Required for an authoring request")));
	}

	const FCortexParamInfo* MigrationParam = ApplyPatch->Params.FindByPredicate(
		[](const FCortexParamInfo& Param) { return Param.Name == TEXT("migration"); });
	TestNotNull(TEXT("the migration parameter is published"), MigrationParam);
	if (MigrationParam != nullptr)
	{
		TestFalse(TEXT("the migration parameter is optional"), MigrationParam->bRequired);
		TestTrue(TEXT("the published migration selector names the operation"),
			MigrationParam->Description.Contains(TEXT("replace_entry")));
		TestTrue(TEXT("the published migration selector names its required target"),
			MigrationParam->Description.Contains(TEXT("target.implementation")));
		TestTrue(TEXT("the published migration selector refuses mixed authoring arrays"),
			MigrationParam->Description.Contains(TEXT("nodes/connections/pin_updates")));
		// No T12/T13 operation is published by this task.
		TestFalse(TEXT("copy_subgraph is not published"),
			MigrationParam->Description.Contains(TEXT("copy_subgraph")));
		TestFalse(TEXT("move_subgraph is not published"),
			MigrationParam->Description.Contains(TEXT("move_subgraph")));
		TestFalse(TEXT("prune_island is not published"),
			MigrationParam->Description.Contains(TEXT("prune_island")));
	}

	// No drift: every patch limit published in the description equals the live authoring limit.
	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_PatchCommandSchema_T10")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	TSharedPtr<FJsonObject> ContextParams = MakeShared<FJsonObject>();
	ContextParams->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
	const FCortexCommandResult Context = Router.Execute(TEXT("graph.get_authoring_context"), ContextParams);
	TestTrue(FString::Printf(TEXT("authoring context succeeds: %s"), *Context.ErrorMessage), Context.bSuccess);
	if (Context.bSuccess && Context.Data.IsValid())
	{
		const TArray<FString> LimitKeys = {
			TEXT("max_nodes"), TEXT("max_edges"), TEXT("max_client_id_length"),
			TEXT("max_request_size_bytes"), TEXT("max_scanned_nodes")
		};
		const TSharedPtr<FJsonObject> Limits = Context.Data->GetObjectField(TEXT("limits"));
		TestTrue(TEXT("the authoring context publishes limits"), Limits.IsValid());
		if (Limits.IsValid())
		{
			for (const FString& Key : LimitKeys)
			{
				const FString Expected = FString::Printf(TEXT("%s=%d"), *Key, static_cast<int32>(Limits->GetNumberField(Key)));
				TestTrue(FString::Printf(TEXT("published limit matches the live authoring limit (%s)"), *Expected),
					ApplyPatch->Description.Contains(Expected));
			}
		}
	}

	Fixture.Cleanup();
	return true;
}

#endif
