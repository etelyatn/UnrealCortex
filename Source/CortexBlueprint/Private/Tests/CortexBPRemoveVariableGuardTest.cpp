#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexBPRenameMemberTestSupport.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "Operations/CortexBPStructureOps.h"
#include "UObject/Package.h"

namespace
{
	TSharedPtr<FJsonObject> RemoveVariableParams(const UBlueprint* Blueprint, const TCHAR* Name)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("name"), Name);
		return Params;
	}

	UK2Node_VariableSet* AddSetNode(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode();
		Node->VariableReference.SetSelfMember(FName(Name));
		Creator.Finalize();
		return Node;
	}

	int32 CountArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? Values->Num() : 0;
	}

	FString JsonToString(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
		return Out;
	}

	/** The object in Field's array whose `variable` field equals Variable; null when absent. */
	TSharedPtr<FJsonObject> FindReferencingNode(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& NodeGuid)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString ItemGuid;
				if (Value->TryGetObject(Item) && Item != nullptr
					&& (*Item)->TryGetStringField(TEXT("node_guid"), ItemGuid) && ItemGuid == NodeGuid)
				{
					return *Item;
				}
			}
		}
		return nullptr;
	}

	bool FingerprintsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A.IsValid() && B.IsValid()
			&& FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(A), *MakeShared<FJsonValueObject>(B));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveVariableGuardTest,
	"Cortex.Blueprint.Cleanup.GuardedRemoveVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveVariableGuardTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using CortexAdoptMemberTest::HasNodeGuid;
	using CortexRenameTest::RetireBlueprint;

	// --- Unreferenced variable removes as before, without a compile by default ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemovePlain"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("TempValue"), DoublePin(), TEXT("2.5"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		int32 CompileCount = 0;
		FCortexBPCompileSupport::SetCompileObserverForTest([&CompileCount](UBlueprint*, EBlueprintCompileOptions)
		{
			++CompileCount;
		});
		const FCortexCommandResult Result = FCortexBPStructureOps::RemoveVariable(
			RemoveVariableParams(Blueprint, TEXT("TempValue")));
		FCortexBPCompileSupport::ClearCompileObserverForTest();

		TestTrue(TEXT("Unreferenced removal succeeds"), Result.bSuccess);
		if (Result.bSuccess)
		{
			TestTrue(TEXT("removed is true"), Result.Data->GetBoolField(TEXT("removed")));
			TestTrue(TEXT("changed is true"), Result.Data->GetBoolField(TEXT("changed")));
			TestEqual(TEXT("No compile by default"), Result.Data->GetStringField(TEXT("compile_status")), FString(TEXT("skipped")));
			TestEqual(TEXT("Readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
			TestEqual(TEXT("No node removed"), CountArray(Result.Data, TEXT("removed_nodes")), 0);
			TestEqual(TEXT("No SaveGame finding"), CountArray(Result.Data, TEXT("save_game_findings")), 0);
			TestEqual(TEXT("No acknowledgment required"), CountArray(Result.Data, TEXT("required_acknowledgment")), 0);
		}
		TestEqual(TEXT("Default removal compiles nothing"), CompileCount, 0);
		TestFalse(TEXT("Variable is gone"), HasVariable(Blueprint, TEXT("TempValue")));
		RetireBlueprint(Blueprint);
	}

	// --- A referenced variable refuses MEMBER_REFERENCED and changes nothing ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemoveReferenced"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Referenced Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("bOpen"), BoolPin());
		UK2Node_VariableGet* Get = AddVariableGet(Blueprint, TEXT("bOpen"));
		UK2Node_VariableSet* Set = AddSetNode(Blueprint, TEXT("bOpen"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestNotNull(TEXT("Get node"), Get) || !TestNotNull(TEXT("Set node"), Set))
		{
			RetireBlueprint(Blueprint);
			return false;
		}
		const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);

		const FCortexCommandResult Refused = FCortexBPStructureOps::RemoveVariable(
			RemoveVariableParams(Blueprint, TEXT("bOpen")));
		TestFalse(TEXT("Referenced variable refuses"), Refused.bSuccess);
		TestEqual(TEXT("Reference refusal code"), Refused.ErrorCode, CortexErrorCodes::MemberReferenced);
		if (TestTrue(TEXT("Refusal carries the reference list"), Refused.ErrorDetails.IsValid()))
		{
			TestEqual(TEXT("Both nodes listed"), CountArray(Refused.ErrorDetails, TEXT("referencing_nodes")), 2);
			const TSharedPtr<FJsonObject> GetEntry = FindReferencingNode(
				Refused.ErrorDetails, TEXT("referencing_nodes"), Get->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			if (TestTrue(TEXT("Get node is listed"), GetEntry.IsValid()))
			{
				TestEqual(TEXT("Graph name"), GetEntry->GetStringField(TEXT("graph_name")), FString(TEXT("EventGraph")));
				TestEqual(TEXT("Member variable kind"), GetEntry->GetStringField(TEXT("kind")), FString(TEXT("k2_variable_node")));
				TestTrue(TEXT("Self context"), GetEntry->GetBoolField(TEXT("self_context")));
				TestFalse(TEXT("Node class reported"), GetEntry->GetStringField(TEXT("node_class")).IsEmpty());
				TestEqual(TEXT("Node name"), GetEntry->GetStringField(TEXT("node_name")), Get->GetName());
			}
		}
		TestTrue(TEXT("Refused variable is untouched"), HasVariable(Blueprint, TEXT("bOpen")));
		TestTrue(TEXT("Get node survives the refusal"), HasNodeGuid(Blueprint, Get->NodeGuid));
		TestTrue(TEXT("Set node survives the refusal"), HasNodeGuid(Blueprint, Set->NodeGuid));
		TestTrue(TEXT("Refusal changes nothing"),
			FingerprintsEqual(FingerprintBefore, FCortexBPMigrationGuard::ComputeFingerprint(Blueprint)));

		// --- dry_run lists the nodes and the package stays exactly as it was ---
		TSharedPtr<FJsonObject> DryParams = RemoveVariableParams(Blueprint, TEXT("bOpen"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
		const FCortexCommandResult Dry = FCortexBPStructureOps::RemoveVariable(DryParams);
		TestTrue(TEXT("Dry run succeeds"), Dry.bSuccess);
		if (Dry.bSuccess)
		{
			TestEqual(TEXT("Dry run applies nothing"), Dry.Data->GetStringField(TEXT("apply_status")), FString(TEXT("dry_run")));
			TestFalse(TEXT("Dry run never reports removed"), Dry.Data->GetBoolField(TEXT("removed")));
			TestEqual(TEXT("Dry run lists both nodes"), CountArray(Dry.Data, TEXT("referencing_nodes")), 2);
			TestTrue(TEXT("Dry run keeps the fingerprint"),
				FingerprintsEqual(Dry.Data->GetObjectField(TEXT("fingerprint_before")), FCortexBPMigrationGuard::ComputeFingerprint(Blueprint)));
		}
		TestEqual(TEXT("Dry run leaves the dirty flag"), Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
		TestTrue(TEXT("Dry run keeps the variable"), HasVariable(Blueprint, TEXT("bOpen")));

		// --- remove_referencing_nodes removes the variable and the nodes ---
		TSharedPtr<FJsonObject> RemoveNodesParams = RemoveVariableParams(Blueprint, TEXT("bOpen"));
		RemoveNodesParams->SetBoolField(TEXT("remove_referencing_nodes"), true);
		const FCortexCommandResult WithNodes = FCortexBPStructureOps::RemoveVariable(RemoveNodesParams);
		TestTrue(TEXT("Opt-in reference removal succeeds"), WithNodes.bSuccess);
		if (WithNodes.bSuccess)
		{
			TestTrue(TEXT("Removed is true"), WithNodes.Data->GetBoolField(TEXT("removed")));
			TestEqual(TEXT("Both nodes reported removed"), CountArray(WithNodes.Data, TEXT("removed_nodes")), 2);
			TestFalse(TEXT("Get node destroyed"), HasNodeGuid(Blueprint, Get->NodeGuid));
			TestFalse(TEXT("Set node destroyed"), HasNodeGuid(Blueprint, Set->NodeGuid));
		}
		TestFalse(TEXT("Variable removed with the nodes"), HasVariable(Blueprint, TEXT("bOpen")));
		RetireBlueprint(Blueprint);
	}

	// --- A same-named member of an unrelated class is not a reference and is never destroyed ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemoveForeignName"), AActor::StaticClass());
		UBlueprint* Foreign = MakeBlueprint(TEXT("GuardedRemoveForeignOwner"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Foreign-name Blueprint"), Blueprint) || !TestNotNull(TEXT("Foreign Blueprint"), Foreign))
		{
			return false;
		}
		AddVariable(Foreign, TEXT("Health"), DoublePin(), TEXT("1.0"));
		FKismetEditorUtilities::CompileBlueprint(Foreign);
		AddVariable(Blueprint, TEXT("Health"), DoublePin(), TEXT("2.0"));
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		FGraphNodeCreator<UK2Node_VariableGet> ForeignCreator(*EventGraph);
		UK2Node_VariableGet* ForeignNode = ForeignCreator.CreateNode();
		ForeignNode->VariableReference.SetExternalMember(TEXT("Health"), Foreign->GeneratedClass);
		ForeignCreator.Finalize();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> DryParams = RemoveVariableParams(Blueprint, TEXT("Health"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		DryParams->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult Dry = FCortexBPStructureOps::RemoveVariable(DryParams);
		if (TestTrue(TEXT("Foreign-name dry run succeeds"), Dry.bSuccess && Dry.Data.IsValid()))
		{
			TestEqual(TEXT("The foreign node is not a reference"), CountArray(Dry.Data, TEXT("referencing_nodes")), 0);
			TestTrue(TEXT("The foreign node can be removed with the variable"), Dry.Data->GetBoolField(TEXT("can_apply")));
		}

		TSharedPtr<FJsonObject> OptInParams = RemoveVariableParams(Blueprint, TEXT("Health"));
		OptInParams->SetBoolField(TEXT("remove_referencing_nodes"), true);
		const FCortexCommandResult Removed = FCortexBPStructureOps::RemoveVariable(OptInParams);
		TestTrue(TEXT("Foreign-name removal succeeds"), Removed.bSuccess);
		TestFalse(TEXT("Variable removed"), HasVariable(Blueprint, TEXT("Health")));
		TestTrue(TEXT("The foreign node survives"), HasNodeGuid(Blueprint, ForeignNode->NodeGuid));
		RetireBlueprint(Foreign);
		RetireBlueprint(Blueprint);
	}

	// --- A SaveGame variable refuses POTENTIAL_DATA_LOSS until acknowledged ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemoveSaveGame"), AActor::StaticClass());
		if (!TestNotNull(TEXT("SaveGame Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("SavedName"), StringPin(), TEXT("hero"), /*bSaveGame=*/true);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> DryParams = RemoveVariableParams(Blueprint, TEXT("SavedName"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Dry = FCortexBPStructureOps::RemoveVariable(DryParams);
		if (TestTrue(TEXT("SaveGame dry run succeeds"), Dry.bSuccess && Dry.Data.IsValid()))
		{
			TestEqual(TEXT("Dry run lists the SaveGame finding"), CountArray(Dry.Data, TEXT("save_game_findings")), 1);
			TestTrue(TEXT("save_game_removed reported"),
				ReadStrings(Dry.Data, TEXT("required_acknowledgment")).Contains(TEXT("save_game:SavedName")));
		}

		const FCortexCommandResult Refused = FCortexBPStructureOps::RemoveVariable(
			RemoveVariableParams(Blueprint, TEXT("SavedName")));
		TestFalse(TEXT("SaveGame variable refuses"), Refused.bSuccess);
		TestEqual(TEXT("SaveGame refusal code"), Refused.ErrorCode, CortexErrorCodes::PotentialDataLoss);
		if (TestTrue(TEXT("SaveGame refusal carries keys"), Refused.ErrorDetails.IsValid()))
		{
			TestEqual(TEXT("Exactly one key required"), CountArray(Refused.ErrorDetails, TEXT("required_acknowledgment")), 1);
			TestTrue(TEXT("Key names the variable"),
				ReadStrings(Refused.ErrorDetails, TEXT("required_acknowledgment")).Contains(TEXT("save_game:SavedName")));
		}
		TestTrue(TEXT("Refused SaveGame variable is untouched"), HasVariable(Blueprint, TEXT("SavedName")));

		TSharedPtr<FJsonObject> AckParams = RemoveVariableParams(Blueprint, TEXT("SavedName"));
		SetStrings(AckParams, TEXT("acknowledged_losses"), {TEXT("save_game:SavedName")});
		const FCortexCommandResult Acknowledged = FCortexBPStructureOps::RemoveVariable(AckParams);
		TestTrue(TEXT("Acknowledged removal succeeds"), Acknowledged.bSuccess);
		TestFalse(TEXT("Acknowledged variable is removed"), HasVariable(Blueprint, TEXT("SavedName")));

		// A stale acknowledgment (the variable is no longer SaveGame-relevant) is refused, not ignored.
		UBlueprint* Plain = MakeBlueprint(TEXT("GuardedRemoveStaleAck"), AActor::StaticClass());
		if (TestNotNull(TEXT("Plain Blueprint"), Plain))
		{
			AddVariable(Plain, TEXT("PlainValue"), DoublePin(), TEXT("1.0"));
			FKismetEditorUtilities::CompileBlueprint(Plain);
			TSharedPtr<FJsonObject> StaleParams = RemoveVariableParams(Plain, TEXT("PlainValue"));
			SetStrings(StaleParams, TEXT("acknowledged_losses"), {TEXT("save_game:PlainValue")});
			const FCortexCommandResult Stale = FCortexBPStructureOps::RemoveVariable(StaleParams);
			TestFalse(TEXT("Stale acknowledgment refuses"), Stale.bSuccess);
			TestEqual(TEXT("Stale acknowledgment code"), Stale.ErrorCode, CortexErrorCodes::PotentialDataLoss);
			TestTrue(TEXT("Stale acknowledgment keeps the variable"), HasVariable(Plain, TEXT("PlainValue")));
			RetireBlueprint(Plain);
		}
		RetireBlueprint(Blueprint);
	}

	// --- A stale expected_fingerprint refuses before any mutation ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemoveFingerprint"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Fingerprint Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("FingerprintTarget"), DoublePin(), TEXT("1.0"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> DryParams = RemoveVariableParams(Blueprint, TEXT("FingerprintTarget"));
		DryParams->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Dry = FCortexBPStructureOps::RemoveVariable(DryParams);
		if (!TestTrue(TEXT("Fingerprint dry run succeeds"), Dry.bSuccess && Dry.Data.IsValid()))
		{
			RetireBlueprint(Blueprint);
			return false;
		}
		const TSharedPtr<FJsonObject> StaleFingerprint = Dry.Data->GetObjectField(TEXT("fingerprint_before"));

		// Another edit moves the fingerprint away from the one the caller read.
		AddVariable(Blueprint, TEXT("LaterEdit"), DoublePin(), TEXT("3.0"));
		TSharedPtr<FJsonObject> StaleParams = RemoveVariableParams(Blueprint, TEXT("FingerprintTarget"));
		StaleParams->SetObjectField(TEXT("expected_fingerprint"), StaleFingerprint);
		const FCortexCommandResult Stale = FCortexBPStructureOps::RemoveVariable(StaleParams);
		TestFalse(TEXT("Stale fingerprint refuses"), Stale.bSuccess);
		TestEqual(TEXT("Stale fingerprint code"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("Stale fingerprint keeps the variable"), HasVariable(Blueprint, TEXT("FingerprintTarget")));
		RetireBlueprint(Blueprint);
	}

	// --- compile:true rolls back when the compile fails ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("GuardedRemoveCompileFailure"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Rollback Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("RollbackTarget"), DoublePin(), TEXT("4.5"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		FString SnapshotError;
		const TSharedPtr<FJsonObject> SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);

		TSharedPtr<FJsonObject> Params = RemoveVariableParams(Blueprint, TEXT("RollbackTarget"));
		Params->SetBoolField(TEXT("compile"), true);
		FCortexCommandResult Result;
		{
			FCortexBPStructureOps::SetRemoveVariablePostApplyHookForTest([](UBlueprint* Target)
			{
				// Two custom events with one name: the compile after the removal reports an error.
				UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Target);
				for (int32 Index = 0; Index < 2; ++Index)
				{
					UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
					Event->CreateNewGuid();
					Event->CustomFunctionName = FName(TEXT("GuardedRemoveDuplicateEvent"));
					EventGraph->AddNode(Event, false, false);
					Event->AllocateDefaultPins();
				}
			});
			Result = FCortexBPStructureOps::RemoveVariable(Params);
			FCortexBPStructureOps::ClearRemoveVariablePostApplyHookForTest();
		}
		TestFalse(TEXT("Forced compile failure fails"), Result.bSuccess);
		TestEqual(TEXT("Forced compile failure code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
		{
			TestFalse(TEXT("Failure never reports removed"), Result.ErrorDetails->GetBoolField(TEXT("removed")));
			TestEqual(TEXT("Rollback restored"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		}
		TestTrue(TEXT("Variable restored"), HasVariable(Blueprint, TEXT("RollbackTarget")));

		const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);
		if (SnapshotBefore.IsValid() && TestNotNull(TEXT("Post-call snapshot"), SnapshotAfter.Get()))
		{
			TArray<TSharedPtr<FJsonValue>> Differences;
			TSharedPtr<FJsonObject> Counts;
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(
				SnapshotBefore, SnapshotAfter, TSet<FString>(), Differences, Counts);
			TestTrue(TEXT("Snapshots comparable"), Diff.bSuccess);
			for (const TSharedPtr<FJsonValue>& Difference : Differences)
			{
				AddError(FString::Printf(TEXT("Rollback difference: %s"), *JsonToString(Difference)));
			}
			TestEqual(TEXT("Snapshot after equals snapshot before"), Differences.Num(), 0);
		}
		RetireBlueprint(Blueprint);
	}

	return true;
}
