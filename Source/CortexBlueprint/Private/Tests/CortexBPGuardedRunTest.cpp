#include "Misc/AutomationTest.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "CortexAssetMutationGuard.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_VariableGet.h"
#include "ScopedTransaction.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"

namespace
{
	FEdGraphPinType BoolPin()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return PinType;
	}

	/** Counts compiles for the lifetime of the scope. */
	struct FCompileCounter
	{
		int32 Count = 0;
		FCompileCounter()
		{
			FCortexBPCompileSupport::SetCompileObserverForTest([this](UBlueprint*, EBlueprintCompileOptions) { ++Count; });
		}
		~FCompileCounter()
		{
			FCortexBPCompileSupport::ClearCompileObserverForTest();
		}
	};

	struct FPostRestoreHookScope
	{
		explicit FPostRestoreHookScope(TFunction<void(UBlueprint*)> Hook)
		{
			FCortexBPGuardedRun::SetPostRestoreHookForTest(MoveTemp(Hook));
		}
		~FPostRestoreHookScope()
		{
			FCortexBPGuardedRun::ClearPostRestoreHookForTest();
		}
	};

	TSharedPtr<FJsonObject> Snapshot(UBlueprint* Blueprint)
	{
		FString Error;
		return FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
	}

	bool SnapshotsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Result = FCortexBPSnapshotOps::DiffSnapshots(A, B, TSet<FString>(), Differences, Counts);
		return Result.bSuccess && Differences.Num() == 0;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Name, Value);
		}
		return Value;
	}

	bool BoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		bool bValue = false;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Name, bValue);
		}
		return bValue;
	}

	FCortexBPGuardedRun::FSpec AddVariableSpec(UBlueprint* Blueprint, const TCHAR* VariableName)
	{
		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("guarded_test");
		Spec.Blueprint = Blueprint;
		Spec.bCompile = true;
		const FName Name(VariableName);
		Spec.Apply = [Name](UBlueprint* Target, FString&)
		{
			return FBlueprintEditorUtils::AddMemberVariable(Target, Name, BoolPin());
		};
		Spec.Verify = [](UBlueprint*, bool, FString&) { return true; };
		return Spec;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGuardedRunTest,
	"Cortex.Blueprint.GuardedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGuardedRunTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	TArray<UBlueprint*> Created;
	const auto Make = [&Created](const TCHAR* Name)
	{
		UBlueprint* Blueprint = MakeBlueprint(Name, AActor::StaticClass());
		if (Blueprint != nullptr)
		{
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Existing"), BoolPin());
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Created.Add(Blueprint);
		}
		return Blueprint;
	};

	// 1. A failing readback restores a real mutation: snapshot equal, one recovery compile, restored.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedVerifyFail")))
	{
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		FCompileCounter Compiles;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Injected"));
		bool bMutated = false;
		Spec.Apply = [&bMutated](UBlueprint* Target, FString&)
		{
			const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("Injected"), BoolPin());
			bMutated = Target->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V) { return V.VarName == TEXT("Injected"); });
			return bAdded;
		};
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced readback mismatch"); return false; };

		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestTrue(TEXT("The mutation really happened before the failure"), bMutated);
		TestFalse(TEXT("Failing verify fails the run"), Result.bSuccess);
		TestEqual(TEXT("Readback code"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		TestEqual(TEXT("rollback_status"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestEqual(TEXT("readback_status"), StringField(Result.ErrorDetails, TEXT("readback_status")), FString(TEXT("mismatched")));
		TestFalse(TEXT("Not blocked"), BoolField(Result.ErrorDetails, TEXT("blocked")));
		TestEqual(TEXT("One apply compile plus exactly one recovery compile"), Compiles.Count, 2);
		TestEqual(TEXT("Variable list restored"), Blueprint->NewVariables.Num(), 1);
		TestTrue(TEXT("Snapshot after equals snapshot before"), SnapshotsEqual(Before, Snapshot(Blueprint)));
		const TSharedPtr<FJsonObject> FingerprintAfter = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		TestEqual(TEXT("Fingerprint restored"),
			FingerprintAfter->GetStringField(TEXT("migration_state_hash")),
			FingerprintBefore->GetStringField(TEXT("migration_state_hash")));
		FString BlockReason;
		TestFalse(TEXT("Asset not blocked after a verified restore"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
	}

	// 2. A recovery that cannot be verified blocks the asset.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedUnverified")))
	{
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Injected"));
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
		const FPostRestoreHookScope Hook([](UBlueprint* Target)
		{
			FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("CorruptedRestore"), BoolPin());
		});
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestEqual(TEXT("rollback_status unverified"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("unverified")));
		TestTrue(TEXT("blocked flag"), BoolField(Result.ErrorDetails, TEXT("blocked")));
		FString BlockReason;
		TestTrue(TEXT("Asset is blocked"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));

		const FCortexCommandResult Again = FCortexBPGuardedRun::Execute(AddVariableSpec(Blueprint, TEXT("Later")));
		TestFalse(TEXT("A blocked asset refuses further runs"), Again.bSuccess);
		TestEqual(TEXT("Blocked run code"), Again.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// 3. dry_run runs neither callback and does not dirty the package.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedDryRun")))
	{
		Blueprint->GetOutermost()->ClearDirtyFlag();
		const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		FCompileCounter Compiles;
		bool bApplied = false;
		bool bVerified = false;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Never"));
		Spec.bDryRun = true;
		Spec.Apply = [&bApplied](UBlueprint*, FString&) { bApplied = true; return true; };
		Spec.Verify = [&bVerified](UBlueprint*, bool, FString&) { bVerified = true; return true; };

		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestTrue(TEXT("Dry run succeeds"), Result.bSuccess);
		TestFalse(TEXT("Apply not called"), bApplied);
		TestFalse(TEXT("Verify not called"), bVerified);
		TestEqual(TEXT("No compile"), Compiles.Count, 0);
		TestFalse(TEXT("Package not dirtied"), Blueprint->GetOutermost()->IsDirty());
		TestTrue(TEXT("Snapshot unchanged"), SnapshotsEqual(Before, Snapshot(Blueprint)));
		if (TestTrue(TEXT("Outcome data"), Result.Data.IsValid()))
		{
			TestTrue(TEXT("dry_run flag"), BoolField(Result.Data, TEXT("dry_run")));
			TestEqual(TEXT("apply_status"), StringField(Result.Data, TEXT("apply_status")), FString(TEXT("dry_run")));
			TestFalse(TEXT("changed"), BoolField(Result.Data, TEXT("changed")));
			TestEqual(TEXT("Fingerprint unchanged"),
				Result.Data->GetObjectField(TEXT("fingerprint_after"))->GetStringField(TEXT("migration_state_hash")),
				FingerprintBefore->GetStringField(TEXT("migration_state_hash")));
		}
	}

	// 4. compile:false skips the compile and the compiled-state readback.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedNoCompile")))
	{
		FCompileCounter Compiles;
		bool bVerifySawCompiled = true;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("NoCompileVar"));
		Spec.bCompile = false;
		Spec.Verify = [&bVerifySawCompiled](UBlueprint*, bool bCompiled, FString&) { bVerifySawCompiled = bCompiled; return true; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestTrue(TEXT("compile:false succeeds"), Result.bSuccess);
		TestEqual(TEXT("No compile ran"), Compiles.Count, 0);
		TestFalse(TEXT("Verify told the compiled state is not available"), bVerifySawCompiled);
		TestEqual(TEXT("Variable kept"), Blueprint->NewVariables.Num(), 2);
		if (Result.Data.IsValid())
		{
			TestEqual(TEXT("compile_status skipped"), StringField(Result.Data, TEXT("compile_status")), FString(TEXT("skipped")));
			TestEqual(TEXT("readback matched"), StringField(Result.Data, TEXT("readback_status")), FString(TEXT("matched")));
		}
	}

	// 5. Success with compile: exactly one compile, applied, dirty, not saved.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedSuccess")))
	{
		Blueprint->GetOutermost()->ClearDirtyFlag();
		FCompileCounter Compiles;
		bool bVerifySawCompiled = false;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Kept"));
		Spec.Verify = [&bVerifySawCompiled](UBlueprint*, bool bCompiled, FString&) { bVerifySawCompiled = bCompiled; return true; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestTrue(TEXT("Run succeeds"), Result.bSuccess);
		TestEqual(TEXT("One compile"), Compiles.Count, 1);
		TestTrue(TEXT("Verify saw compiled state"), bVerifySawCompiled);
		TestEqual(TEXT("Variable kept"), Blueprint->NewVariables.Num(), 2);
		TestTrue(TEXT("Package dirty after a mutation"), Blueprint->GetOutermost()->IsDirty());
		if (Result.Data.IsValid())
		{
			TestEqual(TEXT("apply_status"), StringField(Result.Data, TEXT("apply_status")), FString(TEXT("applied")));
			TestEqual(TEXT("rollback_status"), StringField(Result.Data, TEXT("rollback_status")), FString(TEXT("not_needed")));
			TestFalse(TEXT("saved"), BoolField(Result.Data, TEXT("saved")));
			TestTrue(TEXT("changed"), BoolField(Result.Data, TEXT("changed")));
		}
	}

	// 6. An Apply that mutates and then fails is restored.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedApplyFail")))
	{
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Half"));
		Spec.Apply = [](UBlueprint* Target, FString& OutError)
		{
			FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("Half"), BoolPin());
			OutError = TEXT("forced apply failure");
			return false;
		};
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Failing apply fails"), Result.bSuccess);
		TestEqual(TEXT("Apply failure code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestEqual(TEXT("Restored"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestEqual(TEXT("Variable removed again"), Blueprint->NewVariables.Num(), 1);
		TestTrue(TEXT("Snapshot restored"), SnapshotsEqual(Before, Snapshot(Blueprint)));
	}

	// 7. A compile failure restores the graph that caused it.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedCompileFail")))
	{
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		const int32 NodesBefore = Blueprint->UbergraphPages[0]->Nodes.Num();
		FCompileCounter Compiles;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Unused"));
		Spec.Apply = [](UBlueprint* Target, FString&)
		{
			UEdGraph* EventGraph = Target->UbergraphPages[0];
			EventGraph->Modify();
			for (int32 Index = 0; Index < 2; ++Index)
			{
				UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
				Event->CreateNewGuid();
				Event->CustomFunctionName = FName("GuardedDuplicate");
				EventGraph->AddNode(Event, false, false);
				Event->AllocateDefaultPins();
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
			return true;
		};
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Compile failure fails the run"), Result.bSuccess);
		TestEqual(TEXT("Compile failure code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		TestEqual(TEXT("Restored"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestEqual(TEXT("Nodes restored"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore);
		TestEqual(TEXT("One apply compile plus one recovery compile"), Compiles.Count, 2);
		TestTrue(TEXT("Snapshot restored"), SnapshotsEqual(Before, Snapshot(Blueprint)));
	}

	// 9. The generic journal alone restores graphs, variables and the parent when the undo is skipped (C3).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedJournal")))
	{
		struct FSkipUndoScope
		{
			FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(true); }
			~FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(false); }
		};
		const FSkipUndoScope SkipUndo;
		UClass* OriginalParent = Blueprint->ParentClass;
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		const int32 NodesBefore = Blueprint->UbergraphPages[0]->Nodes.Num();
		FCompileCounter Compiles;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("JournalVar"));
		Spec.Apply = [](UBlueprint* Target, FString&)
		{
			FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("JournalVar"), BoolPin());
			UEdGraph* EventGraph = Target->UbergraphPages[0];
			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
			Event->CreateNewGuid();
			Event->CustomFunctionName = FName("JournalEvent");
			EventGraph->AddNode(Event, false, false);
			Event->AllocateDefaultPins();
			Target->ParentClass = ACortexBPAdoptOtherActorParent::StaticClass();
			return true;
		};
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced readback mismatch"); return false; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestEqual(TEXT("Journal restored without the undo"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestTrue(TEXT("Parent restored"), Blueprint->ParentClass == OriginalParent);
		TestEqual(TEXT("Variables restored"), Blueprint->NewVariables.Num(), 1);
		TestEqual(TEXT("Graph nodes restored"), Blueprint->UbergraphPages[0]->Nodes.Num(), NodesBefore);
		TestTrue(TEXT("Snapshot restored"), SnapshotsEqual(Before, Snapshot(Blueprint)));
		TestEqual(TEXT("One apply compile plus one recovery compile"), Compiles.Count, 2);
	}

	// 10. Operation-owned state: Capture/Restore/DigestExtra; a missing Restore is detected and blocks.
	for (const bool bProvideRestore : {true, false})
	{
		if (UBlueprint* Blueprint = Make(bProvideRestore ? TEXT("BP_GuardedOwnedOk") : TEXT("BP_GuardedOwnedLost")))
		{
			int32 External = 1;
			int32 Captured = 0;
			FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Owned"));
			Spec.Capture = [&External, &Captured](UBlueprint*, FString&) { Captured = External; return true; };
			Spec.Apply = [&External](UBlueprint* Target, FString&)
			{
				External = 2;
				return FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("Owned"), BoolPin());
			};
			Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
			Spec.DigestExtra = [&External](UBlueprint*) { return FString::FromInt(External); };
			if (bProvideRestore)
			{
				Spec.Restore = [&External, &Captured](UBlueprint*, FString&) { External = Captured; return true; };
			}
			const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
			TestFalse(TEXT("Run fails"), Result.bSuccess);
			TestEqual(TEXT("Operation-owned state verdict"),
				StringField(Result.ErrorDetails, TEXT("rollback_status")),
				FString(bProvideRestore ? TEXT("restored") : TEXT("unverified")));
			TestEqual(TEXT("External state back only with a Restore"), External, bProvideRestore ? 1 : 2);
		}
	}

	// 11. Undo is detected by transaction id, not by undo-list growth: redo history must not suppress it (I1).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedRedoHistory")))
	{
		{
			const FScopedTransaction UserEdit(FText::FromString(TEXT("user edit")));
			Blueprint->Modify();
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UserVar"), BoolPin());
		}
		TestTrue(TEXT("User undo leaves redo history"), GEditor != nullptr && GEditor->UndoTransaction(/*bCanRedo=*/true));
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		const int32 ScsNodesBefore = Blueprint->SimpleConstructionScript->GetAllNodes().Num();
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Unused"));
		Spec.Apply = [](UBlueprint* Target, FString&)
		{
			USimpleConstructionScript* SCS = Target->SimpleConstructionScript;
			SCS->Modify();
			SCS->AddNode(SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("RedoMesh")));
			return true;
		};
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestEqual(TEXT("A structural SCS change is undone despite redo history"),
			StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestEqual(TEXT("SCS node removed again"), Blueprint->SimpleConstructionScript->GetAllNodes().Num(), ScsNodesBefore);
		TestTrue(TEXT("Snapshot restored"), SnapshotsEqual(Before, Snapshot(Blueprint)));
	}

	// 12. A template property that was at its class default is put back by the journal (I2).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedTemplateDefault")))
	{
		struct FSkipUndoScope
		{
			FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(true); }
			~FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(false); }
		};
		USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("TplMesh"));
		Blueprint->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		USceneComponent* Template = CastChecked<USceneComponent>(Node->ComponentTemplate);
		TestFalse(TEXT("Template starts at the class default"), Template->bHiddenInGame);
		const FSkipUndoScope SkipUndo;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Unused"));
		Spec.Apply = [Template](UBlueprint*, FString&) { Template->Modify(); Template->bHiddenInGame = true; return true; };
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestFalse(TEXT("Template property reset to the class default"), Template->bHiddenInGame);
		TestEqual(TEXT("Journal restored the template"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
	}

	// 13. Interface implementation graphs are journaled (I3).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedInterface")))
	{
		struct FSkipUndoScope
		{
			FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(true); }
			~FSkipUndoScope() { FCortexBPGuardedRun::SetSkipUndoForTest(false); }
		};
		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(UCortexBPAdoptInterface::StaticClass()));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (TestTrue(TEXT("Interface implementation has a graph"),
			Blueprint->ImplementedInterfaces.Num() == 1 && Blueprint->ImplementedInterfaces[0].Graphs.Num() > 0))
		{
			UEdGraph* InterfaceGraph = Blueprint->ImplementedInterfaces[0].Graphs[0];
			const int32 NodesBefore = InterfaceGraph->Nodes.Num();
			const FSkipUndoScope SkipUndo;
			FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Unused"));
			Spec.Apply = [](UBlueprint* Target, FString&)
			{
				UEdGraph* Graph = Target->ImplementedInterfaces[0].Graphs[0];
				Graph->Modify();
				FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
				UK2Node_VariableGet* Get = Creator.CreateNode();
				Get->VariableReference.SetSelfMember(TEXT("Existing"));
				Creator.Finalize();
				return true;
			};
			Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
			const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
			TestFalse(TEXT("Run fails"), Result.bSuccess);
			TestEqual(TEXT("Interface graph restored by the journal"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
			TestEqual(TEXT("Interface graph node count restored"), Blueprint->ImplementedInterfaces[0].Graphs[0]->Nodes.Num(), NodesBefore);
		}
	}

	// 14. A failing recovery compile alone (content fully restored) is not "restored" and is reported (I4).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedRecoveryCompile")))
	{
		struct FOverrideScope
		{
			FOverrideScope()
			{
				// Compile 1 is the apply compile; compile 2 is the single recovery compile.
				FCortexBPCompileSupport::SetCompileOutcomeOverrideForTest([](int32 Index, FCortexBPCompileOutcome& Outcome)
				{
					if (Index == 2)
					{
						Outcome.ErrorCount = 1;
						Outcome.Status = TEXT("error");
					}
				});
			}
			~FOverrideScope() { FCortexBPCompileSupport::ClearCompileOutcomeOverrideForTest(); }
		};
		const FOverrideScope Scope;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Unused"));
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestEqual(TEXT("Recovery compile status is reported"), StringField(Result.ErrorDetails, TEXT("recovery_compile_status")), FString(TEXT("error")));
		TestTrue(TEXT("Content was fully restored"),
			BoolField(Result.ErrorDetails, TEXT("rollback_snapshot_matches")) && BoolField(Result.ErrorDetails, TEXT("rollback_graphs_match"))
			&& BoolField(Result.ErrorDetails, TEXT("rollback_fingerprint_matches")) && BoolField(Result.ErrorDetails, TEXT("rollback_content_restored")));
		TestEqual(TEXT("Only the recovery compile fails the verdict"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("unverified")));
		TestTrue(TEXT("Recovery diagnostics are emitted"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("recovery_diagnostics")));
	}

	// 15. Nested in an outer transaction (router batch): no undo is attempted, the journal restores, nothing is blocked (N1).
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedNested")))
	{
		const TSharedPtr<FJsonObject> Before = Snapshot(Blueprint);
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Nested"));
		Spec.Verify = [](UBlueprint*, bool, FString& OutError) { OutError = TEXT("forced"); return false; };
		FCortexCommandResult Result;
		{
			const FScopedTransaction Outer(FText::FromString(TEXT("outer batch")));
			Result = FCortexBPGuardedRun::Execute(Spec);
		}
		TestFalse(TEXT("Run fails"), Result.bSuccess);
		TestEqual(TEXT("Nested failure is restored from the journal"), StringField(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("Not blocked"), BoolField(Result.ErrorDetails, TEXT("blocked")));
		FString BlockReason;
		TestFalse(TEXT("Asset not blocked"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
		TestEqual(TEXT("Variable removed again"), Blueprint->NewVariables.Num(), 1);
		TestTrue(TEXT("Snapshot restored"), SnapshotsEqual(Before, Snapshot(Blueprint)));
	}

	// 8. Stale expected_fingerprint refuses before any mutation.
	if (UBlueprint* Blueprint = Make(TEXT("BP_GuardedStale")))
	{
		bool bApplied = false;
		FCortexBPGuardedRun::FSpec Spec = AddVariableSpec(Blueprint, TEXT("Stale"));
		Spec.Expected = MakeShared<FJsonObject>();
		Spec.Expected->SetStringField(TEXT("migration_state_hash"), TEXT("0000"));
		Spec.Apply = [&bApplied](UBlueprint*, FString&) { bApplied = true; return true; };
		const FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		TestFalse(TEXT("Stale fingerprint refuses"), Result.bSuccess);
		TestEqual(TEXT("Stale code"), Result.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestFalse(TEXT("Nothing applied"), bApplied);
	}

	for (UBlueprint* Blueprint : Created)
	{
		Discard(Blueprint);
	}
	return true;
}
