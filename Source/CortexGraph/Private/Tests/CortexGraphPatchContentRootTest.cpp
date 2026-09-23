#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * The patch route must apply the same normalized writable-content-root policy as the existing
 * graph mutators. A resolvable, fingerprint-matching Blueprint under a mounted root the policy does
 * not authorize (here the Engine content mount) is refused at preview and at apply, and nothing is
 * mutated, marked dirty or written to disk. Read-only inspection of the same asset stays available
 * because the refusal lives on the mutating route only.
 *
 * The fixtures are in-memory packages: no real engine asset is loaded or saved, and the positive
 * case uses the sanctioned test-only writable root registration rather than a second allowlist.
 */
namespace CortexGraphPatchContentRootTest
{
/** A package root the shared policy authorizes for tests only. */
const TCHAR* TestWritableRoot = TEXT("/CortexTestContentRoot");

/** A mounted root the shared policy never authorizes: the Engine content mount. */
const TCHAR* DisallowedRoot = TEXT("/Engine/CortexWritableRootProbe");

/**
 * Registers one test-only content root as a real mount point and authorizes it for the scope, so the
 * positive fixture is a mounted package root rather than a policy-only string. Mirrors the shared
 * mounted-path fixture pattern used by the CortexBlueprint and CortexStateTree root tests.
 */
struct FMountedTestRoot
{
	FString Root;
	FString PhysicalDir;

	explicit FMountedTestRoot(const FString& InRoot)
		: Root(InRoot)
	{
		PhysicalDir = FPaths::ProjectSavedDir() / TEXT("CortexGraphContentRootTests") / Root.RightChop(1);
		IFileManager::Get().MakeDirectory(*PhysicalDir, true);
		FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
		FCortexEditorUtils::AddTestWritableContentRoot(Root);
	}

