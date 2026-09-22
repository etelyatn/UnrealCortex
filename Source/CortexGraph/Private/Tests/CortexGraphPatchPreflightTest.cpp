#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#if WITH_EDITOR
namespace CortexGraphPatchPreflightTest
{
static void Cleanup(UPackage* Package, UObject* Asset)
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

static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000000006"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : FString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightMalformedEnvelopeTest,
	"Cortex.Graph.Authoring.Preflight.MalformedEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightMalformedEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightMalformed_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetStringField(TEXT("nodes"), TEXT("not-an-array"));
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error);
	TestFalse(TEXT("malformed nodes type is rejected"), bReady);
	TestEqual(TEXT("malformed nodes error"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightNoMutationTest,
	"Cortex.Graph.Authoring.Preflight.NoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightNoMutationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightNoMutation_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	const FString Before = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	const bool bDirtyBefore = Package->IsDirty();
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error);
	TestTrue(FString::Printf(TEXT("valid preview succeeds: %s"), *Error.ErrorMessage), bReady);
	TestFalse(TEXT("preview does not retain raw target UObject"), Prepared.HasTransientObjects());
	TestFalse(TEXT("preview does not mutate dirty state"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("preview leaves graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), Before);
	TestFalse(TEXT("preview has a validation hash only on success"), Prepared.ValidationHash.IsEmpty());

	const FString PreviewHash = Prepared.ValidationHash;
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewHash);
	FCortexGraphPreparedPatch ApplyPrepared;
	FCortexCommandResult ApplyError;
	const bool bApplyReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, ApplyPrepared, ApplyError);
	TestTrue(FString::Printf(TEXT("apply intent with preview token succeeds: %s"), *ApplyError.ErrorMessage), bApplyReady);
	TestEqual(TEXT("dry_run does not change semantic validation token"), ApplyPrepared.ValidationHash, PreviewHash);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightStrictFlagsTest,
	"Cortex.Graph.Authoring.Preflight.StrictFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightStrictFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightFlags_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("save"), true);
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("preview save=true is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("preview save rejection code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), true);
	TestFalse(TEXT("save=true without compile is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("save compile combination rejection code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightEnvelopeCasesTest,
	"Cortex.Graph.Authoring.Preflight.EnvelopeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightEnvelopeCasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightEnvelope_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetStringField(TEXT("unknown"), TEXT("reject"));
	TestFalse(TEXT("unknown envelope field rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("unknown field code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("dry_run"), false);
	TestFalse(TEXT("apply without preview token rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("missing token code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	Request->TryGetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	if (Fingerprint && Fingerprint->IsValid()) (*Fingerprint)->RemoveField(TEXT("graph_authoring_hash"));
	TestFalse(TEXT("missing graph hash rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("missing graph hash code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> FirstNode = MakeShared<FJsonObject>();
	FirstNode->SetStringField(TEXT("client_id"), TEXT("same"));
	FirstNode->SetStringField(TEXT("node_class"), TEXT("Self"));
	TSharedPtr<FJsonObject> SecondNode = MakeShared<FJsonObject>();
	SecondNode->SetStringField(TEXT("client_id"), TEXT("same"));
	SecondNode->SetStringField(TEXT("node_class"), TEXT("Self"));
	TArray<TSharedPtr<FJsonValue>> DuplicateNodes;
	DuplicateNodes.Add(MakeShared<FJsonValueObject>(FirstNode));
	DuplicateNodes.Add(MakeShared<FJsonValueObject>(SecondNode));
	Request->SetArrayField(TEXT("nodes"), DuplicateNodes);
	TestFalse(TEXT("duplicate client IDs rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("duplicate client ID code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TArray<TSharedPtr<FJsonValue>> OversizeNodes;
	for (int32 Index = 0; Index < 65; ++Index) OversizeNodes.Add(MakeShared<FJsonValueNull>());
	Request->SetArrayField(TEXT("nodes"), OversizeNodes);
	TestFalse(TEXT("oversize node request rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("oversize node code"), Error.ErrorCode, CortexErrorCodes::LimitExceeded);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

#endif
