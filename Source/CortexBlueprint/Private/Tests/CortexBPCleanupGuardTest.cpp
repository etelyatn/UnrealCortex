#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexBPRenameMemberTestSupport.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonObject> CleanupParams(const UBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		return Params;
	}

	int32 CountArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? Values->Num() : 0;
	}

	TSharedPtr<FJsonObject> ReadObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Value) && Value != nullptr && Value->IsValid()
			? *Value : nullptr;
	}

	/** The `name` entry of the per-variable object in Field; null when absent. */
	TSharedPtr<FJsonObject> ReadNamedEntry(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject> Map = ReadObject(Object, Field);
		return Map.IsValid() ? ReadObject(Map, Name) : nullptr;
	}

	UK2Node_VariableGet* AddGetNode(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		Node->VariableReference.SetSelfMember(FName(Name));
		Creator.Finalize();
		return Node;
	}

	void AddDuplicateEventFault(UBlueprint* Blueprint)
	{
		// Two custom events with one name: the compile reports an error.
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
			Event->CreateNewGuid();
			Event->CustomFunctionName = FName(TEXT("GuardedCleanupDuplicateEvent"));
			EventGraph->AddNode(Event, false, false);
			Event->AllocateDefaultPins();
		}
	}

	const ACortexBPAdoptParent* AdoptDefaults(UBlueprint* Blueprint)
	{
		return Blueprint != nullptr && Blueprint->GeneratedClass != nullptr
			? Cast<ACortexBPAdoptParent>(Blueprint->GeneratedClass->GetDefaultObject(false)) : nullptr;
	}
}

