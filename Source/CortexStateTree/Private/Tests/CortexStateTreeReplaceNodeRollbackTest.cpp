#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Operations/CortexSTReplaceNodeOps.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
/** The content that must survive a rollback: every node report and the whole-tree binding count (the payload's fingerprint is not content). */
TSharedPtr<FJsonObject> MigRollbackContent(const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	if (Data.IsValid())
	{
		Content->SetField(TEXT("nodes"), Data->TryGetField(TEXT("nodes")));
		Content->SetField(TEXT("whole_tree_binding_count"), Data->TryGetField(TEXT("whole_tree_binding_count")));
	}
	return Content;
}

TSharedPtr<FJsonObject> MigRollbackSnapshot(CortexSTTest::FMigrationFixture& Fixture)
{
	const FCortexCommandResult Result = Fixture.Run(TEXT("get_node_bindings"), Fixture.BaseParams());
	return MigRollbackContent(Result.Data);
}

bool MigRollbackContentEquals(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
{
	return A.IsValid() && B.IsValid()
		&& FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(A), *MakeShared<FJsonValueObject>(B));
}

TSharedPtr<FJsonObject> MigRollbackParams(CortexSTTest::FMigrationFixture& Fixture, const bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestNativeTask::StaticStruct()->GetPathName());
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	Map->SetStringField(TEXT("Count"), TEXT("Count"));
	Map->SetStringField(TEXT("ResultOut"), TEXT("ResultOut"));
	Params->SetObjectField(TEXT("property_map"), Map);
	if (bSave)
	{
		Params->SetBoolField(TEXT("save"), true);
	}
	return Params;
}

FCortexCommandResult MigRollbackRunWithFault(
	CortexSTTest::FMigrationFixture& Fixture,
	const std::initializer_list<FCortexSTReplaceNodeOps::EFaultPoint> Points,
	const TSharedPtr<FJsonObject>& Params)
{
	for (const FCortexSTReplaceNodeOps::EFaultPoint Point : Points)
	{
		FCortexSTReplaceNodeOps::SetFaultPointForTesting(Point);
	}
	const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
	FCortexSTReplaceNodeOps::ClearFaultPointForTesting();
	return Result;
}

bool MigRollbackIsBlueprintNode(CortexSTTest::FMigrationFixture& Fixture)
{
	FStateTreeEditorNode* Node = Fixture.NodePtr(Fixture.BpTaskId);
	return Node != nullptr && Node->InstanceObject != nullptr
		&& Node->Node.GetScriptStruct() != nullptr
		&& Node->Node.GetScriptStruct()->GetName() == TEXT("StateTreeBlueprintTaskWrapper");
}

