#include "Operations/CortexBPGuardedRun.h"

#include "CortexAssetMutationGuard.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*)> GPostRestoreHookForTest;
}
#endif

namespace
{
	int32 TransactionQueueLength()
	{
		return (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : INDEX_NONE;
	}

	struct FRunState
	{
		bool bDryRun = false;
		FString ApplyStatus = TEXT("not_run");
		bool bChanged = false;
		FString CompileStatus = TEXT("skipped");
		FCortexBPCompileOutcome Compile;
		FString ReadbackStatus = TEXT("skipped");
		FString RollbackStatus = TEXT("not_needed");
		FString SaveStatus = TEXT("skipped");
		bool bSaved = false;
		bool bBlocked = false;
		bool bDirtyBefore = false;
		bool bDirtyAfter = false;
		TSharedPtr<FJsonObject> FingerprintBefore;
		TSharedPtr<FJsonObject> FingerprintAfter;
	};

	TSharedPtr<FJsonObject> ToOutcomeJson(const FRunState& State)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("dry_run"), State.bDryRun);
		Json->SetStringField(TEXT("apply_status"), State.ApplyStatus);
		Json->SetBoolField(TEXT("changed"), State.bChanged);
		Json->SetStringField(TEXT("compile_status"), State.CompileStatus);
		Json->SetNumberField(TEXT("error_count"), State.Compile.ErrorCount);
		Json->SetNumberField(TEXT("warning_count"), State.Compile.WarningCount);
		Json->SetArrayField(TEXT("diagnostics"), State.Compile.Diagnostics);
		Json->SetStringField(TEXT("readback_status"), State.ReadbackStatus);
		Json->SetStringField(TEXT("rollback_status"), State.RollbackStatus);
		Json->SetStringField(TEXT("save_status"), State.SaveStatus);
		Json->SetBoolField(TEXT("saved"), State.bSaved);
		Json->SetBoolField(TEXT("blocked"), State.bBlocked);
		Json->SetBoolField(TEXT("dirty_before"), State.bDirtyBefore);
		Json->SetBoolField(TEXT("dirty_after"), State.bDirtyAfter);
		Json->SetObjectField(TEXT("fingerprint_before"), State.FingerprintBefore);
		Json->SetObjectField(TEXT("fingerprint_after"), State.FingerprintAfter);
		return Json;
	}

	bool FingerprintsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A.IsValid() && B.IsValid() && FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(A), *MakeShared<FJsonValueObject>(B));
	}
}

