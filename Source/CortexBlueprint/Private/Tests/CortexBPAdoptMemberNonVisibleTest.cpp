#include "Misc/AutomationTest.h"

#include "CortexBPAdoptHiddenTestTypes.h"
#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexBPAdoptMemberOps.h"

// Item 18 of issue 274: a same-named native property that is not Blueprint visible is planned as
// engine_already_adopted once the Blueprint variable no longer exists; while the variable still
// exists the adoption refuses.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberNonVisiblePlanTest,
	"Cortex.Blueprint.AdoptMember.NonVisible.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberNonVisiblePlanTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptMemberTest;
	using namespace CortexAdoptTest;
	ExpectConformanceWarnings(*this);

	FStateAFixture Fixture = MakeStateA(
		TEXT("AdoptHiddenPlan"), false, false, false, ACortexBPAdoptHiddenParent::StaticClass(), nullptr, /*bNodes=*/false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("The Blueprint still declares bPowerOn"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn")));

	const auto DryRun = [&Fixture]()
	{
		TSharedPtr<FJsonObject> Params = AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"))});
		Params->SetBoolField(TEXT("dry_run"), true);
		return FCortexBPAdoptMemberOps::Execute(Params);
	};

	// The variable still exists: the refusal stays.
	{
		const FCortexCommandResult Refused = DryRun();
		TestFalse(TEXT("A non-visible native property refuses while the variable exists"), Refused.bSuccess);
		TestEqual(TEXT("Refusal code"), Refused.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("The refusal names the visibility"), Refused.ErrorMessage.Contains(TEXT("not Blueprint visible")));
	}

	// The variable is gone (the engine replaced it): the member is already adopted.
	FBlueprintEditorUtils::RemoveMemberVariable(Fixture.Blueprint, TEXT("bPowerOn"));
	TestFalse(TEXT("The variable is gone"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn")));
	{
		const FCortexCommandResult Result = DryRun();
		if (!Result.bSuccess)
		{
			AddInfo(Explain(Result));
		}
		if (TestTrue(TEXT("The dry run succeeds once the variable is gone"), Result.bSuccess) && Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject> Power = FindMapping(Result, TEXT("bPowerOn"));
			TestEqual(TEXT("state"), StringOf(Power, TEXT("state")), FString(TEXT("engine_already_adopted")));
			TestEqual(TEXT("type_match"), StringOf(Power, TEXT("type_match")), FString(TEXT("verified")));
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			bool bVisibilityWarning = false;
			if (Power.IsValid() && Power->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
				{
					bVisibilityWarning |= Warning->AsString().Contains(TEXT("not Blueprint visible"));
				}
			}
			TestTrue(TEXT("The plan warns that no node can resolve to the property"), bVisibilityWarning);
		}
	}

	// A rename request cannot have been adopted by the engine, so it keeps its own refusal.
	{
		TSharedPtr<FJsonObject> Params = AdoptParams(Fixture.Blueprint, {Mapping(TEXT("bGone"), TEXT("bPowerOn"))});
		Params->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Rename = FCortexBPAdoptMemberOps::Execute(Params);
		TestFalse(TEXT("A rename of a missing variable still refuses"), Rename.bSuccess);
		TestEqual(TEXT("Rename refusal code"), Rename.ErrorCode, CortexErrorCodes::VariableNotFound);
	}
	Discard(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptMemberNonVisibleApplyTest,
	"Cortex.Blueprint.AdoptMember.NonVisible.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptMemberNonVisibleApplyTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptMemberTest;
	using namespace CortexAdoptTest;
	ExpectConformanceWarnings(*this);

	FStateAFixture Fixture = MakeStateA(
		TEXT("AdoptHiddenApply"), false, false, false, ACortexBPAdoptHiddenParent::StaticClass(), nullptr, /*bNodes=*/false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::RemoveMemberVariable(Fixture.Blueprint, TEXT("bPowerOn"));
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	if (!TestFalse(TEXT("The variable is gone"), HasVariable(Fixture.Blueprint, TEXT("bPowerOn"))))
	{
		Discard(Fixture.Blueprint);
		return false;
	}

	TSharedRef<FJsonObject> PowerMapping = Mapping(TEXT("bPowerOn"), TEXT("bPowerOn"));
	PowerMapping->SetBoolField(TEXT("default"), true);
	const FCortexCommandResult Result = FCortexBPAdoptMemberOps::Execute(AdoptParams(Fixture.Blueprint, {PowerMapping}));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("Adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("state"), StringOf(FindMapping(Result, TEXT("bPowerOn")), TEXT("state")), FString(TEXT("engine_already_adopted")));
	}
	TestTrue(TEXT("Blueprint compiled"), Fixture.Blueprint->Status == BS_UpToDate || Fixture.Blueprint->Status == BS_UpToDateWithWarnings);
	if (const ACortexBPAdoptHiddenParent* Defaults = Cast<ACortexBPAdoptHiddenParent>(Fixture.Blueprint->GeneratedClass->GetDefaultObject()))
	{
		TestTrue(TEXT("The supplied default (true) is on the native property"), Defaults->bPowerOn);
	}
	else
	{
		AddError(TEXT("The generated class does not derive from ACortexBPAdoptHiddenParent"));
	}
	Discard(Fixture.Blueprint);
	return true;
}