int32 MigRollbackUndoLength()
{
	return GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeRollbackTest,
	"Cortex.StateTree.ReplaceNode.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeRollbackTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return true;
#else
	// ── (a) A compile failure after the apply: the journal is restored and the content verified ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		// A task-completion binding is keyed by the node-struct ID (GetNodeID, derived from the editor ID) and must survive both the
		// swap and a recovery; a non-default Label proves the journal restores values, not only bindings.
		FStateTreeEditorNode* BpNode = Fixture.NodePtr(Fixture.BpTaskId);
		UStateTreeState* StateA = Fixture.StateA();
		UStateTreeState* StateB = Fixture.StateB();
		if (!TestNotNull(TEXT("the rollback node exists"), BpNode) || !TestNotNull(TEXT("state A exists"), StateA)
			|| !TestNotNull(TEXT("state B exists"), StateB)
			|| !TestTrue(TEXT("the rollback Label is set"), CortexSTTest::SetInstanceValueText(*BpNode, TEXT("Label"), TEXT("rollback"))))
		{
			return false;
		}
		FStateTreeTransition& DelegateTransition = CortexSTCompat::AddTransition(
			*StateA, EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::GotoState, StateB);
		Fixture.Context.EditorData->EditorBindings.AddTaskCompletionBinding(
			BpNode->GetNodeID(),
			FPropertyBindingPath(DelegateTransition.ID, FName(TEXT("DelegateListener"))),
			UE::StateTree::ETaskCompletionCondition::Succeeds);
		Fixture.Context.StateTree->MarkPackageDirty();
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		Package->SetDirtyFlag(false);
		const TSharedPtr<FJsonObject> Before = MigRollbackSnapshot(Fixture);
		TestEqual(TEXT("the completion binding is stored"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);

		const FCortexCommandResult Result = MigRollbackRunWithFault(Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Compile }, MigRollbackParams(Fixture));
		TestFalse(TEXT("an injected compile failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("an injected compile failure code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		if (TestNotNull(TEXT("the failure carries details"), Result.ErrorDetails.Get()))
		{
			TestTrue(TEXT("the content is restored"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
			TestEqual(TEXT("one recovery compile ran"), Result.ErrorDetails->GetStringField(TEXT("recovery_compile")), FString(TEXT("succeeded")));
			TestEqual(TEXT("the rollback status is restored"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
			TestFalse(TEXT("the asset is not blocked"), Result.ErrorDetails->GetBoolField(TEXT("asset_blocked")));
		}
		TestFalse(TEXT("the dirty flag equals the pre-call flag"), Package->IsDirty());
		TestTrue(TEXT("the whole-tree binding report equals the pre-call report"), MigRollbackContentEquals(Before, MigRollbackSnapshot(Fixture)));
		TestTrue(TEXT("the Blueprint node is back"), MigRollbackIsBlueprintNode(Fixture));
		TestEqual(TEXT("the binding count is unchanged by the rollback"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);
		// Values, not only bindings: the journaled Label comes back with its content.
		FStateTreeEditorNode* RestoredNode = Fixture.NodePtr(Fixture.BpTaskId);
		TestEqual(TEXT("the journaled Label value is restored"), RestoredNode != nullptr
			? CortexSTTest::GetInstanceValueText(*RestoredNode, TEXT("Label")) : FString(), FString(TEXT("rollback")));
		// Completion bindings survive the swap: the same GetNodeID source resolves on the replacement node type.
		const FCortexCommandResult Applied = Fixture.Run(TEXT("replace_node"), MigRollbackParams(Fixture));
		if (TestTrue(TEXT("the retried replacement succeeds"), Applied.bSuccess))
		{
			TestEqual(TEXT("the completion binding survives the swap"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);
			TSharedPtr<FJsonObject> Read = Fixture.BaseParams();
			Read->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
			const FCortexCommandResult NodeRead = Fixture.Run(TEXT("get_node_bindings"), Read);
			bool bCompletion = false;
			const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
			if (NodeRead.bSuccess && NodeRead.Data->TryGetArrayField(TEXT("nodes"), Sources) && Sources != nullptr && Sources->Num() == 1)
			{
				const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
				if (Sources->Num() > 0 && (*Sources)[0]->AsObject()->TryGetArrayField(TEXT("as_source"), Entries) && Entries != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Value : *Entries)
					{
						const TSharedPtr<FJsonObject> Entry = Value->AsObject();
						bCompletion = bCompletion || Entry->HasField(TEXT("completion_condition"));
					}
				}
			}
			TestTrue(TEXT("the completion binding is still listed on the replaced node"), bCompletion);
		}
	}

	// ── (a2) The same failure with the journal restore skipped: Cancel() alone restores nothing, so the comparison fails ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		Package->SetDirtyFlag(false);
		const TSharedPtr<FJsonObject> Before = MigRollbackSnapshot(Fixture);

		const FCortexCommandResult Result = MigRollbackRunWithFault(Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Compile, FCortexSTReplaceNodeOps::EFaultPoint::SkipJournalRestore }, MigRollbackParams(Fixture));
		TestFalse(TEXT("a skipped journal restore fails the call"), Result.bSuccess);
		TestEqual(TEXT("a skipped journal restore is a verification failure"), Result.ErrorCode, CortexErrorCodes::VerificationFailed);
		if (TestNotNull(TEXT("the skipped-restore failure carries details"), Result.ErrorDetails.Get()))
		{
			TestFalse(TEXT("a skipped journal restore leaves the content unrestored"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
			TestTrue(TEXT("a skipped journal restore blocks the asset"), Result.ErrorDetails->GetBoolField(TEXT("asset_blocked")));
		}
		TestFalse(TEXT("the skipped-restore content differs from the pre-call content"), MigRollbackContentEquals(Before, MigRollbackSnapshot(Fixture)));
	}

	// ── (b) An injected readback mismatch takes the same route ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		Package->SetDirtyFlag(false);
		const TSharedPtr<FJsonObject> Before = MigRollbackSnapshot(Fixture);

		const FCortexCommandResult Result = MigRollbackRunWithFault(Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Readback }, MigRollbackParams(Fixture));
		TestFalse(TEXT("an injected readback mismatch fails the call"), Result.bSuccess);
		TestEqual(TEXT("an injected readback mismatch code"), Result.ErrorCode, CortexErrorCodes::VerificationFailed);
		if (TestNotNull(TEXT("the readback failure carries details"), Result.ErrorDetails.Get()))
		{
			TestTrue(TEXT("the readback failure restores the content"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
			TestEqual(TEXT("the readback failure runs one recovery compile"), Result.ErrorDetails->GetStringField(TEXT("recovery_compile")), FString(TEXT("succeeded")));
			TestEqual(TEXT("the readback phase is named"), Result.ErrorDetails->GetStringField(TEXT("failed_phase")), FString(TEXT("readback")));
		}
		TestFalse(TEXT("the readback failure restores the dirty flag"), Package->IsDirty());
		TestTrue(TEXT("the readback failure restores the content by comparison"), MigRollbackContentEquals(Before, MigRollbackSnapshot(Fixture)));
		TestTrue(TEXT("the Blueprint node is back after the readback failure"), MigRollbackIsBlueprintNode(Fixture));
	}

	// ── (c) An injected restoration failure blocks the asset, and later mutations refuse ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		const FCortexCommandResult Result = MigRollbackRunWithFault(Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Compile, FCortexSTReplaceNodeOps::EFaultPoint::AfterRestore }, MigRollbackParams(Fixture));
		TestFalse(TEXT("an injected restoration failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("an injected restoration failure code"), Result.ErrorCode, CortexErrorCodes::VerificationFailed);
		if (TestNotNull(TEXT("the restoration failure carries details"), Result.ErrorDetails.Get()))
		{
			TestFalse(TEXT("the restoration failure is not content-restored"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
			TestTrue(TEXT("the restoration failure blocks the asset"), Result.ErrorDetails->GetBoolField(TEXT("asset_blocked")));
		}
		const FCortexCommandResult Later = Fixture.Run(TEXT("replace_node"), MigRollbackParams(Fixture));
		TestFalse(TEXT("a later mutation of a blocked asset refuses"), Later.bSuccess);
		TestEqual(TEXT("a later mutation of a blocked asset code"), Later.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("the refusal names the block"), Later.ErrorMessage.Contains(TEXT("blocked")));

		// F5: the quarantine is shared, so a different command cannot write or persist the unverified asset either.
		TSharedPtr<FJsonObject> Rename = Fixture.MutationParams();
		Rename->SetStringField(TEXT("name"), TEXT("Renamed"));
		Rename->SetStringField(TEXT("state_path"), TEXT("A"));
		const FCortexCommandResult RenameResult = Fixture.Run(TEXT("rename_state"), Rename);
		TestFalse(TEXT("rename_state on a blocked asset refuses"), RenameResult.bSuccess);
		TestEqual(TEXT("rename_state on a blocked asset code"), RenameResult.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("rename_state names the block"), RenameResult.ErrorMessage.Contains(TEXT("blocked")));

		TSharedPtr<FJsonObject> Compile = Fixture.MutationParams();
		Compile->SetBoolField(TEXT("save"), true);
		const FCortexCommandResult CompileResult = Fixture.Run(TEXT("compile"), Compile);
		TestFalse(TEXT("compile with save on a blocked asset refuses"), CompileResult.bSuccess);
		TestEqual(TEXT("compile with save on a blocked asset code"), CompileResult.ErrorCode, CortexErrorCodes::InvalidOperation);

		TSharedPtr<FJsonObject> Save = Fixture.MutationParams();
		const FCortexCommandResult SaveResult = Fixture.Run(TEXT("validate_asset"), Save);
		TestEqual(TEXT("validate_asset on a blocked asset refuses"), SaveResult.ErrorCode, CortexErrorCodes::InvalidOperation);

		TSharedPtr<FJsonObject> Delete = Fixture.BaseParams();
		const FCortexCommandResult DeleteResult = Fixture.Run(TEXT("delete_asset"), Delete);
		TestFalse(TEXT("delete_asset on a blocked asset refuses"), DeleteResult.bSuccess);
		TestEqual(TEXT("delete_asset on a blocked asset code"), DeleteResult.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// ── (d) A stale fingerprint: the asset changed between the plan and the apply ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Params = MigRollbackParams(Fixture);
		const int32 UndoBefore = MigRollbackUndoLength();
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		const bool bDirtyBefore = Package->IsDirty();
		const TSharedPtr<FJsonObject> FingerprintBefore = CortexST::MakeFingerprint(Fixture.Context.StateTree);
		// A content edit while the asset is already dirty: the editor-content signature in the fingerprint has to see it.
		Fixture.StateA()->Name = TEXT("A_Renamed");
		const TSharedPtr<FJsonObject> FingerprintAfter = CortexST::MakeFingerprint(Fixture.Context.StateTree);
		double HashBefore = 0.0;
		double HashAfter = 0.0;
		TestTrue(TEXT("F4: the fingerprint carries an editor-content hash"),
			FingerprintBefore->TryGetNumberField(TEXT("editor_content_hash"), HashBefore));
		TestTrue(TEXT("F4: a nested content edit changes the editor-content hash"),
			FingerprintAfter->TryGetNumberField(TEXT("editor_content_hash"), HashAfter) && HashAfter != HashBefore);
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), Params);
		TestFalse(TEXT("a stale fingerprint refuses"), Result.bSuccess);
		TestEqual(TEXT("a stale fingerprint code"), Result.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestEqual(TEXT("a stale fingerprint opens no transaction"), MigRollbackUndoLength(), UndoBefore);
		TestTrue(TEXT("a stale fingerprint leaves the node alone"), MigRollbackIsBlueprintNode(Fixture));
		TestEqual(TEXT("a stale fingerprint leaves the dirty flag alone"), Package->IsDirty(), bDirtyBefore);
		TestEqual(TEXT("a stale fingerprint leaves every binding alone"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 2);
	}

	// ── (e) A binding that validation would silently remove is refused during planning, never during apply ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		// A binding to a struct that is not part of the tree: RemoveInvalidBindings() deletes it before the compile.
		Fixture.Context.EditorData->AddPropertyBinding(
			FPropertyBindingPath(Fixture.NativeAId, FName(TEXT("ResultOut"))),
			FPropertyBindingPath(FGuid::NewGuid(), FName(TEXT("Nope"))));
		TestEqual(TEXT("the broken binding is stored"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);

		TSharedPtr<FJsonObject> DryRun = MigRollbackParams(Fixture);
		DryRun->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Planned = Fixture.Run(TEXT("replace_node"), DryRun);
		TestFalse(TEXT("a binding validation would drop is refused at plan time"), Planned.bSuccess);
		TestEqual(TEXT("a validation drop is a verification failure"), Planned.ErrorCode, CortexErrorCodes::VerificationFailed);
		const TArray<TSharedPtr<FJsonValue>>* Unresolved = nullptr;
		if (TestNotNull(TEXT("the refusal carries details"), Planned.ErrorDetails.Get())
			&& TestTrue(TEXT("the refusal lists the unresolved bindings"),
				Planned.ErrorDetails->TryGetArrayField(TEXT("unresolved_bindings"), Unresolved) && Unresolved != nullptr))
		{
			if (TestEqual(TEXT("exactly one binding is refused"), Unresolved->Num(), 1))
			{
				const TSharedPtr<FJsonObject> Entry = (*Unresolved)[0]->AsObject();
				TestTrue(TEXT("the refused binding has no owning replacement node"), Entry->GetField<EJson::None>(TEXT("node_id"))->IsNull());
				TestEqual(TEXT("the refused binding is the broken one"), static_cast<int32>(Entry->GetNumberField(TEXT("binding_index"))), 2);
				TestTrue(TEXT("the reason names the validation rule"), Entry->GetStringField(TEXT("reason")).Contains(TEXT("validation would remove")));
			}
		}
		TestEqual(TEXT("nothing was dropped by the refusal"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);
		TestTrue(TEXT("the plan refusal leaves the node alone"), MigRollbackIsBlueprintNode(Fixture));
	}

	// ── (f) save:true saves only on full success; a failed run never saves ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		// The asset exists on disk from creation, so the apply's save is a modification of a known file, not the first write of a
		// package the asset registry still tracks as empty.
		if (!Fixture.Build(*this, /*bCompile=*/true, {}, /*bCreateBindings=*/true, /*bSaveAtCreate=*/true))
		{
			return false;
		}
		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigRollbackParams(Fixture, /*bSave=*/true));
		if (!TestTrue(TEXT("a saved replacement succeeds"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestTrue(TEXT("the successful run reports saved"), Result.Data->GetBoolField(TEXT("saved")));
		TestNotNull(TEXT("the successful run returns a fingerprint field"), Result.Data->TryGetField(TEXT("fingerprint")).Get());
		TestFalse(TEXT("saving clears the dirty flag"), Fixture.Context.StateTree->GetOutermost()->IsDirty());
		// Leave nothing behind. Deleting the asset through the registry records it as an empty package, and the registry's still
		// pending scan batch for the file it just wrote then logs "marked as deleted in editor, but has been modified on disk"
		// (AssetRegistry.cpp:4418, 8503). Removing the bytes and retiring the objects without that notification is quiet.
		FString SavedFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(
			FPackageName::ObjectPathToPackageName(Fixture.AssetPath), SavedFilename, FPackageName::GetAssetPackageExtension()))
		{
			IFileManager::Get().Delete(*SavedFilename, false, true, true);
			UStateTree* Retired = Fixture.Context.StateTree;
			Retired->ClearFlags(RF_Public | RF_Standalone);
			Retired->MarkAsGarbage();
			Retired->GetOutermost()->MarkAsGarbage();
			Fixture.bAssetRetiredByTest = true;
		}
	}
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		Package->SetDirtyFlag(true);
		const FCortexCommandResult Result = MigRollbackRunWithFault(Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Compile }, MigRollbackParams(Fixture, /*bSave=*/true));
		TestFalse(TEXT("a failed run with save:true fails"), Result.bSuccess);
		TestEqual(TEXT("a failed run with save:true code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		if (TestNotNull(TEXT("the failed save run carries details"), Result.ErrorDetails.Get()))
		{
			TestTrue(TEXT("the failed save run restores the content"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
		}
		TestTrue(TEXT("a failed run never saves: the dirty flag is untouched"), Package->IsDirty());
	}

	// ── (g) A real compile failure (an inaccessible binding source) cannot be recovered, so the asset is blocked ──
	{
		// Two compiles fail on purpose here (the apply compile and the recovery compile); their engine errors are expected.
		AddExpectedError(TEXT("Failed to compile"), EAutomationExpectedErrorFlags::Contains, 2);
		AddExpectedError(TEXT("Failed to find binding source property"), EAutomationExpectedErrorFlags::Contains, 2);
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/false))
		{
			return false;
		}
		// Valid structs and paths, but B's instance data cannot feed A's: the compiler refuses it, and so does the recovery compile.
		Fixture.Context.EditorData->AddPropertyBinding(
			FPropertyBindingPath(Fixture.NativeBId, FName(TEXT("Count"))),
			FPropertyBindingPath(Fixture.NativeAId, FName(TEXT("Count"))));
		Fixture.Context.StateTree->MarkPackageDirty();

		const FCortexCommandResult Result = Fixture.Run(TEXT("replace_node"), MigRollbackParams(Fixture));
		TestFalse(TEXT("F6: a replacement whose compile fails refuses"), Result.bSuccess);
		// The recovery compile fails too, so the asset is quarantined and the verdict is the unverified-recovery code.
		TestEqual(TEXT("F6: an unrecoverable compile failure is a verification failure"), Result.ErrorCode, CortexErrorCodes::VerificationFailed);
		if (TestNotNull(TEXT("F6: the failure carries details"), Result.ErrorDetails.Get()))
		{
			TestEqual(TEXT("F6: the recovery compile failed"), Result.ErrorDetails->GetStringField(TEXT("recovery_compile")), FString(TEXT("failed")));
			TestEqual(TEXT("F6: the compile phase is named"), Result.ErrorDetails->GetStringField(TEXT("failed_phase")), FString(TEXT("compile")));
			TestFalse(TEXT("F6: an unverified recovery is not content-restored"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
			TestTrue(TEXT("F6: an unverified recovery blocks the asset"), Result.ErrorDetails->GetBoolField(TEXT("asset_blocked")));
		}
		TestFalse(TEXT("F6: the tree is not runnable after the failed recovery"), Fixture.Context.StateTree->IsReadyToRun());
		const FCortexCommandResult Later = Fixture.Run(TEXT("compile"), Fixture.MutationParams());
		TestEqual(TEXT("F6: the blocked asset cannot be compiled afterwards"), Later.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// ── (h) An injected save failure takes the rollback route and saves nothing ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		UPackage* Package = Fixture.Context.StateTree->GetOutermost();
		Package->SetDirtyFlag(true);
		const TSharedPtr<FJsonObject> Before = MigRollbackSnapshot(Fixture);
		const FCortexCommandResult Result = MigRollbackRunWithFault(
			Fixture, { FCortexSTReplaceNodeOps::EFaultPoint::Save }, MigRollbackParams(Fixture, /*bSave=*/true));
		TestFalse(TEXT("an injected save failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("an injected save failure code"), Result.ErrorCode, CortexErrorCodes::SaveFailed);
		if (TestNotNull(TEXT("the save failure carries details"), Result.ErrorDetails.Get()))
		{
			TestEqual(TEXT("the save phase is named"), Result.ErrorDetails->GetStringField(TEXT("failed_phase")), FString(TEXT("save")));
			TestTrue(TEXT("the save failure restores the content"), Result.ErrorDetails->GetBoolField(TEXT("content_restored")));
		}
		TestTrue(TEXT("the failed save leaves the dirty flag set"), Package->IsDirty());
		TestTrue(TEXT("the failed save restores the content by comparison"), MigRollbackContentEquals(Before, MigRollbackSnapshot(Fixture)));
	}

	// ── (i) The binding journal restores the whole array, including after the count changed ──
	{
		CortexSTTest::FMigrationFixture Fixture;
		if (!Fixture.Build(*this, /*bCompile=*/true))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Before = MigRollbackSnapshot(Fixture);
		CortexSTCompat::FEditorBindingsJournal Journal;
		CortexSTCompat::CaptureEditorBindings(*Fixture.Context.EditorData, Journal);
		// Simulate a compiler fixup that deleted or added entries: the restore must rebuild, not refuse on a size change.
		Fixture.Context.EditorData->AddPropertyBinding(
			FPropertyBindingPath(Fixture.NativeAId, FName(TEXT("ResultOut"))),
			FPropertyBindingPath(Fixture.NativeBId, FName(TEXT("ResultOut"))));
		TestEqual(TEXT("the extra binding changes the count"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 3);
		TestTrue(TEXT("a size-changing journal restore succeeds"), CortexSTCompat::RestoreEditorBindings(*Fixture.Context.EditorData, Journal));
		TestEqual(TEXT("the restored array has the journaled count"), CortexSTCompat::GetEditorBindingCount(*Fixture.Context.EditorData), 2);
		TestTrue(TEXT("the rebuilt array equals the pre-change content"), MigRollbackContentEquals(Before, MigRollbackSnapshot(Fixture)));
	}

	return true;
#endif
}
