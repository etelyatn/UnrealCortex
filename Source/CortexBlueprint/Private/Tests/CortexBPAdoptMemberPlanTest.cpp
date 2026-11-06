#include "Misc/AutomationTest.h"

#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexBPAdoptMemberOps.h"

namespace
{
	using namespace CortexAdoptMemberTest;

	FCortexCommandResult DryRun(UBlueprint* Blueprint, const TArray<TSharedRef<FJsonObject>>& Mappings)
	{
		TSharedPtr<FJsonObject> Params = AdoptParams(Blueprint, Mappings);
		Params->SetBoolField(TEXT("dry_run"), true);
		return FCortexBPAdoptMemberOps::Execute(Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberPlanTest,
	"Cortex.Blueprint.AdoptMember.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberPlanTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	ExpectConformanceWarnings(*this);

	// State A without the incompatible variable: the whole plan is valid.
	FStateAFixture Clean = MakeStateA(TEXT("AdoptPlanClean"), /*bSpeed=*/true, /*bSavedName=*/true, /*bTypeMismatch=*/false);
	if (!TestNotNull(TEXT("Clean fixture"), Clean.Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("State A keeps bPowerOn in NewVariables"), HasVariable(Clean.Blueprint, TEXT("bPowerOn")));
	TestTrue(TEXT("Chain nodes exist"), Clean.PowerGet != nullptr && Clean.PowerSet != nullptr && Clean.Event != nullptr);

	{
		const FCortexCommandResult Result = DryRun(Clean.Blueprint,
			{Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed")), Mapping(TEXT("SavedName"), TEXT("SavedName"))});
		if (TestTrue(TEXT("Clean dry run succeeds"), Result.bSuccess) && Result.Data.IsValid())
		{
			TestEqual(TEXT("adopted false on dry run"), Result.Data->GetBoolField(TEXT("adopted")), false);
			TestTrue(TEXT("can_apply"), Result.Data->GetBoolField(TEXT("can_apply")));
			TestTrue(TEXT("fingerprint reported"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint_before")));

			const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
			const TSharedPtr<FJsonObject> Speed = FindMapping(Result, TEXT("Speed"));
			const TSharedPtr<FJsonObject> Saved = FindMapping(Result, TEXT("SavedName"));
			if (TestTrue(TEXT("bPowerOn mapping"), Power.IsValid()) && TestTrue(TEXT("Speed mapping"), Speed.IsValid()) && TestTrue(TEXT("SavedName mapping"), Saved.IsValid()))
			{
				TestEqual(TEXT("bPowerOn state"), StringOf(Power, TEXT("state")), FString(TEXT("variable_present")));
				TestEqual(TEXT("bPowerOn type"), StringOf(Power, TEXT("type_match")), FString(TEXT("exact")));
				TestEqual(TEXT("bPowerOn old default is read from the compiled CDO"), StringOf(Power, TEXT("old_default")), FString(TEXT("true")));
				TestEqual(TEXT("bPowerOn old default source"), StringOf(Power, TEXT("old_default_source")), FString(TEXT("compiled_cdo")));
				TestEqual(TEXT("SavedName type"), StringOf(Saved, TEXT("type_match")), FString(TEXT("exact")));
				TestEqual(TEXT("SavedName old default"), StringOf(Saved, TEXT("old_default")), FString(TEXT("abc")));
				TestEqual(TEXT("Speed state"), StringOf(Speed, TEXT("state")), FString(TEXT("variable_present")));
				TestEqual(TEXT("Speed type"), StringOf(Speed, TEXT("type_match")), FString(TEXT("numeric_convertible")));
				TestEqual(TEXT("Speed old default"), StringOf(Speed, TEXT("old_default")), FString(TEXT("2.5")));

				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (TestTrue(TEXT("bPowerOn node list"), Power->TryGetArrayField(TEXT("referencing_nodes"), Nodes)) && Nodes != nullptr)
				{
					TestEqual(TEXT("A get and a set node name bPowerOn"), Nodes->Num(), 2);
					for (const TSharedPtr<FJsonValue>& Node : *Nodes)
					{
						const TSharedPtr<FJsonObject>& NodeObject = Node->AsObject();
						TestTrue(TEXT("self_context reported"), NodeObject->HasTypedField<EJson::Boolean>(TEXT("self_context")));
						TestTrue(TEXT("node follows the member"), NodeObject->GetBoolField(TEXT("follows_member")));
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
				TestTrue(TEXT("Speed warns about the numeric conversion"), Speed->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr && Warnings->Num() > 0);
			}
			const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
			TestTrue(TEXT("required_acknowledgment present and empty (native keeps name, type and SaveGame)"),
				Result.Data->TryGetArrayField(TEXT("required_acknowledgment"), Required) && Required != nullptr && Required->IsEmpty());
		}
	}
	Discard(Clean.Blueprint);

	// The incompatible variable refuses; the rest of the plan stays visible in the refusal.
	FStateAFixture Mixed = MakeStateA(TEXT("AdoptPlanMixed"), true, true, /*bTypeMismatch=*/true);
	if (!TestNotNull(TEXT("Mixed fixture"), Mixed.Blueprint))
	{
		return false;
	}
	{
		const FCortexCommandResult Result = DryRun(Mixed.Blueprint,
			{Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed")), Mapping(TEXT("SavedName"), TEXT("SavedName")), Mapping(TEXT("TypeMismatch"), TEXT("TypeMismatch"))});
		TestFalse(TEXT("TypeMismatch refuses"), Result.bSuccess);
		TestEqual(TEXT("INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("The reason names the types"), Result.ErrorMessage.Contains(TEXT("TypeMismatch")) && Result.ErrorMessage.Contains(TEXT("string")));
		const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
		TestEqual(TEXT("Plan details still list bPowerOn"), StringOf(Power, TEXT("type_match")), FString(TEXT("exact")));
	}
	Discard(Mixed.Blueprint);

	// The same fixture after the engine conformed it: bPowerOn and SavedName were adopted by the engine, Speed and TypeMismatch were renamed.
	FStateAFixture Conformed = MakeStateA(TEXT("AdoptPlanConformed"), true, true, true);
	if (!TestNotNull(TEXT("Conformed fixture"), Conformed.Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Conformed.Blueprint);
	TestFalse(TEXT("The engine removed bPowerOn (E1)"), HasVariable(Conformed.Blueprint, TEXT("bPowerOn")));
	TestFalse(TEXT("The engine removed SavedName (E1)"), HasVariable(Conformed.Blueprint, TEXT("SavedName")));
	TestTrue(TEXT("The engine renamed Speed"), HasVariable(Conformed.Blueprint, TEXT("Speed_0")));
	TestTrue(TEXT("The engine renamed TypeMismatch"), HasVariable(Conformed.Blueprint, TEXT("TypeMismatch_0")));
	{
		const FCortexCommandResult Result = DryRun(Conformed.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("SavedName"), TEXT("SavedName"))});
		if (TestTrue(TEXT("engine_already_adopted dry run succeeds"), Result.bSuccess))
		{
			TestEqual(TEXT("bPowerOn state"), StringOf(FindMapping(Result, TEXT("bPowerOn")), TEXT("state")), FString(TEXT("engine_already_adopted")));
			TestEqual(TEXT("SavedName state"), StringOf(FindMapping(Result, TEXT("SavedName")), TEXT("state")), FString(TEXT("engine_already_adopted")));
			const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			TestTrue(TEXT("The nodes survived the engine's removal and are listed"),
				Power.IsValid() && Power->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr && Nodes->Num() == 2);
			TestFalse(TEXT("A conformed Blueprint that was never compiled cannot be applied"), Result.Data->GetBoolField(TEXT("can_apply")));
			const FCortexCommandResult Apply = FCortexBPAdoptMemberOps::Execute(AdoptParams(Conformed.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("SavedName"), TEXT("SavedName"))}));
			TestFalse(TEXT("Apply refuses it"), Apply.bSuccess);
			TestEqual(TEXT("Apply refusal code"), Apply.ErrorCode, CortexErrorCodes::StalePrecondition);
		}
		const FCortexCommandResult Speed = DryRun(Conformed.Blueprint, {Mapping(TEXT("Speed"), TEXT("Speed"))});
		TestFalse(TEXT("Engine-renamed Speed refuses"), Speed.bSuccess);
		TestEqual(TEXT("Speed refusal code"), Speed.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("Speed refusal names Speed_0"), Speed.ErrorMessage.Contains(TEXT("Speed_0")));
		const FCortexCommandResult Mismatch = DryRun(Conformed.Blueprint, {Mapping(TEXT("TypeMismatch"), TEXT("TypeMismatch"))});
		TestFalse(TEXT("Engine-renamed TypeMismatch refuses"), Mismatch.bSuccess);
		TestTrue(TEXT("TypeMismatch refusal names TypeMismatch_0"), Mismatch.ErrorMessage.Contains(TEXT("TypeMismatch_0")));
	}
	Discard(Conformed.Blueprint);

	// Dirty Blueprint, unknown native property, RepNotify variable, non-visible and unmapped collisions.
	FStateAFixture Dirty = MakeStateA(TEXT("AdoptPlanDirty"), false, false, false);
	if (!TestNotNull(TEXT("Dirty fixture"), Dirty.Blueprint))
	{
		return false;
	}
	{
		const FCortexCommandResult Unknown = DryRun(Dirty.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("NoSuchProperty"))});
		TestFalse(TEXT("Unknown native property refuses"), Unknown.bSuccess);
		TestEqual(TEXT("Unknown property code"), Unknown.ErrorCode, CortexErrorCodes::InvalidField);

		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Dirty.Blueprint, TEXT("bPowerOn"));
		Dirty.Blueprint->NewVariables[Index].RepNotifyFunc = TEXT("OnRep_PowerOn");
		const FCortexCommandResult RepNotify = DryRun(Dirty.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
		TestFalse(TEXT("RepNotify variable refuses"), RepNotify.bSuccess);
		TestEqual(TEXT("RepNotify code"), RepNotify.ErrorCode, CortexErrorCodes::InvalidField);
		Dirty.Blueprint->NewVariables[Index].RepNotifyFunc = NAME_None;

		const EBlueprintStatus StatusBefore = Dirty.Blueprint->Status;
		Dirty.Blueprint->Status = BS_Dirty;
		const FCortexCommandResult Stale = DryRun(Dirty.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
		TestFalse(TEXT("An uncompiled Blueprint refuses"), Stale.bSuccess);
		TestEqual(TEXT("Stale code"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		Dirty.Blueprint->Status = StatusBefore;
	}
	Discard(Dirty.Blueprint);

	FStateAFixture Unmapped = MakeStateA(TEXT("AdoptPlanUnmapped"), true, false, false);
	if (TestNotNull(TEXT("Unmapped fixture"), Unmapped.Blueprint))
	{
		const FCortexCommandResult Result = DryRun(Unmapped.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
		TestFalse(TEXT("An unmapped colliding variable refuses"), Result.bSuccess);
		TestEqual(TEXT("NAME_COLLISION"), Result.ErrorCode, CortexErrorCodes::NameCollision);
		TestTrue(TEXT("It names Speed"), Result.ErrorMessage.Contains(TEXT("Speed")));
		Discard(Unmapped.Blueprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberPlacedInstancesTest,
	"Cortex.Blueprint.AdoptMember.PlacedInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberPlacedInstancesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	FStateAFixture Fixture = MakeStateA(TEXT("AdoptPlanInstances"), false, false, false, UCortexBPAdoptObjectParent::StaticClass(), UObject::StaticClass(), /*bNodes=*/false);
	if (!TestNotNull(TEXT("Object-based fixture"), Fixture.Blueprint))
	{
		return false;
	}

	// Instances are created while the class is the compiled (pre-adoption) one.
	UClass* Generated = Fixture.Blueprint->GeneratedClass;
	UObject* Overriding = NewObject<UObject>(GetTransientPackage(), Generated, TEXT("AdoptPlanOverriding"));
	UObject* Following = NewObject<UObject>(GetTransientPackage(), Generated, TEXT("AdoptPlanFollowing"));
	const FBoolProperty* OwnProperty = CastField<FBoolProperty>(Generated->FindPropertyByName(TEXT("bPowerOn")));
	if (!TestNotNull(TEXT("Own property"), OwnProperty) || !TestNotNull(TEXT("Instances"), Overriding) || !TestNotNull(TEXT("Instances"), Following))
	{
		Discard(Fixture.Blueprint);
		return false;
	}
	OwnProperty->SetPropertyValue_InContainer(Overriding, false);
	const FStateDigest Before = FStateDigest::Of(Fixture.Blueprint);
	const bool bOverridingBefore = OwnProperty->GetPropertyValue_InContainer(Overriding);
	const bool bFollowingBefore = OwnProperty->GetPropertyValue_InContainer(Following);

	const FCortexCommandResult Result = DryRun(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
	if (TestTrue(TEXT("Dry run succeeds"), Result.bSuccess))
	{
		const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
		const TArray<TSharedPtr<FJsonValue>>* Loaded = nullptr;
		if (TestTrue(TEXT("loaded_instances listed"), Power.IsValid() && Power->TryGetArrayField(TEXT("loaded_instances"), Loaded) && Loaded != nullptr))
		{
			TestEqual(TEXT("Two live instances"), Loaded->Num(), 2);
			for (const TSharedPtr<FJsonValue>& Value : *Loaded)
			{
				const TSharedPtr<FJsonObject> Item = Value->AsObject();
				const FString Path = Item->GetStringField(TEXT("path"));
				if (Path == Overriding->GetPathName())
				{
					TestTrue(TEXT("The differing instance is an override"), Item->GetBoolField(TEXT("is_override")));
					TestEqual(TEXT("Its value"), Item->GetStringField(TEXT("value")), FString(TEXT("false")));
				}
				else if (Path == Following->GetPathName())
				{
					TestFalse(TEXT("The default instance is not an override"), Item->GetBoolField(TEXT("is_override")));
					TestEqual(TEXT("Its value"), Item->GetStringField(TEXT("value")), FString(TEXT("true")));
				}
				else
				{
					AddError(FString::Printf(TEXT("Unexpected loaded instance %s"), *Path));
				}
			}
		}
	}
	TestTrue(TEXT("Dry run changed nothing"), Before == FStateDigest::Of(Fixture.Blueprint));
	TestEqual(TEXT("Overriding instance untouched"), OwnProperty->GetPropertyValue_InContainer(Overriding), bOverridingBefore);
	TestEqual(TEXT("Following instance untouched"), OwnProperty->GetPropertyValue_InContainer(Following), bFollowingBefore);

	CortexAdoptTest::ResetUndoHistory();
	Overriding->MarkAsGarbage();
	Following->MarkAsGarbage();
	Discard(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberRenameNeedsRedirectTest,
	"Cortex.Blueprint.AdoptMember.RenameNeedsRedirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberRenameNeedsRedirectTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	FStateAFixture Fixture = MakeStateA(TEXT("AdoptPlanRename"), false, false, false, UCortexBPAdoptObjectParent::StaticClass(), UObject::StaticClass(), /*bNodes=*/false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}
	// The rename source: a BP-only variable `bOpen` (the native parent declares `bIsOpen`).
	Fixture.Blueprint->ParentClass = UObject::StaticClass();
	AddVariable(Fixture.Blueprint, TEXT("bOpen"), BoolPin(), TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	Fixture.Blueprint->ParentClass = UCortexBPAdoptObjectParent::StaticClass();
	// bPowerOn also collides with the parent, so it is mapped as well.
	const TArray<TSharedRef<FJsonObject>> Mappings = {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("bOpen"), TEXT("bIsOpen"))};

	{
		const FStateDigest Before = FStateDigest::Of(Fixture.Blueprint);
		const FCortexCommandResult Result = DryRun(Fixture.Blueprint, Mappings);
		TestFalse(TEXT("A rename without a redirect refuses"), Result.bSuccess);
		TestEqual(TEXT("MISSING_PROPERTY_REDIRECT"), Result.ErrorCode, CortexErrorCodes::MissingPropertyRedirect);
		const FString ExpectedLine = FString::Printf(TEXT("+PropertyRedirects=(OldName=\"%s.bOpen\",NewName=\"%s.bIsOpen\")"),
			*UCortexBPAdoptObjectParent::StaticClass()->GetPathName(), *UCortexBPAdoptObjectParent::StaticClass()->GetPathName());
		TestEqual(TEXT("The refusal carries the exact [CoreRedirects] line"), StringOf(Result.ErrorDetails, TEXT("redirect_line")), ExpectedLine);
		TestTrue(TEXT("Nothing changed on refusal"), Before == FStateDigest::Of(Fixture.Blueprint));
	}
	{
		FRedirectScope Redirect(UCortexBPAdoptObjectParent::StaticClass(), TEXT("bOpen"), TEXT("bIsOpen"));
		const FCortexCommandResult Result = DryRun(Fixture.Blueprint, Mappings);
		if (TestTrue(TEXT("With a redirect the plan passes"), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Open = FindMapping(Result, TEXT("bOpen"));
			TestTrue(TEXT("redirect_present"), Open.IsValid() && Open->GetBoolField(TEXT("redirect_present")));
			TestTrue(TEXT("rename flagged"), Open.IsValid() && Open->GetBoolField(TEXT("rename")));
			TestFalse(TEXT("No loss without the acknowledgment"), Open.IsValid() && Open->GetBoolField(TEXT("unloaded_instance_values_lost")));
		}
		TSharedPtr<FJsonObject> Params = AdoptParams(Fixture.Blueprint, Mappings);
		Params->SetBoolField(TEXT("dry_run"), true);
		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("instance_values:bOpen")});
		const FCortexCommandResult Stale = FCortexBPAdoptMemberOps::Execute(Params);
		TestFalse(TEXT("An acknowledgment for a mapping the redirect already covers is stale"), Stale.bSuccess);
		TestEqual(TEXT("Stale acknowledgment code"), Stale.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> Params = AdoptParams(Fixture.Blueprint, Mappings);
		Params->SetBoolField(TEXT("dry_run"), true);
		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("instance_values:bOpen")});
		const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(Params);
		if (TestTrue(TEXT("Acknowledging instance_values passes without a redirect"), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Open = FindMapping(Result, TEXT("bOpen"));
			TestTrue(TEXT("The loss is reported"), Open.IsValid() && Open->GetBoolField(TEXT("unloaded_instance_values_lost")));
		}
		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("instance_values:bPowerOn")});
		const FCortexCommandResult Wrong = FCortexBPAdoptMemberOps::Execute(Params);
		TestFalse(TEXT("instance_values for a same-name mapping is refused"), Wrong.bSuccess);
		TestEqual(TEXT("Wrong key code"), Wrong.ErrorCode, CortexErrorCodes::InvalidField);
	}
	Discard(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberDryRunChangesNothingTest,
	"Cortex.Blueprint.AdoptMember.DryRunChangesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberDryRunChangesNothingTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	FStateAFixture Fixture = MakeStateA(TEXT("AdoptPlanDryRun"), true, true, true);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}
	Fixture.Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FStateDigest Before = FStateDigest::Of(Fixture.Blueprint);
	TestFalse(TEXT("Package starts clean"), Before.bDirty);

	// Refusing dry run (TypeMismatch is incompatible and the plan has an unmapped collision).
	const FCortexCommandResult Refused = DryRun(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("TypeMismatch"), TEXT("TypeMismatch"))});
	TestFalse(TEXT("The dry run refuses"), Refused.bSuccess);
	TestTrue(TEXT("Nothing changed on refusal"), Before == FStateDigest::Of(Fixture.Blueprint));

	// Succeeding dry run on a clean state-A Blueprint.
	FStateAFixture Clean = MakeStateA(TEXT("AdoptPlanDryRunClean"), true, true, false);
	if (!TestNotNull(TEXT("Clean fixture"), Clean.Blueprint))
	{
		Discard(Fixture.Blueprint);
		return false;
	}
	Clean.Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FStateDigest CleanBefore = FStateDigest::Of(Clean.Blueprint);
	const FCortexCommandResult Result = DryRun(Clean.Blueprint,
		{Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed")), Mapping(TEXT("SavedName"), TEXT("SavedName"))});
	TestTrue(TEXT("The dry run succeeds"), Result.bSuccess);
	TestTrue(TEXT("Parent, NewVariables, dirty flag and fingerprint are identical"), CleanBefore == FStateDigest::Of(Clean.Blueprint));
	TestTrue(TEXT("The parent is still the native class"), Clean.Blueprint->ParentClass == ACortexBPAdoptParent::StaticClass());
	TestTrue(TEXT("bPowerOn still declared"), HasVariable(Clean.Blueprint, TEXT("bPowerOn")));

	// The fingerprint a dry run reports is the one apply accepts.
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
		if (TestTrue(TEXT("Fingerprint reported"), Result.Data->TryGetObjectField(TEXT("fingerprint_before"), Fingerprint)))
		{
			TSharedPtr<FJsonObject> Params = AdoptParams(Clean.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn")), Mapping(TEXT("Speed"), TEXT("Speed")), Mapping(TEXT("SavedName"), TEXT("SavedName"))});
			Params->SetBoolField(TEXT("dry_run"), true);
			Params->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
			TestTrue(TEXT("expected_fingerprint from the dry run is accepted"), FCortexBPAdoptMemberOps::Execute(Params).bSuccess);

			TSharedPtr<FJsonObject> Stale = MakeShared<FJsonObject>(*(*Fingerprint));
			Stale->SetStringField(TEXT("migration_state_hash"), TEXT("0000"));
			Params->SetObjectField(TEXT("expected_fingerprint"), Stale);
			const FCortexCommandResult StaleResult = FCortexBPAdoptMemberOps::Execute(Params);
			TestFalse(TEXT("A stale fingerprint refuses"), StaleResult.bSuccess);
			TestEqual(TEXT("STALE_PRECONDITION"), StaleResult.ErrorCode, CortexErrorCodes::StalePrecondition);
		}
	}

	Discard(Clean.Blueprint);
	Discard(Fixture.Blueprint);
	return true;
}