FCortexCommandResult FCortexBPGuardedRun::Execute(const FSpec& Spec)
{
	UBlueprint* Blueprint = Spec.Blueprint;
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: no Blueprint to run against"), *Spec.OpName));
	}
	if (Spec.bDryRun && Spec.bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (Spec.bSave && !Spec.bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
	}
	if (!Spec.bDryRun && (!Spec.Apply || !Spec.Verify))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: apply and verify steps are required"), *Spec.OpName));
	}

	UPackage* Package = Blueprint->GetOutermost();
	FRunState State;
	State.bDryRun = Spec.bDryRun;
	State.bDirtyBefore = Package->IsDirty();
	State.bDirtyAfter = State.bDirtyBefore;

	if (!Spec.bDryRun)
	{
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		}
	}

	if (Spec.Expected.IsValid())
	{
		FCortexCommandResult ExpectedError;
		if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Spec.Expected, ExpectedError))
		{
			return ExpectedError;
		}
	}

	State.FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	State.FingerprintAfter = State.FingerprintBefore;

	if (Spec.bDryRun)
	{
		State.ApplyStatus = TEXT("dry_run");
		return FCortexCommandRouter::Success(ToOutcomeJson(State));
	}

	FString SnapshotError;
	const TSharedPtr<FJsonObject> SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, Spec.SnapshotSections, SnapshotError);
	if (!SnapshotBefore.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: could not record the pre-call snapshot: %s"), *Spec.OpName, *SnapshotError));
	}

	const int32 QueueBefore = TransactionQueueLength();
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
		FText::FromString(FString::Printf(TEXT("Cortex: %s"), *Spec.OpName)));
	Blueprint->Modify();

	FString FailurePhase;
	FString FailureMessage;
	FString FailureCode;
	if (!Spec.Apply(Blueprint, FailureMessage))
	{
		FailurePhase = TEXT("apply");
		FailureCode = CortexErrorCodes::InvalidOperation;
		State.ApplyStatus = TEXT("failed");
	}
	else
	{
		State.ApplyStatus = TEXT("applied");
		State.bChanged = true;
		if (Spec.bCompile)
		{
			FCortexBPCompileSupport::Compile(Blueprint, Spec.CompileOptions, State.Compile);
			State.CompileStatus = State.Compile.Status;
			if (State.Compile.HasErrors() || Blueprint->Status == BS_Error)
			{
				FailurePhase = TEXT("compile");
				FailureCode = CortexErrorCodes::CompileFailed;
				FailureMessage = FString::Printf(TEXT("Blueprint compilation failed with %d errors"), State.Compile.ErrorCount);
			}
		}
		if (FailurePhase.IsEmpty())
		{
			if (Spec.Verify(Blueprint, Spec.bCompile, FailureMessage))
			{
				State.ReadbackStatus = TEXT("matched");
			}
			else
			{
				State.ReadbackStatus = TEXT("mismatched");
				FailurePhase = TEXT("readback");
				FailureCode = CortexErrorCodes::ReadbackMismatch;
			}
		}
	}

	if (!FailurePhase.IsEmpty())
	{
		// End the guarded transaction (it is recorded, not cancelled: Cancel restores nothing), then restore.
		Transaction.Reset();
		FString RestoreError;
		bool bRestored = true;
		if (Spec.Restore)
		{
			bRestored = Spec.Restore(Blueprint, RestoreError);
		}
		else if (TransactionQueueLength() > QueueBefore)
		{
			bRestored = GEditor != nullptr && GEditor->UndoTransaction(/*bCanRedo=*/false);
		}

		FCortexBPCompileOutcome Recovery;
		if (bRestored && Spec.bCompile)
		{
			FCortexBPCompileSupport::Compile(Blueprint, EBlueprintCompileOptions::None, Recovery);
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (GPostRestoreHookForTest)
		{
			GPostRestoreHookForTest(Blueprint);
		}
#endif

		FString AfterError;
		const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, Spec.SnapshotSections, AfterError);
		bool bSnapshotsEqual = false;
		if (SnapshotAfter.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> Differences;
			TSharedPtr<FJsonObject> Counts;
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(SnapshotBefore, SnapshotAfter, Spec.SnapshotSections, Differences, Counts);
			bSnapshotsEqual = Diff.bSuccess && Differences.Num() == 0;
		}
		const TSharedPtr<FJsonObject> FingerprintRestored = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		const bool bFingerprintEqual = FingerprintsEqual(FingerprintRestored, State.FingerprintBefore);
		const bool bVerified = bRestored && bSnapshotsEqual && bFingerprintEqual;

		if (bVerified)
		{
			Package->SetDirtyFlag(State.bDirtyBefore);
			State.RollbackStatus = TEXT("restored");
			State.bChanged = false;
		}
		else
		{
			Package->SetDirtyFlag(true);
			FCortexAssetMutationGuard::Block(Blueprint,
				FString::Printf(TEXT("%s rollback verification failed"), *Spec.OpName));
			State.RollbackStatus = TEXT("unverified");
			State.bBlocked = true;
		}
		if (Spec.bCompile)
		{
			State.CompileStatus = FailurePhase == TEXT("compile") ? TEXT("failed") : State.CompileStatus;
		}
		State.FingerprintAfter = FingerprintRestored;
		State.bDirtyAfter = Package->IsDirty();

		TSharedPtr<FJsonObject> Details = ToOutcomeJson(State);
		Details->SetStringField(TEXT("failure_phase"), FailurePhase);
		Details->SetStringField(TEXT("failure_message"), FailureMessage);
		Details->SetBoolField(TEXT("rollback_content_restored"), bRestored);
		Details->SetBoolField(TEXT("rollback_snapshot_matches"), bSnapshotsEqual);
		Details->SetBoolField(TEXT("rollback_fingerprint_matches"), bFingerprintEqual);
		if (!RestoreError.IsEmpty())
		{
			Details->SetStringField(TEXT("rollback_error"), RestoreError);
		}
		return FCortexCommandRouter::Error(FailureCode,
			FString::Printf(TEXT("%s %s failed: %s"), *Spec.OpName, *FailurePhase, *FailureMessage), Details);
	}

	Transaction.Reset();
	Package->MarkPackageDirty();

	if (Spec.bSave)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		SaveParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		const FCortexCommandResult SaveResult = FCortexBPAssetOps::Save(SaveParams);
		State.SaveStatus = SaveResult.bSuccess ? TEXT("saved") : TEXT("failed");
		State.bSaved = SaveResult.bSuccess;
		State.FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		State.bDirtyAfter = Package->IsDirty();
		if (!SaveResult.bSuccess)
		{
			FCortexCommandResult SaveError = FCortexCommandRouter::Error(
				SaveResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : SaveResult.ErrorCode,
				SaveResult.ErrorMessage, ToOutcomeJson(State));
			return SaveError;
		}
	}

	State.FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	State.bDirtyAfter = Package->IsDirty();
	return FCortexCommandRouter::Success(ToOutcomeJson(State));
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPGuardedRun::SetPostRestoreHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	GPostRestoreHookForTest = MoveTemp(Hook);
}

void FCortexBPGuardedRun::ClearPostRestoreHookForTest()
{
	GPostRestoreHookForTest = nullptr;
}
#endif
