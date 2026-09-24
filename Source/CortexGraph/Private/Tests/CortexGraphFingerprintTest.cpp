#include "Misc/AutomationTest.h"
#include "CortexGraphFingerprint.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexGraphPatchState.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphFingerprintTest
{
UBlueprint* CreateBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint)
	{
		Blueprint->UbergraphPages.Reset();
		Blueprint->FunctionGraphs.Reset();
		Blueprint->MacroGraphs.Reset();
		Blueprint->DelegateSignatureGraphs.Reset();
		Blueprint->ImplementedInterfaces.Reset();
		Blueprint->NewVariables.Reset();

		UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, FName(TEXT("FingerprintGraph")));
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		Graph->GraphGuid = FGuid(0xA1B2C3D4, 0x01020304, 0x50607080, 0x90ABCDEF);
		Blueprint->UbergraphPages.Add(Graph);
		OutPackage->MarkPackageDirty();
	}
	return Blueprint;
}

void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (Blueprint)
	{
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->SetDirtyFlag(false);
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphFingerprintParityTest,
	"Cortex.Graph.Fingerprint.Parity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphFingerprintParityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphFingerprintTest::CreateBlueprint(
		Package, TEXT("BP_GraphFingerprint_Parity"));
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphFingerprintTest::Cleanup(Package, nullptr);
		return false;
	}

	const TSharedPtr<FJsonObject> Shared = FCortexGraphFingerprint::Compute(Blueprint);
	const TSharedPtr<FJsonObject> Core = MakeObjectAssetFingerprint(Blueprint).ToJson();
	TestTrue(TEXT("fingerprint and core asset authority return values"), Shared.IsValid() && Core.IsValid());
	if (Shared.IsValid() && Core.IsValid())
	{
		const FString ExpectedGraphHash = TEXT("11199164ca81589f153dff34ba72e741238a7784");
		TestEqual(TEXT("graph-authoring hash matches the pre-extraction fixture oracle"),
			Shared->GetStringField(TEXT("graph_authoring_hash")), ExpectedGraphHash);
		TestEqual(TEXT("patch-state façade retains the pre-extraction graph-authoring hash"),
			FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")),
			ExpectedGraphHash);
		TestEqual(TEXT("saved-package hash matches Core"), Shared->GetStringField(TEXT("package_saved_hash")),
			Core->GetStringField(TEXT("package_saved_hash")));
		TestEqual(TEXT("dirty state matches Core"), Shared->GetBoolField(TEXT("is_dirty")),
			Core->GetBoolField(TEXT("is_dirty")));
		TestEqual(TEXT("dirty epoch matches Core"), Shared->GetStringField(TEXT("dirty_epoch")),
			Core->GetStringField(TEXT("dirty_epoch")));
		TestEqual(TEXT("not-ready state matches Core"), Shared->GetBoolField(TEXT("not_ready")),
			Core->GetBoolField(TEXT("not_ready")));
		TestEqual(TEXT("compiled-signature field presence matches Core"),
			Shared->HasField(TEXT("compiled_signature_crc")), Core->HasField(TEXT("compiled_signature_crc")));
		if (Shared->HasField(TEXT("compiled_signature_crc")) && Core->HasField(TEXT("compiled_signature_crc")))
		{
			TestEqual(TEXT("compiled-signature CRC matches Core"),
				Shared->GetNumberField(TEXT("compiled_signature_crc")),
				Core->GetNumberField(TEXT("compiled_signature_crc")));
		}
		TestEqual(TEXT("graph-authoring version remains version one"),
			Shared->GetNumberField(TEXT("graph_authoring_version")), 1.0);
	}

	const FString ExpectedGeneratedStateDigest =
		TEXT("GeneratedClass: /Temp/BP_GraphFingerprint_Parity.BP_GraphFingerprint_Parity_C\n")
		TEXT("SuperClass: /Script/Engine.Actor\n")
		TEXT("ParentClass: /Script/Engine.Actor\n")
		TEXT("Functions: 0\n");
	TestEqual(TEXT("generated-state digest matches the pre-extraction fixture oracle"),
		FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint), ExpectedGeneratedStateDigest);
	TestEqual(TEXT("patch-state digest façade preserves generated-state semantics"),
		FCortexGraphPatchState::ComputeGeneratedStateDigest(Blueprint), ExpectedGeneratedStateDigest);
	CortexGraphFingerprintTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphFingerprintDirtyMutationTest,
	"Cortex.Graph.Fingerprint.DirtyMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphFingerprintDirtyMutationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphFingerprintTest::CreateBlueprint(
		Package, TEXT("BP_GraphFingerprint_DirtyMutation"));
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphFingerprintTest::Cleanup(Package, nullptr);
		return false;
	}

	UEdGraph* Graph = Blueprint->UbergraphPages.IsEmpty() ? nullptr : Blueprint->UbergraphPages[0];
	TestNotNull(TEXT("event graph exists"), Graph);
	if (!Graph)
	{
		CortexGraphFingerprintTest::Cleanup(Package, Blueprint);
		return false;
	}

	const TSharedPtr<FJsonObject> Before = FCortexGraphFingerprint::Compute(Blueprint);
	const FString SavedPackageHashBefore = Before->GetStringField(TEXT("package_saved_hash"));
	const FString GraphHashBefore = Before->GetStringField(TEXT("graph_authoring_hash"));
	UEdGraphNode* DirtyNode = NewObject<UEdGraphNode>(Graph);
	DirtyNode->CreateNewGuid();
	DirtyNode->NodePosX = 17;
	Graph->AddNode(DirtyNode);

	const TSharedPtr<FJsonObject> After = FCortexGraphFingerprint::Compute(Blueprint);
	TestNotEqual(TEXT("dirty graph mutation changes graph-authoring hash"),
		After->GetStringField(TEXT("graph_authoring_hash")), GraphHashBefore);
	TestEqual(TEXT("dirty graph mutation leaves saved-package hash unchanged"),
		After->GetStringField(TEXT("package_saved_hash")), SavedPackageHashBefore);

	CortexGraphFingerprintTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphFingerprintStalePreconditionTest,
	"Cortex.Graph.Fingerprint.StalePrecondition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphFingerprintStalePreconditionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphFingerprintTest::CreateBlueprint(
		Package, TEXT("BP_GraphFingerprint_StalePrecondition"));
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphFingerprintTest::Cleanup(Package, nullptr);
		return false;
	}

	UEdGraph* Graph = Blueprint->UbergraphPages.IsEmpty() ? nullptr : Blueprint->UbergraphPages[0];
	TestNotNull(TEXT("event graph exists"), Graph);
	if (!Graph)
	{
		CortexGraphFingerprintTest::Cleanup(Package, Blueprint);
		return false;
	}

	const TSharedPtr<FJsonObject> Expected = FCortexGraphFingerprint::Compute(Blueprint);
	UEdGraphNode* MutatedNode = NewObject<UEdGraphNode>(Graph);
	MutatedNode->CreateNewGuid();
	MutatedNode->NodePosX = 29;
	Graph->AddNode(MutatedNode);
	const TSharedPtr<FJsonObject> Current = FCortexGraphFingerprint::Compute(Blueprint);

	FCortexCommandResult Error;
	TestFalse(TEXT("mutation invalidates the retained precondition"),
		FCortexGraphFingerprint::ValidatePrecondition(Expected, Current, Error));
	TestEqual(TEXT("invalidated precondition keeps the stale error family"),
		Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	CortexGraphFingerprintTest::Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