/**
 * The CDO-order proof (design item 7): E1 runs at RefreshAllNodes, so the native parent replaces a compatible
 * variable before any compile; the value moves to the native member in the one compile (E4), which must carry the
 * editor's delta flags or the new parent's constructor default wins (E5).
 * Engine citations: Editor/UnrealEd/Private/Kismet2/BlueprintEditorUtils.cpp:704-753 (MarkBlueprintAsStructurallyModified
 * regenerates the skeleton) and :1890-1919 (RefreshAllNodes), KismetCompiler.cpp (ValidateVariableNames renames an
 * incompatible same-named variable).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupGuardedCdoOrderTest,
	"Cortex.Blueprint.Cleanup.GuardedCdoOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupGuardedCdoOrderTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using CortexAdoptMemberTest::HasNodeGuid;
	using CortexRenameTest::RetireBlueprint;

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();

	// --- A compatible same-named variable is adopted by the engine: value preserved, nodes preserved ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupCdoOrder"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
		UK2Node_VariableGet* Get = AddGetNode(Blueprint, TEXT("bPowerOn"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestNotNull(TEXT("Get node"), Get))
		{
			RetireBlueprint(Blueprint);
			return false;
		}

		TSharedPtr<FJsonObject> Params = CleanupParams(Blueprint);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		SetStrings(Params, TEXT("remove_variables"), {TEXT("bPowerOn")});
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);

		const FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
		TestTrue(TEXT("CDO-order cleanup succeeds"), Result.bSuccess);
		if (Result.bSuccess && TestTrue(TEXT("Result data"), Result.Data.IsValid()))
		{
			TestTrue(TEXT("Reparented"), Result.Data->GetBoolField(TEXT("reparented")));
			TestEqual(TEXT("Engine adoption reported"), ReadStrings(Result.Data, TEXT("adopted_by_engine")), TArray<FString>{TEXT("bPowerOn")});
			TestEqual(TEXT("Adopted variable removed"), ReadStrings(Result.Data, TEXT("removed_variables")), TArray<FString>{TEXT("bPowerOn")});
			for (const FString& Warning : ReadStrings(Result.Data, TEXT("warnings")))
			{
				TestFalse(TEXT("No 'not found, skipped' warning"), Warning.Contains(TEXT("not found")));
			}
			TestEqual(TEXT("Compile succeeded"), Result.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
			TestEqual(TEXT("Readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));

			const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Result.Data, TEXT("cdo_readback"), TEXT("bPowerOn"));
			if (TestTrue(TEXT("CDO readback reported"), Readback.IsValid()))
			{
				TestEqual(TEXT("CDO value before"), Readback->GetStringField(TEXT("before")), FString(TEXT("True")));
				TestEqual(TEXT("CDO value after"), Readback->GetStringField(TEXT("after")), FString(TEXT("True")));
				TestTrue(TEXT("CDO value preserved"), Readback->GetBoolField(TEXT("preserved")));
			}

			const TSharedPtr<FJsonObject> ExpectationMap = ReadObject(Result.Data, TEXT("cdo_expectations"));
			if (TestTrue(TEXT("Stage expectation reported"), ExpectationMap.IsValid()))
			{
				TestEqual(TEXT("Expectation is the pre-call value"), ExpectationMap->GetStringField(TEXT("bPowerOn")), FString(TEXT("True")));
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (TestTrue(TEXT("Referencing nodes reported"),
				Result.Data->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr))
			{
				TestEqual(TEXT("One referencing node"), Nodes->Num(), 1);
				if (Nodes->Num() == 1)
				{
					const TSharedPtr<FJsonObject> Node = (*Nodes)[0]->AsObject();
					TestEqual(TEXT("Adopted node is preserved"), Node->GetStringField(TEXT("resolution")), FString(TEXT("preserved")));
				}
			}
		}
		TestFalse(TEXT("Variable adopted away"), HasVariable(Blueprint, TEXT("bPowerOn")));
		TestTrue(TEXT("Node preserved through adoption"), HasNodeGuid(Blueprint, Get->NodeGuid));
		TestTrue(TEXT("Parent changed"), Blueprint->ParentClass == ACortexBPAdoptParent::StaticClass());
		const ACortexBPAdoptParent* Defaults = AdoptDefaults(Blueprint);
		if (TestNotNull(TEXT("Actor CDO"), Defaults))
		{
			TestTrue(TEXT("Adopted value survived into the native CDO"), Defaults->bPowerOn);
			TestEqual(TEXT("The native parent's own default won the compile"), Defaults->InitialLifeSpan, 7.f);
		}
		RetireBlueprint(Blueprint);
	}

	// --- I2: a numeric-convertible adoption is read back (double 2.5 -> native float) ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupGuardConvertible"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Convertible Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("Speed"), DoublePin(), TEXT("2.5"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> Params = CleanupParams(Blueprint);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		SetStrings(Params, TEXT("remove_variables"), {TEXT("Speed")});
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
		TestTrue(TEXT("Convertible adoption succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestTrue(TEXT("Convertible adoption reported"), ReadStrings(Result.Data, TEXT("adopted_by_engine")).Contains(TEXT("Speed")));
			const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Result.Data, TEXT("cdo_readback"), TEXT("Speed"));
			if (TestTrue(TEXT("Convertible adoption is read back"), Readback.IsValid()))
			{
				TestTrue(TEXT("Convertible adoption is checked"), Readback->GetBoolField(TEXT("checked")));
				TestTrue(TEXT("Convertible value preserved"), Readback->GetBoolField(TEXT("preserved")));
			}
		}
		const ACortexBPAdoptParent* Defaults = AdoptDefaults(Blueprint);
		if (TestNotNull(TEXT("Convertible CDO"), Defaults))
		{
			TestEqual(TEXT("Converted value"), Defaults->Speed, 2.5f);
			TestFalse(TEXT("No renamed variable left"), HasVariable(Blueprint, TEXT("Speed_0")));
		}
		RetireBlueprint(Blueprint);
	}

	// --- A same-named incompatible variable is renamed by the engine and removed explicitly ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupGuardMismatch"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Mismatch Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("TypeMismatch"), StringPin(), TEXT("text"));
		UK2Node_VariableGet* Get = AddGetNode(Blueprint, TEXT("TypeMismatch"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestNotNull(TEXT("Mismatch get node"), Get))
		{
			RetireBlueprint(Blueprint);
			return false;
		}

		TSharedPtr<FJsonObject> Params = CleanupParams(Blueprint);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		SetStrings(Params, TEXT("remove_variables"), {TEXT("TypeMismatch")});
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);

		// The engine rename this case pins is reported by the compiler, not caused by the command.
		AddExpectedMessagePlain(TEXT("ValidateVariableNames name TypeMismatch"), EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Found a member variable with a conflicting name (TypeMismatch)"), EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Type mismatch in TypeMismatch"), EAutomationExpectedMessageFlags::Contains, 1);

		const FCortexCommandResult Refused = FCortexBPCleanupOps::CleanupMigration(Params);
		TestFalse(TEXT("Renamed variable with nodes refuses without the opt-in"), Refused.bSuccess);
		TestEqual(TEXT("Reference refusal code"), Refused.ErrorCode, CortexErrorCodes::MemberReferenced);
		if (TestTrue(TEXT("Refusal lists the node"), Refused.ErrorDetails.IsValid()))
		{
			TestEqual(TEXT("One referencing node"), CountArray(Refused.ErrorDetails, TEXT("referencing_nodes")), 1);
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (Refused.ErrorDetails->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr && Nodes->Num() == 1)
			{
				TestEqual(TEXT("Refusal names the variable"), (*Nodes)[0]->AsObject()->GetStringField(TEXT("variable")), FString(TEXT("TypeMismatch")));
			}
		}
		TestTrue(TEXT("Refusal keeps the variable"), HasVariable(Blueprint, TEXT("TypeMismatch")));
		TestTrue(TEXT("Refusal keeps the node"), HasNodeGuid(Blueprint, Get->NodeGuid));
		TestTrue(TEXT("Refusal keeps the parent"), Blueprint->ParentClass == AActor::StaticClass());

		Params->SetBoolField(TEXT("remove_referencing_nodes"), true);
		const FCortexCommandResult Removed = FCortexBPCleanupOps::CleanupMigration(Params);
		TestTrue(TEXT("Opt-in removal succeeds"), Removed.bSuccess);
		if (Removed.bSuccess && Removed.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Renamed = nullptr;
			if (TestTrue(TEXT("Renamed variable reported"),
				Removed.Data->TryGetArrayField(TEXT("engine_renamed"), Renamed) && Renamed != nullptr && Renamed->Num() == 1))
			{
				const TSharedPtr<FJsonObject> Entry = (*Renamed)[0]->AsObject();
				TestEqual(TEXT("Original name"), Entry->GetStringField(TEXT("from")), FString(TEXT("TypeMismatch")));
				TestEqual(TEXT("Engine-renamed name"), Entry->GetStringField(TEXT("to")), FString(TEXT("TypeMismatch_0")));
			}
			TestEqual(TEXT("Explicit node removal reported"), CountArray(Removed.Data, TEXT("removed_nodes")), 1);
			TestEqual(TEXT("Not adopted by the engine"), CountArray(Removed.Data, TEXT("adopted_by_engine")), 0);
		}
		TestFalse(TEXT("Renamed variable removed"), HasVariable(Blueprint, TEXT("TypeMismatch")));
		TestFalse(TEXT("Engine rename did not survive"), HasVariable(Blueprint, TEXT("TypeMismatch_0")));
		TestFalse(TEXT("Node removed with the variable"), HasNodeGuid(Blueprint, Get->NodeGuid));
		RetireBlueprint(Blueprint);
	}

	// --- N1: an explicit removal of a numerically incompatible variable is not a value-preserving move ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupGuardExplicitNumeric"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Explicit-numeric Blueprint"), Blueprint))
		{
			return false;
		}
		// The native TypeMismatch is an int32; the Blueprint declares a double, so the engine renames it and the value is
		// NOT preserved. The readback must not check it (a numeric check here used to roll a correct removal back).
		AddVariable(Blueprint, TEXT("TypeMismatch"), DoublePin(), TEXT("2.5"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		TSharedPtr<FJsonObject> Params = CleanupParams(Blueprint);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		SetStrings(Params, TEXT("remove_variables"), {TEXT("TypeMismatch")});
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);
		AddExpectedMessagePlain(TEXT("ValidateVariableNames name TypeMismatch"), EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Found a member variable with a conflicting name (TypeMismatch)"), EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Potential data loss during conversion of integer property TypeMismatch"), EAutomationExpectedMessageFlags::Contains, 1);
		const FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
		TestTrue(TEXT("Explicit numeric removal succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("Readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
			const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Result.Data, TEXT("cdo_readback"), TEXT("TypeMismatch"));
			if (TestTrue(TEXT("Explicit numeric removal is read back"), Readback.IsValid()))
			{
				TestFalse(TEXT("Its value is not checked"), Readback->GetBoolField(TEXT("checked")));
			}
			const TArray<TSharedPtr<FJsonValue>>* Renamed = nullptr;
			if (TestTrue(TEXT("Engine rename reported"),
				Result.Data->TryGetArrayField(TEXT("engine_renamed"), Renamed) && Renamed != nullptr && Renamed->Num() == 1))
			{
				TestEqual(TEXT("Renamed to the uniquified name"), (*Renamed)[0]->AsObject()->GetStringField(TEXT("to")), FString(TEXT("TypeMismatch_0")));
			}
		}
		TestFalse(TEXT("The variable is gone"), HasVariable(Blueprint, TEXT("TypeMismatch")));
		TestTrue(TEXT("The parent changed"), Blueprint->ParentClass == ACortexBPAdoptParent::StaticClass());
		RetireBlueprint(Blueprint);
	}

	// --- BuildPlan runs: an unlisted incompatible variable blocks, acknowledged collisions are accepted ---
	{
		UBlueprint* Unlisted = MakeBlueprint(TEXT("CleanupGuardUnlisted"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Unlisted Blueprint"), Unlisted))
		{
			return false;
		}
		AddVariable(Unlisted, TEXT("TypeMismatch"), StringPin(), TEXT("text"));
		FKismetEditorUtilities::CompileBlueprint(Unlisted);

		TSharedPtr<FJsonObject> Params = CleanupParams(Unlisted);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult Blocked = FCortexBPCleanupOps::CleanupMigration(Params);
		TestFalse(TEXT("Unlisted incompatible variable blocks"), Blocked.bSuccess);
		TestEqual(TEXT("Blocking collision code"), Blocked.ErrorCode, CortexErrorCodes::NameCollision);
		if (TestTrue(TEXT("Blocking details"), Blocked.ErrorDetails.IsValid()))
		{
			TestTrue(TEXT("Blocking key listed"), ReadStrings(Blocked.ErrorDetails, TEXT("blocking")).Contains(TEXT("variable:TypeMismatch")));
		}
		TestTrue(TEXT("Blocked reparent keeps the parent"), Unlisted->ParentClass == AActor::StaticClass());
		RetireBlueprint(Unlisted);

		UBlueprint* Adoptable = MakeBlueprint(TEXT("CleanupGuardAck"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Acknowledgeable Blueprint"), Adoptable))
		{
			return false;
		}
		AddVariable(Adoptable, TEXT("bPowerOn"), BoolPin(), TEXT("true"));
		FKismetEditorUtilities::CompileBlueprint(Adoptable);

		TSharedPtr<FJsonObject> AckParams = CleanupParams(Adoptable);
		AckParams->SetStringField(TEXT("new_parent_class"), Target);
		AckParams->SetBoolField(TEXT("compile"), true);
		AckParams->SetBoolField(TEXT("save"), false);

		// I1: the dry run reports the plan and can_apply instead of refusing with the acknowledgment error.
		TSharedPtr<FJsonObject> DryAckParams = CleanupParams(Adoptable);
		DryAckParams->SetStringField(TEXT("new_parent_class"), Target);
		DryAckParams->SetBoolField(TEXT("dry_run"), true);
		DryAckParams->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult DryPlan = FCortexBPCleanupOps::CleanupMigration(DryAckParams);
		if (TestTrue(TEXT("Unacknowledged dry run reports a plan"), DryPlan.bSuccess && DryPlan.Data.IsValid()))
		{
			TestEqual(TEXT("Dry run applies nothing"), DryPlan.Data->GetStringField(TEXT("apply_status")), FString(TEXT("dry_run")));
			TestFalse(TEXT("Dry run cannot apply without the acknowledgment"), DryPlan.Data->GetBoolField(TEXT("can_apply")));
			TestTrue(TEXT("Dry run lists the required key"),
				ReadStrings(DryPlan.Data, TEXT("required_acknowledgment")).Contains(TEXT("variable:bPowerOn")));
		}
		TestTrue(TEXT("Dry run keeps the variable"), HasVariable(Adoptable, TEXT("bPowerOn")));

		const FCortexCommandResult NeedsAck = FCortexBPCleanupOps::CleanupMigration(AckParams);
		TestFalse(TEXT("Engine-replaced variable needs acknowledgment"), NeedsAck.bSuccess);
		TestEqual(TEXT("Acknowledgment code"), NeedsAck.ErrorCode, CortexErrorCodes::PotentialDataLoss);
		if (TestTrue(TEXT("Acknowledgment details"), NeedsAck.ErrorDetails.IsValid()))
		{
			TestTrue(TEXT("Required key listed"),
				ReadStrings(NeedsAck.ErrorDetails, TEXT("required_acknowledgment")).Contains(TEXT("variable:bPowerOn")));
		}
		TestTrue(TEXT("Refused cleanup keeps the variable"), HasVariable(Adoptable, TEXT("bPowerOn")));

		SetStrings(AckParams, TEXT("acknowledged_collisions"), {TEXT("variable:bPowerOn")});
		const FCortexCommandResult Acknowledged = FCortexBPCleanupOps::CleanupMigration(AckParams);
		TestTrue(TEXT("Acknowledged cleanup succeeds"), Acknowledged.bSuccess);
		if (Acknowledged.bSuccess && Acknowledged.Data.IsValid())
		{
			TestTrue(TEXT("Adoption reported"), ReadStrings(Acknowledged.Data, TEXT("adopted_by_engine")).Contains(TEXT("bPowerOn")));
			// I2: an adopted variable that was not listed in remove_variables is verified too.
			const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Acknowledged.Data, TEXT("cdo_readback"), TEXT("bPowerOn"));
			if (TestTrue(TEXT("Unlisted adoption is read back"), Readback.IsValid()))
			{
				TestTrue(TEXT("Unlisted adoption is checked"), Readback->GetBoolField(TEXT("checked")));
				TestTrue(TEXT("Unlisted adoption is preserved"), Readback->GetBoolField(TEXT("preserved")));
			}
			const TSharedPtr<FJsonObject> Expectations = ReadObject(Acknowledged.Data, TEXT("cdo_expectations"));
			if (TestTrue(TEXT("Unlisted adoption carries an expectation"), Expectations.IsValid()))
			{
				TestEqual(TEXT("Unlisted expectation value"), Expectations->GetStringField(TEXT("bPowerOn")), FString(TEXT("True")));
			}
		}
		TestFalse(TEXT("Acknowledged variable adopted away"), HasVariable(Adoptable, TEXT("bPowerOn")));
		RetireBlueprint(Adoptable);
	}

	// --- A CDO divergence fails closed and restores ---
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupGuardDivergence"), AActor::StaticClass());
		if (!TestNotNull(TEXT("Divergence Blueprint"), Blueprint))
		{
			return false;
		}
		AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		FString SnapshotError;
		const TSharedPtr<FJsonObject> SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);

		TSharedPtr<FJsonObject> Params = CleanupParams(Blueprint);
		Params->SetStringField(TEXT("new_parent_class"), Target);
		SetStrings(Params, TEXT("remove_variables"), {TEXT("bPowerOn")});
		Params->SetBoolField(TEXT("compile"), true);
		Params->SetBoolField(TEXT("save"), false);

		FCortexCommandResult Result;
		FCortexBPCleanupOps::SetCdoDivergenceTestHook([](UBlueprint* Target2)
		{
			if (Target2 == nullptr || Target2->GeneratedClass == nullptr)
			{
				return;
			}
			UObject* Cdo = Target2->GeneratedClass->GetDefaultObject(false);
			if (FBoolProperty* Property = Cdo != nullptr
				? FindFProperty<FBoolProperty>(Cdo->GetClass(), TEXT("bPowerOn")) : nullptr)
			{
				Property->SetPropertyValue_InContainer(Cdo, false);
			}
		});
		Result = FCortexBPCleanupOps::CleanupMigration(Params);
		FCortexBPCleanupOps::SetCdoDivergenceTestHook(nullptr);

		TestFalse(TEXT("Diverged CDO fails closed"), Result.bSuccess);
		TestEqual(TEXT("Divergence code"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		if (TestTrue(TEXT("Divergence details"), Result.ErrorDetails.IsValid()))
		{
			TestEqual(TEXT("Rollback restored"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
			const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Result.ErrorDetails, TEXT("cdo_readback"), TEXT("bPowerOn"));
			if (TestTrue(TEXT("Divergence readback reported"), Readback.IsValid()))
			{
				TestEqual(TEXT("Diverged value reported"), Readback->GetStringField(TEXT("after")), FString(TEXT("False")));
				TestFalse(TEXT("Divergence is not preserved"), Readback->GetBoolField(TEXT("preserved")));
			}
		}
		TestTrue(TEXT("Variable restored"), HasVariable(Blueprint, TEXT("bPowerOn")));
		TestTrue(TEXT("Parent restored"), Blueprint->ParentClass == AActor::StaticClass());

		const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);
		if (SnapshotBefore.IsValid() && TestNotNull(TEXT("Post-call snapshot"), SnapshotAfter.Get()))
		{
			TArray<TSharedPtr<FJsonValue>> Differences;
			TSharedPtr<FJsonObject> Counts;
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(
				SnapshotBefore, SnapshotAfter, TSet<FString>(), Differences, Counts);
			TestTrue(TEXT("Snapshots comparable"), Diff.bSuccess);
			TestEqual(TEXT("Snapshot after equals snapshot before"), Differences.Num(), 0);
		}
		RetireBlueprint(Blueprint);
	}

	return true;
}

/**
 * The wrapper's two-call flow: stage with compile:false (the parent assignment and the engine adoption, no compile),
 * then a finalize-only call that compiles with the delta flags, verifies the stage's `cdo_expectations` through
 * `expected_cdo_values` and saves only after a clean compile.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupGuardedStagedTest,
	"Cortex.Blueprint.Cleanup.GuardedStaged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupGuardedStagedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;
	using CortexAdoptMemberTest::HasNodeGuid;
	using CortexRenameTest::RetireBlueprint;

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();

	UBlueprint* Blueprint = MakeBlueprint(TEXT("CleanupGuardedStaged"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Staged Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	UK2Node_VariableGet* Get = AddGetNode(Blueprint, TEXT("bPowerOn"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestNotNull(TEXT("Staged get node"), Get))
	{
		RetireBlueprint(Blueprint);
		return false;
	}

	// --- dry_run lists the reference, the SaveGame finding and the reparent plan ---
	TSharedPtr<FJsonObject> DryParams = CleanupParams(Blueprint);
	DryParams->SetStringField(TEXT("new_parent_class"), Target);
	SetStrings(DryParams, TEXT("remove_variables"), {TEXT("bPowerOn")});
	DryParams->SetBoolField(TEXT("dry_run"), true);
	DryParams->SetBoolField(TEXT("save"), false);
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	const FCortexCommandResult Dry = FCortexBPCleanupOps::CleanupMigration(DryParams);
	TestTrue(TEXT("Dry run succeeds"), Dry.bSuccess);
	if (Dry.bSuccess && TestTrue(TEXT("Dry run data"), Dry.Data.IsValid()))
	{
		TestEqual(TEXT("Dry run applies nothing"), Dry.Data->GetStringField(TEXT("apply_status")), FString(TEXT("dry_run")));
		TestFalse(TEXT("Dry run never reports reparented"), Dry.Data->GetBoolField(TEXT("reparented")));
		TestEqual(TEXT("Dry run lists the node"), CountArray(Dry.Data, TEXT("referencing_nodes")), 1);
		const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Dry.Data, TEXT("cdo_readback"), TEXT("bPowerOn"));
		if (TestTrue(TEXT("Dry run reports the expected CDO value"), Readback.IsValid()))
		{
			TestEqual(TEXT("Dry run before value"), Readback->GetStringField(TEXT("before")), FString(TEXT("True")));
		}
		const TSharedPtr<FJsonObject> Plan = ReadObject(Dry.Data, TEXT("reparent_plan"));
		if (TestTrue(TEXT("Dry run carries the reparent plan"), Plan.IsValid()))
		{
			const TSharedPtr<FJsonObject> Collision = FindByKey(Plan, TEXT("collisions"), TEXT("variable:bPowerOn"));
			if (TestTrue(TEXT("Plan lists the replaced variable"), Collision.IsValid()))
			{
				TestEqual(TEXT("Collision resolution"), Collision->GetStringField(TEXT("resolution")), FString(TEXT("engine_replaces")));
			}
			TestEqual(TEXT("Plan save game findings"), CountArray(Plan, TEXT("save_game_findings")), 0);
		}
		TestEqual(TEXT("Dry run keeps the variable"), HasVariable(Blueprint, TEXT("bPowerOn")), true);
		TestTrue(TEXT("Dry run leaves the fingerprint"),
			FJsonValue::CompareEqual(*MakeShared<FJsonValueObject>(Dry.Data->GetObjectField(TEXT("fingerprint_before"))),
				*MakeShared<FJsonValueObject>(FingerprintBefore)));
	}
	TestEqual(TEXT("Dry run leaves the dirty flag"), Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	TestTrue(TEXT("Dry run keeps the node"), HasNodeGuid(Blueprint, Get->NodeGuid));

	// --- stage: reparent and adopt without a compile ---
	TSharedPtr<FJsonObject> StageParams = CleanupParams(Blueprint);
	StageParams->SetStringField(TEXT("new_parent_class"), Target);
	SetStrings(StageParams, TEXT("remove_variables"), {TEXT("bPowerOn")});
	StageParams->SetBoolField(TEXT("compile"), false);
	StageParams->SetBoolField(TEXT("save"), false);
	const FCortexCommandResult Stage = FCortexBPCleanupOps::CleanupMigration(StageParams);
	if (!TestTrue(TEXT("Stage succeeds"), Stage.bSuccess && Stage.Data.IsValid()))
	{
		RetireBlueprint(Blueprint);
		return false;
	}
	TestTrue(TEXT("Stage reparents"), Stage.Data->GetBoolField(TEXT("reparented")));
	TestEqual(TEXT("Stage adopts"), ReadStrings(Stage.Data, TEXT("adopted_by_engine")), TArray<FString>{TEXT("bPowerOn")});
	TestEqual(TEXT("Stage compiles nothing"), Stage.Data->GetStringField(TEXT("compile_status")), FString(TEXT("skipped")));
	TestTrue(TEXT("Stage keeps the package dirty"), Stage.Data->GetBoolField(TEXT("dirty_after")));
	const TSharedPtr<FJsonObject> Expectations = ReadObject(Stage.Data, TEXT("cdo_expectations"));
	if (!TestTrue(TEXT("Stage returns cdo_expectations"), Expectations.IsValid()))
	{
		RetireBlueprint(Blueprint);
		return false;
	}
	TestEqual(TEXT("Stage expectation value"), Expectations->GetStringField(TEXT("bPowerOn")), FString(TEXT("True")));
	TestFalse(TEXT("Stage removed the variable"), HasVariable(Blueprint, TEXT("bPowerOn")));
	TestTrue(TEXT("Stage keeps the node"), HasNodeGuid(Blueprint, Get->NodeGuid));
	// The stage assigns the parent but compiles nothing: only the finalize compile applies the new native defaults.
	TestTrue(TEXT("Stage already assigned the parent"), Blueprint->ParentClass == ACortexBPAdoptParent::StaticClass());

	// --- finalize-only: compile with the delta flags, verify expectations, do not save ---
	TSharedPtr<FJsonObject> FinalizeParams = CleanupParams(Blueprint);
	FinalizeParams->SetBoolField(TEXT("compile"), true);
	FinalizeParams->SetBoolField(TEXT("save"), false);
	FinalizeParams->SetObjectField(TEXT("expected_cdo_values"), Expectations);
	const FCortexCommandResult Finalized = FCortexBPCleanupOps::CleanupMigration(FinalizeParams);
	TestTrue(TEXT("Finalize-only succeeds"), Finalized.bSuccess);
	if (Finalized.bSuccess && TestTrue(TEXT("Finalize data"), Finalized.Data.IsValid()))
	{
		TestEqual(TEXT("Finalize compiled"), Finalized.Data->GetStringField(TEXT("compile_status")), FString(TEXT("success")));
		TestEqual(TEXT("Finalize readback matched"), Finalized.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		const TSharedPtr<FJsonObject> Readback = ReadNamedEntry(Finalized.Data, TEXT("cdo_readback"), TEXT("bPowerOn"));
		if (TestTrue(TEXT("Finalize readback reported"), Readback.IsValid()))
		{
			TestTrue(TEXT("Finalize value preserved"), Readback->GetBoolField(TEXT("preserved")));
		}
		TestFalse(TEXT("Finalize does not save"), Finalized.Data->GetBoolField(TEXT("saved")));
	}
	const ACortexBPAdoptParent* FinalDefaults = AdoptDefaults(Blueprint);
	if (TestNotNull(TEXT("Finalized class CDO"), FinalDefaults))
	{
		// The delta flags make the new native parent's constructor default win (E5).
		TestEqual(TEXT("Finalize applies the native parent default"), FinalDefaults->InitialLifeSpan, 7.f);
		TestTrue(TEXT("Finalize keeps the adopted value"), FinalDefaults->bPowerOn);
	}

	// --- a mismatching expectation refuses and restores ---
	TSharedPtr<FJsonObject> WrongParams = CleanupParams(Blueprint);
	WrongParams->SetBoolField(TEXT("compile"), true);
	WrongParams->SetBoolField(TEXT("save"), false);
	{
		TSharedPtr<FJsonObject> Wrong = MakeShared<FJsonObject>();
		Wrong->SetStringField(TEXT("bPowerOn"), TEXT("False"));
		WrongParams->SetObjectField(TEXT("expected_cdo_values"), Wrong);
	}
	const FCortexCommandResult Mismatched = FCortexBPCleanupOps::CleanupMigration(WrongParams);
	TestFalse(TEXT("Mismatching expectation refuses"), Mismatched.bSuccess);
	TestEqual(TEXT("Mismatching expectation code"), Mismatched.ErrorCode, CortexErrorCodes::ReadbackMismatch);
	if (TestTrue(TEXT("Mismatch details"), Mismatched.ErrorDetails.IsValid()))
	{
		TestEqual(TEXT("Mismatch restored"), Mismatched.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
	}

	// --- save requires a compile ---
	TSharedPtr<FJsonObject> SaveWithoutCompile = CleanupParams(Blueprint);
	SaveWithoutCompile->SetBoolField(TEXT("compile"), false);
	SaveWithoutCompile->SetBoolField(TEXT("save"), true);
	const FCortexCommandResult RefusedSave = FCortexBPCleanupOps::CleanupMigration(SaveWithoutCompile);
	TestFalse(TEXT("Save without compile refuses"), RefusedSave.bSuccess);
	TestEqual(TEXT("Save without compile code"), RefusedSave.ErrorCode, CortexErrorCodes::InvalidOperation);

	// --- a failing finalize never saves; the repaired one saves and persists the adoption ---
	{
		AddDuplicateEventFault(Blueprint);
		const FCortexCommandResult Broken = FCortexBPCleanupOps::CleanupMigration(FinalizeParams);
		TestFalse(TEXT("Broken finalize fails"), Broken.bSuccess);
		TestEqual(TEXT("Broken finalize code"), Broken.ErrorCode, CortexErrorCodes::CompileFailed);
		if (TestTrue(TEXT("Broken finalize details"), Broken.ErrorDetails.IsValid()))
		{
			TestFalse(TEXT("Broken finalize never saves"), Broken.ErrorDetails->GetBoolField(TEXT("saved")));
		}
	}

	// The failed run blocked this asset (the pre-existing fault survives the restore, so the recovery compile cannot
	// prove the class). A fresh fixture proves the save path instead.
	RetireBlueprint(Blueprint);

	UBlueprint* Fresh = MakeBlueprint(TEXT("CleanupGuardedStagedSave"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Fresh Blueprint"), Fresh))
	{
		return false;
	}
	AddVariable(Fresh, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Fresh);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Fresh->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());

	TSharedPtr<FJsonObject> FreshStage = CleanupParams(Fresh);
	FreshStage->SetStringField(TEXT("new_parent_class"), Target);
	SetStrings(FreshStage, TEXT("remove_variables"), {TEXT("bPowerOn")});
	FreshStage->SetBoolField(TEXT("compile"), false);
	FreshStage->SetBoolField(TEXT("save"), false);
	const FCortexCommandResult FreshStaged = FCortexBPCleanupOps::CleanupMigration(FreshStage);
	if (!TestTrue(TEXT("Fresh stage succeeds"), FreshStaged.bSuccess && FreshStaged.Data.IsValid()))
	{
		IFileManager::Get().Delete(*Filename);
		RetireBlueprint(Fresh);
		return false;
	}

	TSharedPtr<FJsonObject> SaveParams = CleanupParams(Fresh);
	SaveParams->SetBoolField(TEXT("compile"), true);
	SaveParams->SetBoolField(TEXT("save"), true);
	SaveParams->SetObjectField(TEXT("expected_cdo_values"), ReadObject(FreshStaged.Data, TEXT("cdo_expectations")));
	const FCortexCommandResult Saved = FCortexBPCleanupOps::CleanupMigration(SaveParams);
	TestTrue(TEXT("Finalize with save succeeds"), Saved.bSuccess);
	if (Saved.bSuccess && TestTrue(TEXT("Save data"), Saved.Data.IsValid()))
	{
		TestTrue(TEXT("Save reported"), Saved.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("Save status"), Saved.Data->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
		TestFalse(TEXT("Saved package is clean"), Saved.Data->GetBoolField(TEXT("dirty_after")));
	}
	TArray<uint8> Bytes;
	TestTrue(TEXT("Saved package exists on disk"), FFileHelper::LoadFileToArray(Bytes, *Filename));
	if (const ACortexBPAdoptParent* SavedDefaults = AdoptDefaults(Fresh))
	{
		TestEqual(TEXT("Saved native default"), SavedDefaults->InitialLifeSpan, 7.f);
		TestTrue(TEXT("Saved adopted value"), SavedDefaults->bPowerOn);
	}
	IFileManager::Get().Delete(*Filename);
	RetireBlueprint(Fresh);

	return true;
}