	~FMountedTestRoot()
	{
		FCortexEditorUtils::RemoveTestWritableContentRoot(Root);
		FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
		IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
	}
};

void Cleanup(UPackage* Package, UObject* Asset)
{
	if (!Package) return;
	Package->SetDirtyFlag(false);
	if (Asset)
	{
		Asset->ClearFlags(RF_Standalone);
		Asset->MarkAsGarbage();
	}
	Package->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
}

UBlueprint* MakeBlueprint(UPackage*& OutPackage, const FString& PackagePath, const FName AssetName)
{
	OutPackage = CreatePackage(*PackagePath);
	if (!OutPackage) return nullptr;
	OutPackage->SetDirtyFlag(false);
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, AssetName, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

/** A structurally valid, change-free patch request targeting the EventGraph of the fixture. */
TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-00000000c001"));
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : FString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetArrayField(TEXT("pin_updates"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), true);
	return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchContentRootRefusalTest,
	"Cortex.Graph.Authoring.Patch.ContentRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchContentRootRefusalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// --- A mounted root the shared policy does not authorize is refused before any mutation ---
	UPackage* DisallowedPackage = nullptr;
	const FString DisallowedPackagePath = FString::Printf(TEXT("%s/BP_PatchContentRootRefusal"),
		CortexGraphPatchContentRootTest::DisallowedRoot);
	UBlueprint* DisallowedBlueprint = CortexGraphPatchContentRootTest::MakeBlueprint(
		DisallowedPackage, DisallowedPackagePath, TEXT("BP_PatchContentRootRefusal"));
	TestNotNull(TEXT("disallowed-root fixture Blueprint created"), DisallowedBlueprint);
	if (!DisallowedBlueprint)
	{
		CortexGraphPatchContentRootTest::Cleanup(DisallowedPackage, nullptr);
		return false;
	}

	FString PolicyError;
	TestFalse(TEXT("the fixture root is outside the writable policy"),
		FCortexEditorUtils::IsWritableMountedContentPath(DisallowedPackagePath, PolicyError));
	TestTrue(TEXT("the policy error names the offending root"), PolicyError.Contains(TEXT("/Engine")));

	const int32 NodeCountBefore = DisallowedBlueprint->UbergraphPages.Num() > 0
		? DisallowedBlueprint->UbergraphPages[0]->Nodes.Num() : 0;
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(DisallowedBlueprint)
		->GetStringField(TEXT("graph_authoring_hash"));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bPreviewReady = FCortexGraphPatchOps::Preflight(
		DisallowedBlueprint, CortexGraphPatchContentRootTest::BaseRequest(DisallowedBlueprint), Prepared, Error);
	TestFalse(TEXT("preview of a non-writable target is refused"), bPreviewReady);
	TestEqual(TEXT("preview refusal uses the shared invalid-field code"), Error.ErrorCode,
		FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("preview refusal names the non-writable root"), Error.ErrorMessage.Contains(TEXT("/Engine")));
	TestFalse(TEXT("refused preview leaves the package clean"), DisallowedPackage->IsDirty());
	TestEqual(TEXT("refused preview leaves the graph unchanged"),
		DisallowedBlueprint->UbergraphPages.Num() > 0 ? DisallowedBlueprint->UbergraphPages[0]->Nodes.Num() : 0,
		NodeCountBefore);
	TestEqual(TEXT("refused preview leaves the authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(DisallowedBlueprint)->GetStringField(TEXT("graph_authoring_hash")),
		FingerprintBefore);
	TestFalse(TEXT("refused preview wrote no bytes to disk"),
		FPackageName::DoesPackageExist(DisallowedPackagePath));

	// Apply must be refused by the same shared authority, so an approval token can never reach the
	// mutation coordinator for a target the policy excludes.
	TSharedPtr<FJsonObject> ApplyRequest = CortexGraphPatchContentRootTest::BaseRequest(DisallowedBlueprint);
	ApplyRequest->SetBoolField(TEXT("dry_run"), false);
	ApplyRequest->SetStringField(TEXT("expected_validation_hash"), TEXT("0000000000000000000000000000000000000000"));
	FCortexGraphPatchOutcome ApplyOutcome;
	FCortexCommandResult ApplyError;
	const bool bApplyReady = FCortexGraphPatchOps::Execute(
		DisallowedBlueprint, ApplyRequest, ApplyOutcome, ApplyError);
	TestFalse(TEXT("apply of a non-writable target is refused"), bApplyReady);
	TestEqual(TEXT("apply refusal uses the shared invalid-field code"), ApplyError.ErrorCode,
		FString(CortexErrorCodes::InvalidField));
	TestFalse(TEXT("refused apply leaves the package clean"), DisallowedPackage->IsDirty());
	TestFalse(TEXT("refused apply wrote no bytes to disk"),
		FPackageName::DoesPackageExist(DisallowedPackagePath));
	CortexGraphPatchContentRootTest::Cleanup(DisallowedPackage, DisallowedBlueprint);

	// --- The same request against a policy-authorized root is accepted ---
	CortexGraphPatchContentRootTest::FMountedTestRoot MountedRoot(CortexGraphPatchContentRootTest::TestWritableRoot);
	UPackage* WritablePackage = nullptr;
	const FString WritablePackagePath = FString::Printf(TEXT("%s/BP_PatchContentRootAllowed"),
		CortexGraphPatchContentRootTest::TestWritableRoot);
	UBlueprint* WritableBlueprint = CortexGraphPatchContentRootTest::MakeBlueprint(
		WritablePackage, WritablePackagePath, TEXT("BP_PatchContentRootAllowed"));
	TestNotNull(TEXT("authorized-root fixture Blueprint created"), WritableBlueprint);
	if (WritableBlueprint)
	{
		FCortexGraphPreparedPatch WritablePrepared;
		FCortexCommandResult WritableError;
		const bool bWritableReady = FCortexGraphPatchOps::Preflight(
			WritableBlueprint, CortexGraphPatchContentRootTest::BaseRequest(WritableBlueprint),
			WritablePrepared, WritableError);
		TestTrue(FString::Printf(TEXT("preview of an authorized root succeeds: %s"), *WritableError.ErrorMessage),
			bWritableReady);
	}
	CortexGraphPatchContentRootTest::Cleanup(WritablePackage, WritableBlueprint);

	return true;
}

/**
 * The routed entry point must reach the same shared authority, so `graph_cmd` and the
 * `blueprint_compose` facade cannot bypass the policy by choosing a different dispatch path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchContentRootRoutedTest,
	"Cortex.Graph.Authoring.Patch.ContentRootRouted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchContentRootRoutedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	const FString PackagePath = FString::Printf(TEXT("%s/BP_PatchContentRootRouted"),
		CortexGraphPatchContentRootTest::DisallowedRoot);
	UBlueprint* Blueprint = CortexGraphPatchContentRootTest::MakeBlueprint(
		Package, PackagePath, TEXT("BP_PatchContentRootRouted"));
	TestNotNull(TEXT("routed fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphPatchContentRootTest::Cleanup(Package, nullptr);
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	const FCortexCommandResult Result = Router.Execute(
		TEXT("graph.apply_patch"), CortexGraphPatchContentRootTest::BaseRequest(Blueprint));
	TestFalse(TEXT("the routed preview refuses a non-writable target"), Result.bSuccess);
	TestEqual(TEXT("the routed refusal carries the shared code"), Result.ErrorCode,
		FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("the routed refusal names the non-writable root"),
		Result.ErrorMessage.Contains(TEXT("/Engine")));
	TestFalse(TEXT("the routed refusal wrote no bytes to disk"), FPackageName::DoesPackageExist(PackagePath));
	CortexGraphPatchContentRootTest::Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
