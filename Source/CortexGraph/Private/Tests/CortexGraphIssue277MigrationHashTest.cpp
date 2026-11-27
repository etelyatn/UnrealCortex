// Issue 277 (Blueprint review): the migration validation hash must commit to the LIVE fingerprint, never to the shape of the
// caller-supplied expected_fingerprint. A preview whose fingerprint is valid but partial (compiled_signature_crc is not
// required) must hash the same as a preview with the full live fingerprint, and a later apply that carries the full live
// fingerprint (what the Blueprint wrapper injects for a hash-only apply) must accept that preview's hash.

#include "Misc/AutomationTest.h"

#include "CortexGraphReplaceInterfaceTestSupport.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277MigrationHashLiveFingerprintTest,
	"Cortex.Graph.Issue277.MigrationHash.LiveFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277MigrationHashLiveFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue277Hash"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue277Hash"), Impl)))
	{
		return false;
	}
	const FString OldPath = Interface->GetPathName();
	const TCHAR* const PatchId = TEXT("00000000-0000-0000-0000-000000277001");

	// A dirty loaded Blueprint exposes compiled_signature_crc in its fingerprint.
	Impl.Blueprint->MarkPackageDirty();

	const TSharedPtr<FJsonObject> Live = FCortexGraphPatchState::ComputeFingerprint(Impl.Blueprint);
	if (!TestTrue(TEXT("The live fingerprint carries compiled_signature_crc, so omitting it makes a partial fingerprint"),
		Live.IsValid() && Live->HasField(TEXT("compiled_signature_crc"))))
	{
		return false;
	}

	// Full live fingerprint: the reference hash.
	FCortexGraphPreparedPatch Full;
	FCortexCommandResult FullError;
	TestTrue(FString::Printf(TEXT("The full-fingerprint preview succeeds: %s"), *FullError.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Impl.Blueprint, Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap())), Full, FullError));

	// Valid but partial fingerprint: every required field, no compiled_signature_crc.
	const TSharedPtr<FJsonObject> PartialRequest = Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap()));
	const TSharedPtr<FJsonObject> Partial = FCortexGraphPatchState::ComputeFingerprint(Impl.Blueprint);
	Partial->RemoveField(TEXT("compiled_signature_crc"));
	PartialRequest->SetObjectField(TEXT("expected_fingerprint"), Partial);
	FCortexGraphPreparedPatch PartialPreview;
	FCortexCommandResult PartialError;
	if (!TestTrue(FString::Printf(TEXT("The partial-fingerprint preview succeeds: %s"), *PartialError.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Impl.Blueprint, PartialRequest, PartialPreview, PartialError)))
	{
		return false;
	}
	TestEqual(TEXT("The validation hash does not depend on which optional fingerprint fields the caller supplied"),
		PartialPreview.ValidationHash, Full.ValidationHash);

	// A hash-only apply: the partial preview's hash plus the full live fingerprint must not be stale on an unchanged asset.
	const TSharedPtr<FJsonObject> ApplyRequest = Request(Impl.Blueprint, PatchId, MigrationJson(OldPath, NativePath(), FullFunctionMap()), false);
	ApplyRequest->SetStringField(TEXT("expected_validation_hash"), PartialPreview.ValidationHash);
	FCortexGraphPreparedPatch ApplyPrepared;
	FCortexCommandResult ApplyError;
	const bool bPrepared = FCortexGraphPatchOps::Preflight(Impl.Blueprint, ApplyRequest, ApplyPrepared, ApplyError);
	TestTrue(FString::Printf(TEXT("The apply accepts the partial preview's validation hash (code %s: %s)"), *ApplyError.ErrorCode, *ApplyError.ErrorMessage), bPrepared);
	TestNotEqual(TEXT("The refusal, if any, is not STALE_PRECONDITION"), ApplyError.ErrorCode, FString(CortexErrorCodes::StalePrecondition));

	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
