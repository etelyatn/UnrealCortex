#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexBPAdoptMemberOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPSnapshotOps.h"

namespace
{
	using namespace CortexAdoptMemberTest;

	TSet<FGuid> NodeGuids(const UBlueprint* Blueprint)
	{
		TSet<FGuid> Guids;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr)
				{
					Guids.Add(Node->NodeGuid);
				}
			}
		}
		return Guids;
	}

	/** Every variable node in the Blueprint resolves to a native property of that name. */
	bool AllVariableNodesResolveToNative(UBlueprint* Blueprint, const FName Name, int32& OutCount)
	{
		OutCount = 0;
		TArray<UK2Node_Variable*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
		for (UK2Node_Variable* Node : Nodes)
		{
			if (Node->GetVarName() != Name)
			{
				continue;
			}
			++OutCount;
			const FProperty* Resolved = Node->VariableReference.ResolveMember<FProperty>(Blueprint);
			if (Resolved == nullptr || Resolved->GetOwnerClass() == nullptr || !Resolved->GetOwnerClass()->HasAnyClassFlags(CLASS_Native))
			{
				return false;
			}
		}
		return OutCount > 0;
	}

	bool SnapshotsEqual(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Before)
	{
		FString Error;
		const TSharedPtr<FJsonObject> After = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
		if (!After.IsValid() || !Before.IsValid())
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(Before, After, TSet<FString>(), Differences, Counts);
		return Diff.bSuccess && Differences.IsEmpty();
	}

	TSharedPtr<FJsonObject> Snapshot(UBlueprint* Blueprint)
	{
		FString Error;
		return FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
	}

	bool IsCompileClean(const FCortexCommandResult& Result)
	{
		const FString Status = Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("compile_status")) : FString();
		return Status == TEXT("success") || Status == TEXT("warning");
	}

	struct FCompileFailureScope
	{
		FCompileFailureScope()
		{
			FCortexBPCompileSupport::SetCompileOutcomeOverrideForTest([](int32 Index, FCortexBPCompileOutcome& Outcome)
			{
				if (Index == 1)
				{
					Outcome.ErrorCount = 1;
					Outcome.Status = TEXT("error");
				}
			});
		}
		~FCompileFailureScope() { FCortexBPCompileSupport::ClearCompileOutcomeOverrideForTest(); }
	};

	struct FPostWriteHookScope
	{
		explicit FPostWriteHookScope(TFunction<void(UBlueprint*)> Hook) { FCortexBPAdoptMemberOps::SetPostWriteHookForTest(MoveTemp(Hook)); }
		~FPostWriteHookScope() { FCortexBPAdoptMemberOps::ClearPostWriteHookForTest(); }
	};

	struct FPostRestoreHookScope
	{
		explicit FPostRestoreHookScope(TFunction<void(UBlueprint*)> Hook) { FCortexBPGuardedRun::SetPostRestoreHookForTest(MoveTemp(Hook)); }
		~FPostRestoreHookScope() { FCortexBPGuardedRun::ClearPostRestoreHookForTest(); }
	};

	struct FPostApplyHookScope
	{
		explicit FPostApplyHookScope(TFunction<void(UBlueprint*)> Hook) { FCortexBPAdoptMemberOps::SetPostApplyHookForTest(MoveTemp(Hook)); }
		~FPostApplyHookScope() { FCortexBPAdoptMemberOps::ClearPostApplyHookForTest(); }
	};

	const FBoolProperty* BoolOn(const UObject* Object, const TCHAR* Name)
	{
		return Object != nullptr ? CastField<FBoolProperty>(FindFProperty<FProperty>(Object->GetClass(), Name)) : nullptr;
	}

	bool ReadBool(const UObject* Object, const TCHAR* Name)
	{
		const FBoolProperty* Property = BoolOn(Object, Name);
		return Property != nullptr && Property->GetPropertyValue_InContainer(Object);
	}

	/** An object-based Blueprint with the single bool variable Name, compiled against UObject and then handed the native parent directly. */
	UBlueprint* MakeObjectStateA(const TCHAR* AssetName, const TCHAR* VariableName, const TCHAR* DefaultValue = TEXT("true"))
	{
		UBlueprint* Blueprint = CortexAdoptTest::MakeBlueprint(AssetName, UObject::StaticClass());
		if (Blueprint == nullptr)
		{
			return nullptr;
		}
		AddVariable(Blueprint, VariableName, BoolPin(), DefaultValue);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Blueprint->ParentClass = UCortexBPAdoptObjectParent::StaticClass();
		return Blueprint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberSameNameTest,
	"Cortex.Blueprint.AdoptMember.SameName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberSameNameTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	FStateAFixture Fixture = MakeStateA(TEXT("AdoptSameName"), false, false, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Chain"), Fixture.PowerGet != nullptr && Fixture.PowerSet != nullptr && Fixture.Event != nullptr))
	{
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	const TSet<FGuid> NodesBefore = NodeGuids(Blueprint);
	const FGuid GetGuid = Fixture.PowerGet->NodeGuid;
	const FGuid SetGuid = Fixture.PowerSet->NodeGuid;
	TestEqual(TEXT("Old CDO default"), ReadBool(Blueprint->GeneratedClass->GetDefaultObject(), TEXT("bPowerOn")), true);
	TestEqual(TEXT("Native default is false"), ACortexBPAdoptParent::StaticClass()->GetDefaultObject<ACortexBPAdoptParent>()->bPowerOn, false);

	// compile=false rejects value moves but a same-name transfer is allowed; use the real compile here.
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))}));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("adopted"), Result.Data->GetBoolField(TEXT("adopted")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("apply status"), Result.Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestTrue(TEXT("compile clean"), IsCompileClean(Result));
		TestEqual(TEXT("variable removed"), KindOf(FindDifference(Result.Data, TEXT("variables"), TEXT("bPowerOn"))), FString(TEXT("removed")));
		TestEqual(TEXT("CDO member moved only"), KindOf(FindDifference(Result.Data, TEXT("cdo"), TEXT("bPowerOn"))), FString(TEXT("moved")));

		const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (TestTrue(TEXT("referencing_nodes"), Power.IsValid() && Power->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr))
		{
			TestEqual(TEXT("Two nodes"), Nodes->Num(), 2);
			for (const TSharedPtr<FJsonValue>& Node : *Nodes)
			{
				TestTrue(TEXT("retargeted"), Node->AsObject()->GetBoolField(TEXT("retargeted")));
				TestTrue(TEXT("resolves_to_native"), Node->AsObject()->GetBoolField(TEXT("resolves_to_native")));
			}
		}
	}

	TestFalse(TEXT("The variable is gone"), HasVariable(Blueprint, TEXT("bPowerOn")));
	TestTrue(TEXT("No node was destroyed (node GUID set equal)"), NodesBefore.Num() == NodeGuids(Blueprint).Num() && NodesBefore.Includes(NodeGuids(Blueprint)));
	TestTrue(TEXT("get node survives"), HasNodeGuid(Blueprint, GetGuid));
	TestTrue(TEXT("set node survives"), HasNodeGuid(Blueprint, SetGuid));
	int32 ResolvedCount = 0;
	TestTrue(TEXT("Every node resolves to the native property"), AllVariableNodesResolveToNative(Blueprint, TEXT("bPowerOn"), ResolvedCount));
	TestEqual(TEXT("Both variable nodes resolved"), ResolvedCount, 2);
	TestTrue(TEXT("The event chain is intact"),
		Fixture.Event->FindPin(UEdGraphSchema_K2::PN_Then)->LinkedTo.Contains(Fixture.PowerSet->FindPin(UEdGraphSchema_K2::PN_Execute))
		&& Fixture.PowerGet->GetValuePin()->LinkedTo.Contains(Fixture.PowerSet->FindPin(TEXT("bPowerOn"))));
	TestTrue(TEXT("Blueprint compiled"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	if (const ACortexBPAdoptParent* Defaults = Cast<ACortexBPAdoptParent>(Blueprint->GeneratedClass->GetDefaultObject()))
	{
		TestTrue(TEXT("The CDO keeps the Blueprint default (true) over the native default (false)"), Defaults->bPowerOn);
	}
	else
	{
		AddError(TEXT("The generated class does not derive from ACortexBPAdoptParent"));
	}
	TestTrue(TEXT("Property lives on the native class"), Blueprint->GeneratedClass->FindPropertyByName(TEXT("bPowerOn")) != nullptr
		&& Blueprint->GeneratedClass->FindPropertyByName(TEXT("bPowerOn"))->GetOwnerClass() == ACortexBPAdoptParent::StaticClass());

	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberTransferDefaultFalseTest,
	"Cortex.Blueprint.AdoptMember.TransferDefaultFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberTransferDefaultFalseTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	// The plan requires the instance behaviour here too: instance B equalled the old default (true) and must follow the new
	// CDO (the parent default, false) without becoming a persisted override; instance A already differed and keeps false.
	UBlueprint* Blueprint = MakeObjectStateA(TEXT("AdoptTransferFalse"), TEXT("bPowerOn"));
	if (!TestNotNull(TEXT("Fixture"), Blueprint))
	{
		return false;
	}
	UClass* OldClass = Blueprint->GeneratedClass;
	UObject* Overriding = NewObject<UObject>(GetTransientPackage(), OldClass, TEXT("AdoptTransferFalseOverriding"));
	UObject* Following = NewObject<UObject>(GetTransientPackage(), OldClass, TEXT("AdoptTransferFalseFollowing"));
	Overriding->SetFlags(RF_Standalone);
	Following->SetFlags(RF_Standalone);
	const FString OverridingPath = Overriding->GetPathName();
	const FString FollowingPath = Following->GetPathName();
	if (const FBoolProperty* Own = BoolOn(Overriding, TEXT("bPowerOn")))
	{
		Own->SetPropertyValue_InContainer(Overriding, false);
	}
	TestTrue(TEXT("The following instance starts at the old default"), ReadBool(Following, TEXT("bPowerOn")));

	TSharedPtr<FJsonObject> Params = AdoptParams(Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
	Params->SetBoolField(TEXT("transfer_default"), false);
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestFalse(TEXT("The response says the default was not transferred"), Result.Data->GetBoolField(TEXT("transfer_default")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
	}
	UObject* CdoObject = Blueprint->GeneratedClass->GetDefaultObject();
	TestFalse(TEXT("The CDO takes the parent default (false)"), ReadBool(CdoObject, TEXT("bPowerOn")));
	UObject* OverridingAfter = FindObject<UObject>(nullptr, *OverridingPath);
	UObject* FollowingAfter = FindObject<UObject>(nullptr, *FollowingPath);
	if (TestNotNull(TEXT("Overriding instance"), OverridingAfter) && TestNotNull(TEXT("Following instance"), FollowingAfter))
	{
		TestFalse(TEXT("The differing instance keeps its value"), ReadBool(OverridingAfter, TEXT("bPowerOn")));
		TestFalse(TEXT("The instance that equalled the old default follows the new CDO"), ReadBool(FollowingAfter, TEXT("bPowerOn")));
		const FProperty* Property = FindFProperty<FProperty>(FollowingAfter->GetClass(), TEXT("bPowerOn"));
		TestTrue(TEXT("It is not an override (identical to the CDO)"), Property != nullptr && Property->Identical_InContainer(FollowingAfter, CdoObject));
	}
	TestFalse(TEXT("The variable is gone"), HasVariable(Blueprint, TEXT("bPowerOn")));
	CortexAdoptTest::ResetUndoHistory();
	if (OverridingAfter != nullptr) { OverridingAfter->MarkAsGarbage(); }
	if (FollowingAfter != nullptr) { FollowingAfter->MarkAsGarbage(); }
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberEngineAlreadyAdoptedTest,
	"Cortex.Blueprint.AdoptMember.EngineAlreadyAdopted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberEngineAlreadyAdoptedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	ExpectConformanceWarnings(*this);

	FStateAFixture Fixture = MakeStateA(TEXT("AdoptConformed"), false, false, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}
	// The editor conformed and compiled the Blueprint (E1): the variable is already gone, the nodes still name it.
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	if (!TestFalse(TEXT("The engine already removed bPowerOn"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn"))))
	{
		Discard(Fixture.Blueprint);
		return false;
	}
	const TSet<FGuid> NodesBefore = NodeGuids(Fixture.Blueprint);

	TSharedRef<FJsonObject> PowerMapping = Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"));
	PowerMapping->SetBoolField(TEXT("default"), true);

	// A supplied default is written by the compile-time step, so compile=false must refuse instead of dropping it.
	{
		TSharedPtr<FJsonObject> NoCompile = AdoptParams(Fixture.Blueprint, {PowerMapping});
		NoCompile->SetBoolField(TEXT("compile"), false);
		const FCortexCommandResult Refused = FCortexBPAdoptMemberOps::Execute(NoCompile);
		TestFalse(TEXT("compile=false cannot silently drop a mapping default"), Refused.bSuccess);
		TestEqual(TEXT("Refusal code"), Refused.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("Nothing was adopted"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn")) == false);
	}
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {PowerMapping}));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
		TestEqual(TEXT("state"), StringOf(Power, TEXT("state")), FString(TEXT("engine_already_adopted")));
		TestFalse(TEXT("No variable removal is reported for an already adopted member"), FindDifference(Result.Data, TEXT("variables"), TEXT("bPowerOn")).IsValid());
	}
	TestTrue(TEXT("No node was destroyed"), NodesBefore.Includes(NodeGuids(Fixture.Blueprint)) && NodeGuids(Fixture.Blueprint).Num() == NodesBefore.Num());
	int32 Count = 0;
	TestTrue(TEXT("Every node resolves to the native property"), AllVariableNodesResolveToNative(Fixture.Blueprint, TEXT("bPowerOn"), Count));
	TestTrue(TEXT("Blueprint compiled"), Fixture.Blueprint->Status == BS_UpToDate || Fixture.Blueprint->Status == BS_UpToDateWithWarnings);
	if (const ACortexBPAdoptParent* Defaults = Cast<ACortexBPAdoptParent>(Fixture.Blueprint->GeneratedClass->GetDefaultObject()))
	{
		TestTrue(TEXT("The supplied default (true) is on the CDO"), Defaults->bPowerOn);
	}

	// A forced divergence after the writes fails closed and restores.
	FStateAFixture Second = MakeStateA(TEXT("AdoptConformedFault"), false, false, false);
	if (TestNotNull(TEXT("Second fixture"), Second.Blueprint))
	{
		FKismetEditorUtilities::CompileBlueprint(Second.Blueprint);
		const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Second.Blueprint);
		const FPostWriteHookScope Hook([](UBlueprint* Target)
		{
			if (ACortexBPAdoptParent* Cdo = Cast<ACortexBPAdoptParent>(Target->GeneratedClass->GetDefaultObject()))
			{
				Cdo->bPowerOn = !Cdo->bPowerOn;
			}
		});
		const FCortexCommandResult Failed = FCortexBPAdoptMemberOps::Execute(AdoptParams(Second.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))}));
		AddInfo(Explain(Failed));
		TestFalse(TEXT("A diverging CDO fails"), Failed.bSuccess);
		TestEqual(TEXT("READBACK_MISMATCH"), Failed.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		TestEqual(TEXT("rollback restored"), StringOf(Failed.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestTrue(TEXT("Snapshot equal after the rollback"), SnapshotsEqual(Second.Blueprint, SnapshotBefore));
		Discard(Second.Blueprint);
	}
	Discard(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberInstancesSurviveTest,
	"Cortex.Blueprint.AdoptMember.InstancesSurvive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberInstancesSurviveTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	// Same name: bPowerOn (Blueprint default true, native default false).
	{
		UBlueprint* Blueprint = MakeObjectStateA(TEXT("AdoptInstancesSame"), TEXT("bPowerOn"));
		if (!TestNotNull(TEXT("Same-name fixture"), Blueprint))
		{
			return false;
		}
		UBlueprint* Child = MakeBlueprint(TEXT("AdoptInstancesSameChild"), Blueprint->GeneratedClass);
		UClass* OldClass = Blueprint->GeneratedClass;
		UObject* Overriding = NewObject<UObject>(GetTransientPackage(), OldClass, TEXT("AdoptSameOverriding"));
		UObject* Following = NewObject<UObject>(GetTransientPackage(), OldClass, TEXT("AdoptSameFollowing"));
		// Standalone survives the reinstancing (the replacement copies the flag) and the garbage collection that follows the compile.
		Overriding->SetFlags(RF_Standalone);
		Following->SetFlags(RF_Standalone);
		const FString OverridingPath = Overriding->GetPathName();
		const FString FollowingPath = Following->GetPathName();
		const FString ChildCdoPath = Child != nullptr && Child->GeneratedClass != nullptr ? Child->GeneratedClass->GetDefaultObject()->GetPathName() : FString();
		const FBoolProperty* Own = BoolOn(Overriding, TEXT("bPowerOn"));
		if (TestNotNull(TEXT("Own property"), Own))
		{
			Own->SetPropertyValue_InContainer(Overriding, false);
		}
		TestTrue(TEXT("The following instance starts at the old default"), ReadBool(Following, TEXT("bPowerOn")));

		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))}));
		if (!Result.bSuccess)
		{
			AddInfo(Explain(Result));
		}
		if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess))
		{
			UObject* After = FindObject<UObject>(nullptr, *OverridingPath);
			UObject* FollowAfter = FindObject<UObject>(nullptr, *FollowingPath);
			UObject* ChildCdo = ChildCdoPath.IsEmpty() ? nullptr : FindObject<UObject>(nullptr, *ChildCdoPath);
			UObject* Cdo = Blueprint->GeneratedClass->GetDefaultObject();
			if (TestNotNull(TEXT("Overriding instance found"), After) && TestNotNull(TEXT("Following instance found"), FollowAfter))
			{
				TestFalse(TEXT("The override keeps its value (false)"), ReadBool(After, TEXT("bPowerOn")));
				TestTrue(TEXT("The following instance equals the CDO (true, the transferred default)"), ReadBool(FollowAfter, TEXT("bPowerOn")));
				const FProperty* Property = FindFProperty<FProperty>(FollowAfter->GetClass(), TEXT("bPowerOn"));
				TestTrue(TEXT("The following instance is identical to the CDO (not an override)"), Property != nullptr && Property->Identical_InContainer(FollowAfter, Cdo));
			}
			TestTrue(TEXT("The CDO keeps true"), ReadBool(Cdo, TEXT("bPowerOn")));
			if (TestNotNull(TEXT("Child CDO found"), ChildCdo))
			{
				TestTrue(TEXT("The child Blueprint CDO equalled the old default and gets the transferred default"), ReadBool(ChildCdo, TEXT("bPowerOn")));
			}
		}
		CortexAdoptTest::ResetUndoHistory();
		if (UObject* Found = FindObject<UObject>(nullptr, *OverridingPath)) { Found->MarkAsGarbage(); }
		if (UObject* Found = FindObject<UObject>(nullptr, *FollowingPath)) { Found->MarkAsGarbage(); }
		Discard(Child);
		Discard(Blueprint);
	}

	// Renamed: bOpen (Blueprint default false) -> native bIsOpen (default false). The overriding instance is true, so it
	// differs from the old default, the native default and the new CDO: only an explicit move can produce it after the rename.
	const auto RunRenamedCase = [this](const TCHAR* AssetName, const bool bRedirect, const bool bAcknowledge)
	{
		UBlueprint* Blueprint = MakeObjectStateA(AssetName, TEXT("bOpen"), TEXT("false"));
		if (!TestNotNull(TEXT("Rename fixture"), Blueprint))
		{
			return;
		}
		UClass* OldClass = Blueprint->GeneratedClass;
		UObject* Overriding = NewObject<UObject>(GetTransientPackage(), OldClass, *FString::Printf(TEXT("%sOverriding"), AssetName));
		UObject* Following = NewObject<UObject>(GetTransientPackage(), OldClass, *FString::Printf(TEXT("%sFollowing"), AssetName));
		Overriding->SetFlags(RF_Standalone);
		Following->SetFlags(RF_Standalone);
		const FString OverridingPath = Overriding->GetPathName();
		const FString FollowingPath = Following->GetPathName();
		if (const FBoolProperty* Own = BoolOn(Overriding, TEXT("bOpen")))
		{
			Own->SetPropertyValue_InContainer(Overriding, true);
		}
		TestFalse(TEXT("The following instance starts at the old default"), ReadBool(Following, TEXT("bOpen")));

		TOptional<FRedirectScope> Redirect;
		if (bRedirect)
		{
			Redirect.Emplace(UCortexBPAdoptObjectParent::StaticClass(), TEXT("bOpen"), TEXT("bIsOpen"));
		}
		TSharedPtr<FJsonObject> Params = AdoptParams(Blueprint, {Mapping(TEXT("bOpen"), TEXT("bIsOpen"))});
		if (bAcknowledge)
		{
			SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("instance_values:bOpen")});
		}
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(Params);
		if (!Result.bSuccess)
		{
			AddInfo(Explain(Result));
		}
		if (TestTrue(TEXT("Renamed adoption succeeds"), Result.bSuccess))
		{
			UObject* After = FindObject<UObject>(nullptr, *OverridingPath);
			UObject* FollowAfter = FindObject<UObject>(nullptr, *FollowingPath);
			UObject* Cdo = Blueprint->GeneratedClass->GetDefaultObject();
			if (TestNotNull(TEXT("Overriding instance found"), After) && TestNotNull(TEXT("Following instance found"), FollowAfter))
			{
				TestTrue(TEXT("The override value moved to the new property (true, neither the native default nor the new CDO)"), ReadBool(After, TEXT("bIsOpen")));
				TestFalse(TEXT("The following instance equals the CDO (the old default, false)"), ReadBool(FollowAfter, TEXT("bIsOpen")));
				const FProperty* Property = FindFProperty<FProperty>(FollowAfter->GetClass(), TEXT("bIsOpen"));
				TestTrue(TEXT("It is not an override"), Property != nullptr && Property->Identical_InContainer(FollowAfter, Cdo));
			}
			TestFalse(TEXT("The CDO carries the old default on the new property"), ReadBool(Cdo, TEXT("bIsOpen")));
			TestTrue(TEXT("The old property is gone from the class"), Blueprint->GeneratedClass->FindPropertyByName(TEXT("bOpen")) == nullptr);
		}
		CortexAdoptTest::ResetUndoHistory();
		if (UObject* Found = FindObject<UObject>(nullptr, *OverridingPath)) { Found->MarkAsGarbage(); }
		if (UObject* Found = FindObject<UObject>(nullptr, *FollowingPath)) { Found->MarkAsGarbage(); }
		Discard(Blueprint);
	};

	// With a property redirect, and on the acknowledged path without one (where the explicit move is the only mechanism).
	RunRenamedCase(TEXT("AdoptInstancesRename"), /*bRedirect=*/true, /*bAcknowledge=*/false);
	RunRenamedCase(TEXT("AdoptInstancesRenameAck"), /*bRedirect=*/false, /*bAcknowledge=*/true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberNumericAndRollbackTest,
	"Cortex.Blueprint.AdoptMember.NumericAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberNumericAndRollbackTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	ExpectConformanceWarnings(*this);

	// double -> float converts and warns.
	{
		FStateAFixture Fixture = MakeStateA(TEXT("AdoptNumeric"), /*bSpeed=*/true, false, false);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
		{
			return false;
		}
		// bPowerOn collides with the parent too, so both are mapped.
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed"))}));
		if (!Result.bSuccess)
		{
			AddInfo(Explain(Result));
		}
		if (TestTrue(TEXT("Numeric adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			TestTrue(TEXT("The response warns about the conversion"), Result.Data->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr && Warnings->Num() > 0);
			TestEqual(TEXT("Speed type"), StringOf(FindMapping(Result, TEXT("Speed")), TEXT("type_match")), FString(TEXT("numeric_convertible")));
		}
		if (const ACortexBPAdoptParent* Defaults = Cast<ACortexBPAdoptParent>(Fixture.Blueprint->GeneratedClass->GetDefaultObject()))
		{
			TestEqual(TEXT("Speed converted to the native float"), Defaults->Speed, 2.5f);
		}
		TestFalse(TEXT("No Speed_0 rename"), HasVariable(Fixture.Blueprint, TEXT("Speed_0")));
		int32 Count = 0;
		TestTrue(TEXT("Speed nodes resolve to the native property"), AllVariableNodesResolveToNative(Fixture.Blueprint, TEXT("Speed"), Count));
		Discard(Fixture.Blueprint);
	}

	// A forced compile failure restores the Blueprint.
	{
		FStateAFixture Fixture = MakeStateA(TEXT("AdoptRollbackCompile"), true, false, false);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Fixture.Blueprint);
		const FStateDigest Before = FStateDigest::Of(Fixture.Blueprint);
		const FCompileFailureScope Failure;
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed"))}));
		AddInfo(Explain(Result));
		TestFalse(TEXT("A forced compile failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("COMPILE_FAILED"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		TestEqual(TEXT("rollback_status"), StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("Never reported adopted"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("adopted")));
		TestTrue(TEXT("Snapshot equal to the pre-call snapshot"), SnapshotsEqual(Fixture.Blueprint, SnapshotBefore));
		TestTrue(TEXT("Variables, graphs and fingerprint equal"), Before == FStateDigest::Of(Fixture.Blueprint));
		TestTrue(TEXT("bPowerOn variable is back"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn")));
		TestTrue(TEXT("Speed variable is back"), HasVariable(Fixture.Blueprint, TEXT("Speed")));
		FString Reason;
		TestFalse(TEXT("Not blocked"), FCortexAssetMutationGuard::IsBlocked(Fixture.Blueprint, Reason));
		Discard(Fixture.Blueprint);
	}

	// A forced readback mismatch restores the Blueprint and keeps the instance values of a renamed member.
	{
		UBlueprint* Blueprint = MakeObjectStateA(TEXT("AdoptRollbackReadback"), TEXT("bOpen"));
		if (!TestNotNull(TEXT("Object fixture"), Blueprint))
		{
			return false;
		}
		UObject* Overriding = NewObject<UObject>(GetTransientPackage(), Blueprint->GeneratedClass, TEXT("AdoptRollbackOverriding"));
		UObject* Following = NewObject<UObject>(GetTransientPackage(), Blueprint->GeneratedClass, TEXT("AdoptRollbackFollowing"));
		Overriding->SetFlags(RF_Standalone);
		Following->SetFlags(RF_Standalone);
		const FString OverridingPath = Overriding->GetPathName();
		const FString FollowingPath = Following->GetPathName();
		if (const FBoolProperty* Own = BoolOn(Overriding, TEXT("bOpen")))
		{
			Own->SetPropertyValue_InContainer(Overriding, false);
		}
		const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Blueprint);
		const FStateDigest Before = FStateDigest::Of(Blueprint);

		FRedirectScope Redirect(UCortexBPAdoptObjectParent::StaticClass(), TEXT("bOpen"), TEXT("bIsOpen"));
		{
			const FPostWriteHookScope Hook([](UBlueprint* Target)
			{
				if (UCortexBPAdoptObjectParent* Cdo = Cast<UCortexBPAdoptObjectParent>(Target->GeneratedClass->GetDefaultObject()))
				{
					Cdo->bIsOpen = !Cdo->bIsOpen;
				}
			});
			const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Blueprint, {Mapping(TEXT("bOpen"), TEXT("bIsOpen"))}));
			AddInfo(Explain(Result));
			TestFalse(TEXT("A forced readback mismatch fails the call"), Result.bSuccess);
			TestEqual(TEXT("READBACK_MISMATCH"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
			TestEqual(TEXT("rollback_status"), StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		}
		TestTrue(TEXT("Snapshot equal to the pre-call snapshot"), SnapshotsEqual(Blueprint, SnapshotBefore));
		TestTrue(TEXT("Variables and fingerprint equal"), Before == FStateDigest::Of(Blueprint));
		UObject* After = FindObject<UObject>(nullptr, *OverridingPath);
		UObject* FollowAfter = FindObject<UObject>(nullptr, *FollowingPath);
		if (TestNotNull(TEXT("Overriding instance"), After) && TestNotNull(TEXT("Following instance"), FollowAfter))
		{
			TestFalse(TEXT("The override value is kept under the old name"), ReadBool(After, TEXT("bOpen")));
			TestTrue(TEXT("The default instance keeps the old default"), ReadBool(FollowAfter, TEXT("bOpen")));
		}
		CortexAdoptTest::ResetUndoHistory();
		if (After != nullptr) { After->MarkAsGarbage(); }
		if (FollowAfter != nullptr) { FollowAfter->MarkAsGarbage(); }
		Discard(Blueprint);
	}

	// A restore that cannot be verified blocks the asset.
	{
		FStateAFixture Fixture = MakeStateA(TEXT("AdoptRollbackBlocked"), false, false, false);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
		{
			return false;
		}
		const FCompileFailureScope Failure;
		const FPostRestoreHookScope Hook([](UBlueprint* Target)
		{
			FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("CorruptedRestore"), BoolPin());
		});
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))}));
		AddInfo(Explain(Result));
		TestFalse(TEXT("The call fails"), Result.bSuccess);
		TestEqual(TEXT("rollback_status unverified"), StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("unverified")));
		FString Reason;
		TestTrue(TEXT("The asset is blocked"), FCortexAssetMutationGuard::IsBlocked(Fixture.Blueprint, Reason));
		Discard(Fixture.Blueprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberSaveGameAcknowledgmentTest,
	"Cortex.Blueprint.AdoptMember.SaveGameAcknowledgment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberSaveGameAcknowledgmentTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	// bNotSave: a SaveGame Blueprint variable whose native property is not SaveGame (flag missing).
	UBlueprint* Blueprint = MakeBlueprint(TEXT("AdoptSaveGame"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bNotSave"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Blueprint->ParentClass = ACortexBPAdoptParent::StaticClass();
	const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Blueprint);

	TSharedPtr<FJsonObject> DryParams = AdoptParams(Blueprint, {Mapping(TEXT("bNotSave"), TEXT("bNotSave"))});
	DryParams->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Dry = FCortexBPAdoptMemberOps::Execute(DryParams);
	if (TestTrue(TEXT("The dry run lists the finding"), Dry.bSuccess) && Dry.Data.IsValid())
	{
		const TArray<FString> Required = ReadStrings(Dry.Data, TEXT("required_acknowledgment"));
		TestTrue(TEXT("required_acknowledgment is exactly save_game:bNotSave"), Required.Num() == 1 && Required[0] == TEXT("save_game:bNotSave"));
		TestFalse(TEXT("can_apply is false until acknowledged"), Dry.Data->GetBoolField(TEXT("can_apply")));
		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		const TSharedPtr<FJsonObject> Mapping_ = FindMapping(Dry, TEXT("bNotSave"));
		TestTrue(TEXT("save_game_findings listed"), Mapping_.IsValid() && Mapping_->TryGetArrayField(TEXT("save_game_findings"), Findings) && Findings != nullptr && Findings->Num() == 1);
	}

	const FCortexCommandResult Refused = FCortexBPAdoptMemberOps::Execute(AdoptParams(Blueprint, {Mapping(TEXT("bNotSave"), TEXT("bNotSave"))}));
	TestFalse(TEXT("Apply refuses without the acknowledgment"), Refused.bSuccess);
	TestEqual(TEXT("POTENTIAL_DATA_LOSS"), Refused.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	TestTrue(TEXT("Nothing changed"), SnapshotsEqual(Blueprint, SnapshotBefore) && HasVariable(Blueprint, TEXT("bNotSave")));

	TSharedPtr<FJsonObject> Stale = AdoptParams(Blueprint, {Mapping(TEXT("bNotSave"), TEXT("bNotSave"))});
	SetStrings(Stale, TEXT("acknowledged_losses"), {TEXT("save_game:bNotSave"), TEXT("save_game:Other")});
	const FCortexCommandResult StaleResult = FCortexBPAdoptMemberOps::Execute(Stale);
	TestFalse(TEXT("An extra key refuses"), StaleResult.bSuccess);
	TestEqual(TEXT("Stale code"), StaleResult.ErrorCode, CortexErrorCodes::PotentialDataLoss);

	TSharedPtr<FJsonObject> Acknowledged = AdoptParams(Blueprint, {Mapping(TEXT("bNotSave"), TEXT("bNotSave"))});
	SetStrings(Acknowledged, TEXT("acknowledged_losses"), {TEXT("save_game:bNotSave")});
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(Acknowledged);
	TestTrue(TEXT("The exact acknowledgment succeeds"), Result.bSuccess);
	TestFalse(TEXT("The variable is gone"), HasVariable(Blueprint, TEXT("bNotSave")));
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberRefusalsTest,
	"Cortex.Blueprint.AdoptMember.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Blueprint = MakeObjectStateA(TEXT("AdoptRefusals"), TEXT("bOpen"));
	if (!TestNotNull(TEXT("Fixture"), Blueprint))
	{
		return false;
	}
	FRedirectScope Redirect(UCortexBPAdoptObjectParent::StaticClass(), TEXT("bOpen"), TEXT("bIsOpen"));
	const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Blueprint);
	const FStateDigest Before = FStateDigest::Of(Blueprint);

	// M3: a FieldNotify variable cannot be renamed by adoption.
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("bOpen"));
	Blueprint->NewVariables[Index].SetMetaData(FBlueprintMetadata::MD_FieldNotify, FString());
	{
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Blueprint, {Mapping(TEXT("bOpen"), TEXT("bIsOpen"))}));
		TestFalse(TEXT("A FieldNotify rename refuses"), Result.bSuccess);
		TestEqual(TEXT("FieldNotify code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("It names FieldNotify"), Result.ErrorMessage.Contains(TEXT("FieldNotify")));
	}
	Blueprint->NewVariables[Index].RemoveMetaData(FBlueprintMetadata::MD_FieldNotify);

	{
		TSharedPtr<FJsonObject> Params = AdoptParams(Blueprint, {Mapping(TEXT("bOpen"), TEXT("bIsOpen"))});
		Params->SetBoolField(TEXT("compile"), false);
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(Params);
		TestFalse(TEXT("compile=false with a rename refuses"), Result.bSuccess);
		TestEqual(TEXT("compile=false code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}
	TestTrue(TEXT("Refusals left the Blueprint untouched"), SnapshotsEqual(Blueprint, SnapshotBefore) && Before == FStateDigest::Of(Blueprint));

	{
		FStateAFixture Chain = MakeStateA(TEXT("AdoptRefusalsNodes"), false, false, false);
		if (TestNotNull(TEXT("Node fixture"), Chain.Blueprint))
		{
			// A variable named bOpen referenced by nodes cannot be left behind when it is renamed.
			Chain.Blueprint->ParentClass = AActor::StaticClass();
			AddVariable(Chain.Blueprint, TEXT("bOpen"), BoolPin(), TEXT("true"));
			AddVariableGet(Chain.Blueprint, TEXT("bOpen"));
			FKismetEditorUtilities::CompileBlueprint(Chain.Blueprint);
			Chain.Blueprint->ParentClass = ACortexBPAdoptParent::StaticClass();
			TSharedPtr<FJsonObject> NodeParams = AdoptParams(Chain.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("bOpen"), TEXT("bIsOpen"))});
			NodeParams->SetBoolField(TEXT("retarget_nodes"), false);
			SetStrings(NodeParams, TEXT("acknowledged_losses"), {TEXT("instance_values:bOpen")});
			const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(NodeParams);
			TestFalse(TEXT("retarget_nodes=false with a rename and referencing nodes refuses"), Result.bSuccess);
			TestEqual(TEXT("retarget_nodes code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
			Discard(Chain.Blueprint);
		}
	}
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberUndoNotRecordedTest,
	"Cortex.Blueprint.AdoptMember.UndoNotRecorded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberUndoNotRecordedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		AddInfo(TEXT("No editor transaction buffer in this context"));
		return true;
	}
	FStateAFixture Fixture = MakeStateA(TEXT("AdoptUndoNotRecorded"), false, false, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}

	// State A cannot be rebuilt by the editor undo (the compile conforms the variable away), so the adoption must not
	// record an undo entry at all - not only stay out of the undo on failure.
	const FGuid UndoBefore = GEditor->Trans->GetUndoContext(false).TransactionId;
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))}));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	TestTrue(TEXT("Adoption succeeds"), Result.bSuccess);
	TestEqual(TEXT("No undo entry was recorded"), GEditor->Trans->GetUndoContext(false).TransactionId, UndoBefore);
	TestFalse(TEXT("No transaction left open"), GEditor->IsTransactionActive());
	Discard(Fixture.Blueprint);
	return true;
}
