#include "Operations/CortexBPGuardedRun.h"

#include "CortexAssetMutationGuard.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexGraphFingerprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*)> GPostRestoreHookForTest;
	TFunction<void(UBlueprint*)> GPreRecoveryCompileHookForTest;
	bool GSkipUndoForTest = false;
	bool GForceSaveFailureForTest = false;
}
#endif

namespace
{
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

	FString GraphAuthoringHash(UBlueprint* Blueprint)
	{
		const TSharedPtr<FJsonObject> Fingerprint = FCortexGraphFingerprint::Compute(Blueprint);
		return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("graph_authoring_hash")) : FString();
	}

	/**
	 * Generic recovery journal: parent, NewVariables, interfaces, a DuplicateObject copy of every top-level graph (composite
	 * and nested graphs are subobjects, so they are copied with it) and the SCS / inheritable-component template properties.
	 */
	struct FJournal
	{
		UClass* Parent = nullptr;
		TArray<FBPVariableDescription> NewVariables;
		TArray<FBPInterfaceDescription> Interfaces;
		TStrongObjectPtr<UBlueprint> Holder;

		struct FGraphCopy
		{
			/** 0 ubergraph, 1 function, 2 macro, 3 delegate signature, 4 interface implementation graph. */
			int32 Category = 0;
			/** Category 4: the implemented interface that owns the graph. */
			UClass* Interface = nullptr;
			TStrongObjectPtr<UEdGraph> Copy;
		};
		TArray<FGraphCopy> Graphs;

		struct FScsState
		{
			FName Variable;
			FName ParentVariable;
			FName AttachTo;
			FName OwnerClassName;
			bool bParentNative = false;
		};
		TArray<FScsState> ScsStates;

		struct FTemplateCopy
		{
			FName Name;
			bool bInheritable = false;
			TStrongObjectPtr<UActorComponent> Copy;
		};
		TArray<FTemplateCopy> Templates;

		FString GraphHash;
		FString GeneratedDigest;
		FString ExtraDigest;
	};

	TArray<TObjectPtr<UEdGraph>>* GraphArray(UBlueprint* Blueprint, int32 Category)
	{
		switch (Category)
		{
		case 0: return &Blueprint->UbergraphPages;
		case 1: return &Blueprint->FunctionGraphs;
		case 2: return &Blueprint->MacroGraphs;
		default: return &Blueprint->DelegateSignatureGraphs;
		}
	}

	UInheritableComponentHandler* InheritableHandler(UBlueprint* Blueprint)
	{
		UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		return Class ? Class->GetInheritableComponentHandler(false) : nullptr;
	}

	bool CaptureJournal(UBlueprint* Blueprint, const FCortexBPGuardedRun::FSpec& Spec, FJournal& Journal)
	{
		Journal.Parent = Blueprint->ParentClass;
		Journal.NewVariables = Blueprint->NewVariables;
		Journal.Interfaces = Blueprint->ImplementedInterfaces;
		Journal.Holder = TStrongObjectPtr<UBlueprint>(NewObject<UBlueprint>(GetTransientPackage()));
		if (!Journal.Holder.IsValid())
		{
			return false;
		}

		const auto CopyGraphs = [&Journal](const TArray<TObjectPtr<UEdGraph>>& Graphs, int32 Category, UClass* Interface)
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph == nullptr)
				{
					continue;
				}
				UEdGraph* Copy = DuplicateObject<UEdGraph>(Graph, Journal.Holder.Get(), Graph->GetFName());
				if (Copy == nullptr)
				{
					return false;
				}
				FJournal::FGraphCopy& Saved = Journal.Graphs.AddDefaulted_GetRef();
				Saved.Category = Category;
				Saved.Interface = Interface;
				Saved.Copy = TStrongObjectPtr<UEdGraph>(Copy);
			}
			return true;
		};
		for (int32 Category = 0; Category < 4; ++Category)
		{
			if (!CopyGraphs(*GraphArray(Blueprint, Category), Category, nullptr))
			{
				return false;
			}
		}
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			if (!CopyGraphs(Description.Graphs, 4, Description.Interface))
			{
				return false;
			}
		}

		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node == nullptr)
				{
					continue;
				}
				FJournal::FScsState& State = Journal.ScsStates.AddDefaulted_GetRef();
				State.Variable = Node->GetVariableName();
				State.ParentVariable = Node->ParentComponentOrVariableName;
				State.AttachTo = Node->AttachToName;
				State.OwnerClassName = Node->ParentComponentOwnerClassName;
				State.bParentNative = Node->bIsParentComponentNative;
				if (Node->ComponentTemplate != nullptr)
				{
					UActorComponent* Copy = DuplicateObject<UActorComponent>(Node->ComponentTemplate, GetTransientPackage());
					if (Copy == nullptr)
					{
						return false;
					}
					FJournal::FTemplateCopy& Saved = Journal.Templates.AddDefaulted_GetRef();
					Saved.Name = Node->GetVariableName();
					Saved.Copy = TStrongObjectPtr<UActorComponent>(Copy);
				}
			}
		}
		if (UInheritableComponentHandler* Handler = InheritableHandler(Blueprint))
		{
			TArray<UActorComponent*> Overrides;
			Handler->GetAllTemplates(Overrides);
			for (UActorComponent* Template : Overrides)
			{
				if (Template == nullptr)
				{
					continue;
				}
				UActorComponent* Copy = DuplicateObject<UActorComponent>(Template, GetTransientPackage());
				if (Copy == nullptr)
				{
					return false;
				}
				FJournal::FTemplateCopy& Saved = Journal.Templates.AddDefaulted_GetRef();
				Saved.Name = Template->GetFName();
				Saved.bInheritable = true;
				Saved.Copy = TStrongObjectPtr<UActorComponent>(Copy);
			}
		}

		Journal.GraphHash = GraphAuthoringHash(Blueprint);
		Journal.GeneratedDigest = FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint);
		Journal.ExtraDigest = Spec.DigestExtra ? Spec.DigestExtra(Blueprint) : FString();
		return true;
	}

	/** Reconciles the Blueprint with the journal; idempotent after an undo that already restored everything. */
	bool RestoreFromJournal(UBlueprint* Blueprint, FJournal& Journal)
	{
		bool bOk = true;
		Blueprint->ParentClass = Journal.Parent;
		Blueprint->NewVariables = Journal.NewVariables;
		Blueprint->ImplementedInterfaces = Journal.Interfaces;

		if (GraphAuthoringHash(Blueprint) != Journal.GraphHash)
		{
			const auto Rebuild = [&](TArray<TObjectPtr<UEdGraph>>& Array, int32 Category, UClass* Interface)
			{
				TArray<UEdGraph*> Replaced;
				for (UEdGraph* Graph : Array)
				{
					if (Graph != nullptr)
					{
						Replaced.Add(Graph);
						Graph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
					}
				}
				Array.Reset();
				for (const FJournal::FGraphCopy& Saved : Journal.Graphs)
				{
					if (Saved.Category != Category || Saved.Interface != Interface)
					{
						continue;
					}
					UEdGraph* Fresh = DuplicateObject<UEdGraph>(Saved.Copy.Get(), Blueprint, Saved.Copy->GetFName());
					if (Fresh == nullptr)
					{
						bOk = false;
						continue;
					}
					Array.Add(Fresh);
				}
				for (UEdGraph* Old : Replaced)
				{
					Old->MarkAsGarbage();
				}
			};
			for (int32 Category = 0; Category < 4; ++Category)
			{
				Rebuild(*GraphArray(Blueprint, Category), Category, nullptr);
			}
			for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
			{
				Rebuild(Description.Graphs, 4, Description.Interface);
			}
		}

		if (Blueprint->SimpleConstructionScript != nullptr)
		{
			for (const FJournal::FScsState& State : Journal.ScsStates)
			{
				USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(State.Variable);
				if (Node == nullptr)
				{
					bOk = false; // a structural SCS change is only recoverable through the undo
					continue;
				}
				Node->ParentComponentOrVariableName = State.ParentVariable;
				Node->AttachToName = State.AttachTo;
				Node->ParentComponentOwnerClassName = State.OwnerClassName;
				Node->bIsParentComponentNative = State.bParentNative;
			}
		}
		for (const FJournal::FTemplateCopy& Saved : Journal.Templates)
		{
			UActorComponent* Current = nullptr;
			if (!Saved.bInheritable)
			{
				USCS_Node* Node = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(Saved.Name) : nullptr;
				Current = Node ? Node->ComponentTemplate : nullptr;
			}
			else if (UInheritableComponentHandler* Handler = InheritableHandler(Blueprint))
			{
				TArray<UActorComponent*> Overrides;
				Handler->GetAllTemplates(Overrides);
				for (UActorComponent* Candidate : Overrides)
				{
					if (Candidate != nullptr && Candidate->GetFName() == Saved.Name)
					{
						Current = Candidate;
						break;
					}
				}
			}
			if (Current == nullptr)
			{
				bOk = false;
				continue;
			}
			// Full copy, not a delta against the class default: a property the mutation moved away from its default must come back.
			UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
			CopyParams.bDoDelta = false;
			CopyParams.bNotifyObjectReplacement = false;
			UEngine::CopyPropertiesForUnrelatedObjects(Saved.Copy.Get(), Current, CopyParams);
		}
		return bOk;
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

	FJournal Journal;
	if (!CaptureJournal(Blueprint, Spec, Journal))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%s: could not record the recovery journal"), *Spec.OpName));
	}

	const EBlueprintStatus StatusBefore = Blueprint->Status;
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(
		FText::FromString(FString::Printf(TEXT("Cortex: %s"), *Spec.OpName)));
	Blueprint->Modify();
	// The guarded transaction is identified by id, not by undo-list growth: redo history or eviction keeps the length flat.
	const FGuid TransactionId = (GEditor && GEditor->Trans) ? GEditor->Trans->GetUndoContext(false).TransactionId : FGuid();

	FString FailurePhase;
	FString FailureMessage;
	FString FailureCode;
	if (Spec.Capture && !Spec.Capture(Blueprint, FailureMessage))
	{
		FailurePhase = TEXT("capture");
		FailureCode = CortexErrorCodes::InvalidOperation;
		State.ApplyStatus = TEXT("failed");
	}
	else if (!Spec.Apply(Blueprint, FailureMessage))
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
		const bool bParentChanged = Blueprint->ParentClass != Journal.Parent;

		// End the guarded transaction (recorded, not cancelled: Cancel restores nothing), then restore.
		Transaction.Reset();
		bool bUndone = true;
		bool bSkipUndo = false;
