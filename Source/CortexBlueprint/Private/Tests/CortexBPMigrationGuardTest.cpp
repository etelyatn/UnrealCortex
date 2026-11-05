#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationGuardTest,
	"Cortex.Blueprint.MigrationGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationGuardTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("BP_MigrationGuard"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	FEdGraphPinType IntPin;
	IntPin.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Counter"), IntPin);
	USCS_Node* ScsNode = Blueprint->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("HostMesh"));
	Blueprint->SimpleConstructionScript->AddNode(ScsNode);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const auto Hash = [Blueprint]()
	{
		const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		return Fingerprint.IsValid() ? Fingerprint->GetStringField(TEXT("migration_state_hash")) : FString();
	};

	const FString Baseline = Hash();
	TestFalse(TEXT("Fingerprint has a state hash"), Baseline.IsEmpty());
	TestEqual(TEXT("Stable without changes"), Hash(), Baseline);
	Blueprint->GetOutermost()->SetDirtyFlag(!Blueprint->GetOutermost()->IsDirty());
	TestEqual(TEXT("Stable across the dirty flag"), Hash(), Baseline);

	// A variable default (uncompiled text).
	FBPVariableDescription& Variable = Blueprint->NewVariables.Last();
	const FString OriginalDefault = Variable.DefaultValue;
	Variable.DefaultValue = TEXT("5");
	TestNotEqual(TEXT("Changes with a variable default"), Hash(), Baseline);
	Variable.DefaultValue = OriginalDefault;
	TestEqual(TEXT("Returns to baseline after revert (default)"), Hash(), Baseline);

	// A CDO value.
	UObject* Cdo = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
	const FIntProperty* CounterProperty = Blueprint->GeneratedClass ? CastField<FIntProperty>(Blueprint->GeneratedClass->FindPropertyByName(TEXT("Counter"))) : nullptr;
	if (TestNotNull(TEXT("CDO"), Cdo) && TestNotNull(TEXT("Counter property"), CounterProperty))
	{
		const int32 OriginalCounter = CounterProperty->GetPropertyValue_InContainer(Cdo);
		CounterProperty->SetPropertyValue_InContainer(Cdo, OriginalCounter + 7);
		TestNotEqual(TEXT("Changes with a CDO value"), Hash(), Baseline);
		CounterProperty->SetPropertyValue_InContainer(Cdo, OriginalCounter);
		TestEqual(TEXT("Returns to baseline after revert (CDO)"), Hash(), Baseline);
	}

	// An SCS node property.
	if (TestNotNull(TEXT("SCS template"), ScsNode->ComponentTemplate.Get()))
	{
		USceneComponent* Template = CastChecked<USceneComponent>(ScsNode->ComponentTemplate);
		const bool bOriginalHidden = Template->bHiddenInGame;
		Template->bHiddenInGame = !bOriginalHidden;
		TestNotEqual(TEXT("Changes with an SCS node property"), Hash(), Baseline);
		Template->bHiddenInGame = bOriginalHidden;
		TestEqual(TEXT("Returns to baseline after revert (SCS)"), Hash(), Baseline);
	}

	// The parent.
	UClass* OriginalParent = Blueprint->ParentClass;
	Blueprint->ParentClass = ACortexBPAdoptOtherActorParent::StaticClass();
	TestNotEqual(TEXT("Changes with the parent"), Hash(), Baseline);
	Blueprint->ParentClass = OriginalParent;
	TestEqual(TEXT("Returns to baseline after revert (parent)"), Hash(), Baseline);

	// CheckExpected.
	FCortexCommandResult Error;
	const TSharedPtr<FJsonObject> Current = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	TestTrue(TEXT("Current fingerprint accepted"), FCortexBPMigrationGuard::CheckExpected(Blueprint, Current, Error));

	TSharedPtr<FJsonObject> Stale = MakeShared<FJsonObject>();
	Stale->SetStringField(TEXT("migration_state_hash"), TEXT("0000"));
	TestFalse(TEXT("Stale hash refused"), FCortexBPMigrationGuard::CheckExpected(Blueprint, Stale, Error));
	TestEqual(TEXT("Stale code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	if (TestTrue(TEXT("Stale details carry the current fingerprint"),
		Error.ErrorDetails.IsValid() && Error.ErrorDetails->HasTypedField<EJson::Object>(TEXT("current_fingerprint"))))
	{
		const TSharedPtr<FJsonObject> Reported = Error.ErrorDetails->GetObjectField(TEXT("current_fingerprint"));
		TestEqual(TEXT("Reported current hash"), Reported->GetStringField(TEXT("migration_state_hash")), Baseline);
	}

	// A fingerprint taken before a change is stale after it.
	const TSharedPtr<FJsonObject> Before = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	Variable.DefaultValue = TEXT("9");
	TestFalse(TEXT("Fingerprint taken before a change is stale"), FCortexBPMigrationGuard::CheckExpected(Blueprint, Before, Error));
	TestEqual(TEXT("Stale-after-change code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	Variable.DefaultValue = OriginalDefault;

	TSharedPtr<FJsonObject> Unknown = MakeShared<FJsonObject>();
	Unknown->SetStringField(TEXT("migration_state_hash"), Baseline);
	Unknown->SetStringField(TEXT("bogus"), TEXT("x"));
	TestFalse(TEXT("Unknown field refused"), FCortexBPMigrationGuard::CheckExpected(Blueprint, Unknown, Error));
	TestEqual(TEXT("Unknown field code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("graph_authoring_hash"), Current->GetStringField(TEXT("graph_authoring_hash")));
	TestFalse(TEXT("Missing migration_state_hash refused"), FCortexBPMigrationGuard::CheckExpected(Blueprint, Missing, Error));
	TestEqual(TEXT("Missing hash code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	// Graph edits since the dry run must be caught: migration_state_hash alone is not enough.
	TSharedPtr<FJsonObject> StateOnly = MakeShared<FJsonObject>();
	StateOnly->SetStringField(TEXT("migration_state_hash"), Baseline);
	StateOnly->SetNumberField(TEXT("migration_state_version"), 1);
	TestFalse(TEXT("migration_state_hash without the graph hash is stale"), FCortexBPMigrationGuard::CheckExpected(Blueprint, StateOnly, Error));
	TestEqual(TEXT("State-only code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	TSharedPtr<FJsonObject> NoVersion = MakeShared<FJsonObject>();
	NoVersion->SetStringField(TEXT("migration_state_hash"), Baseline);
	NoVersion->SetStringField(TEXT("graph_authoring_hash"), Current->GetStringField(TEXT("graph_authoring_hash")));
	NoVersion->SetNumberField(TEXT("graph_authoring_version"), 1);
	TestFalse(TEXT("Missing migration_state_version is stale"), FCortexBPMigrationGuard::CheckExpected(Blueprint, NoVersion, Error));
	TestTrue(TEXT("The fingerprint carries migration_state_version 1"), Current->HasTypedField<EJson::Number>(TEXT("migration_state_version")));

	TestFalse(TEXT("Null expected refused"), FCortexBPMigrationGuard::CheckExpected(Blueprint, nullptr, Error));
	TestEqual(TEXT("Null expected code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Discard(Blueprint);
	return true;
}