#if WITH_DEV_AUTOMATION_TESTS
		bSkipUndo = GSkipUndoForTest;
#endif
		if (!bSkipUndo && TransactionId.IsValid() && GEditor != nullptr && GEditor->Trans != nullptr
			&& GEditor->Trans->GetUndoContext(false).TransactionId == TransactionId)
		{
			bUndone = GEditor->UndoTransaction(/*bCanRedo=*/false);
		}
		const bool bJournalRestored = RestoreFromJournal(Blueprint, Journal);
		FString RestoreError;
		const bool bOwnRestored = !Spec.Restore || Spec.Restore(Blueprint, RestoreError);
		const bool bRestored = bJournalRestored && bOwnRestored && (bUndone || bSkipUndo);

		FCortexBPCompileOutcome Recovery;
		if (Spec.bCompile)
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (GPreRecoveryCompileHookForTest)
			{
				GPreRecoveryCompileHookForTest(Blueprint);
			}
#endif
			// Exactly one recovery compile; it reuses the failed compile's options when the parent changed in the run so
			// restored inherited CDO values are not overwritten by the new parent's constructor defaults.
			const EBlueprintCompileOptions RecoveryOptions = bParentChanged
				? State.Compile.EffectiveOptions
				: EBlueprintCompileOptions::None;
			FCortexBPCompileSupport::Compile(Blueprint, RecoveryOptions, Recovery);
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
		const bool bGraphEqual = GraphAuthoringHash(Blueprint) == Journal.GraphHash;
		const bool bGeneratedEqual = !Spec.bCompile
			|| FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint) == Journal.GeneratedDigest;
		const bool bExtraEqual = !Spec.DigestExtra || Spec.DigestExtra(Blueprint) == Journal.ExtraDigest;
		const bool bRecoveryCompileOk = !Spec.bCompile || !Recovery.HasErrors() || StatusBefore == BS_Error;
		const bool bVerified = bRestored && bRecoveryCompileOk && bSnapshotsEqual && bFingerprintEqual && bGraphEqual && bGeneratedEqual && bExtraEqual;

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
		Details->SetBoolField(TEXT("rollback_graphs_match"), bGraphEqual);
		Details->SetBoolField(TEXT("rollback_generated_state_matches"), bGeneratedEqual);
		Details->SetBoolField(TEXT("rollback_extra_digest_matches"), bExtraEqual);
		if (Spec.bCompile)
		{
			Details->SetStringField(TEXT("recovery_compile_status"), Recovery.Status);
			Details->SetArrayField(TEXT("recovery_diagnostics"), Recovery.Diagnostics);
		}
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
		FCortexCommandResult SaveResult;
		bool bForcedSaveFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
		bForcedSaveFailure = GForceSaveFailureForTest;
#endif
		if (bForcedSaveFailure)
		{
			SaveResult = FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, TEXT("Forced save failure"));
		}
		else
		{
			SaveResult = FCortexBPAssetOps::Save(SaveParams);
		}
		State.SaveStatus = SaveResult.bSuccess ? TEXT("saved") : TEXT("failed");
		State.bSaved = SaveResult.bSuccess;
		State.FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		State.bDirtyAfter = Package->IsDirty();
		if (!SaveResult.bSuccess)
		{
			return FCortexCommandRouter::Error(
				SaveResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : SaveResult.ErrorCode,
				SaveResult.ErrorMessage, ToOutcomeJson(State));
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

void FCortexBPGuardedRun::SetPreRecoveryCompileHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	GPreRecoveryCompileHookForTest = MoveTemp(Hook);
}

void FCortexBPGuardedRun::ClearPreRecoveryCompileHookForTest()
{
	GPreRecoveryCompileHookForTest = nullptr;
}

void FCortexBPGuardedRun::SetSkipUndoForTest(bool bSkip)
{
	GSkipUndoForTest = bSkip;
}

void FCortexBPGuardedRun::SetForceSaveFailureForTest(bool bForce)
{
	GForceSaveFailureForTest = bForce;
}
#endif
