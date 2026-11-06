#include "Misc/AutomationTest.h"

#include "CortexBPReparentTestSupport.h"
#include "CortexCommandRouter.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPReparentOps.h"
#include "UObject/Package.h"

namespace
{
	FString FingerprintText(UBlueprint* Blueprint)
	{
		const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Fingerprint.ToSharedRef(), Writer);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentDryRunTest,
	"Cortex.Blueprint.Reparent.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentDryRunTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("ReparentDryRun"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	AddVariable(Blueprint, TEXT("Speed"), DoublePin(), TEXT("2.5"));
	AddVariable(Blueprint, TEXT("TypeMismatch"), StringPin(), TEXT("text"));

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("NativeCall"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, /*bIsUserCreated=*/true, nullptr);

	USCS_Node* Component = Blueprint->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("ReparentDryRunMesh"));
	Blueprint->SimpleConstructionScript->AddNode(Component);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UPackage* Package = Blueprint->GetOutermost();
	Package->SetDirtyFlag(false);
	const FString FingerprintBefore = FingerprintText(Blueprint);
	const int32 VariablesBefore = Blueprint->NewVariables.Num();

	TSharedPtr<FJsonObject> Params = ReparentParams(Blueprint, TEXT("CortexBPAdoptParent"));
	Params->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Result = FCortexBPReparentOps::Execute(Params);
	if (TestTrue(TEXT("Dry run answers"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Power = FindByKey(Result.Data, TEXT("collisions"), TEXT("variable:bPowerOn"));
		if (TestNotNull(TEXT("bPowerOn collision"), Power.Get()))
		{
			TestEqual(TEXT("bPowerOn resolution"), Power->GetStringField(TEXT("resolution")), FString(TEXT("engine_replaces")));
			TestTrue(TEXT("bPowerOn type_exact"), Power->GetBoolField(TEXT("type_exact")));
			TestFalse(TEXT("bPowerOn not blocking"), Power->GetBoolField(TEXT("blocking")));
		}
		const TSharedPtr<FJsonObject> Speed = FindByKey(Result.Data, TEXT("collisions"), TEXT("variable:Speed"));
		if (TestNotNull(TEXT("Speed collision"), Speed.Get()))
		{
			TestEqual(TEXT("Speed resolution"), Speed->GetStringField(TEXT("resolution")), FString(TEXT("adoptable_convertible")));
			TestFalse(TEXT("Speed not blocking"), Speed->GetBoolField(TEXT("blocking")));
		}
		const TSharedPtr<FJsonObject> Mismatch = FindByKey(Result.Data, TEXT("collisions"), TEXT("variable:TypeMismatch"));
		if (TestNotNull(TEXT("TypeMismatch collision"), Mismatch.Get()))
		{
			TestEqual(TEXT("TypeMismatch resolution"), Mismatch->GetStringField(TEXT("resolution")), FString(TEXT("engine_renames")));
			TestTrue(TEXT("TypeMismatch blocking"), Mismatch->GetBoolField(TEXT("blocking")));
		}
		const TSharedPtr<FJsonObject> Native = FindByKey(Result.Data, TEXT("collisions"), TEXT("function:NativeCall"));
		if (TestNotNull(TEXT("NativeCall collision"), Native.Get()))
		{
			TestTrue(TEXT("NativeCall blocking"), Native->GetBoolField(TEXT("blocking")));
		}
		const TArray<FString> Blocking = ReadStrings(Result.Data, TEXT("blocking"));
		TestTrue(TEXT("Blocking lists TypeMismatch"), Blocking.Contains(TEXT("variable:TypeMismatch")));
		TestTrue(TEXT("Blocking lists NativeCall"), Blocking.Contains(TEXT("function:NativeCall")));
		TestFalse(TEXT("Blocking omits the replaced variable"), Blocking.Contains(TEXT("variable:bPowerOn")));
		const TArray<FString> Required = ReadStrings(Result.Data, TEXT("required_acknowledgment"));
		TestTrue(TEXT("Acknowledgment lists bPowerOn"), Required.Contains(TEXT("variable:bPowerOn")));
		TestTrue(TEXT("Acknowledgment lists Speed"), Required.Contains(TEXT("variable:Speed")));
		TestFalse(TEXT("Acknowledgment omits blocking items"), Required.Contains(TEXT("variable:TypeMismatch")));
		TestTrue(TEXT("Dry run flagged"), Result.Data->GetBoolField(TEXT("dry_run")));
		TestFalse(TEXT("Not reparented"), Result.Data->GetBoolField(TEXT("reparented")));
		TestTrue(TEXT("Current fingerprint returned"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint_before")));
	}

	TestTrue(TEXT("Parent unchanged"), Blueprint->ParentClass == AActor::StaticClass());
	TestEqual(TEXT("NewVariables unchanged"), Blueprint->NewVariables.Num(), VariablesBefore);
	TestFalse(TEXT("Package not dirtied"), Package->IsDirty());
	TestEqual(TEXT("Fingerprint unchanged"), FingerprintText(Blueprint), FingerprintBefore);

	// An unknown class is reported before anything else.
	TSharedPtr<FJsonObject> Unknown = ReparentParams(Blueprint, TEXT("CortexBPNoSuchClassAnywhere"));
	Unknown->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult UnknownResult = FCortexBPReparentOps::Execute(Unknown);
	TestFalse(TEXT("Unknown parent refused"), UnknownResult.bSuccess);
	TestEqual(TEXT("Unknown parent code"), UnknownResult.ErrorCode, CortexErrorCodes::ClassNotFound);

	// A short name shared by two Blueprints is ambiguous.
	UBlueprint* AmbiguousA = MakeBlueprint(TEXT("ReparentAmbiguous"), AActor::StaticClass(), TEXT("ReparentAmbiguousPkgA"));
	UBlueprint* AmbiguousB = MakeBlueprint(TEXT("ReparentAmbiguous"), AActor::StaticClass(), TEXT("ReparentAmbiguousPkgB"));
	if (TestNotNull(TEXT("Ambiguous A"), AmbiguousA) && TestNotNull(TEXT("Ambiguous B"), AmbiguousB))
	{
		TSharedPtr<FJsonObject> Ambiguous = ReparentParams(Blueprint, TEXT("ReparentAmbiguous"));
		Ambiguous->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult AmbiguousResult = FCortexBPReparentOps::Execute(Ambiguous);
		TestFalse(TEXT("Ambiguous parent refused"), AmbiguousResult.bSuccess);
		TestEqual(TEXT("Ambiguous parent code"), AmbiguousResult.ErrorCode, CortexErrorCodes::AmbiguousClass);
	}

	Discard(AmbiguousA);
	Discard(AmbiguousB);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentRefusesFamilyTest,
	"Cortex.Blueprint.Reparent.RefusesFamilyAndReportsCollisions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentRefusesFamilyTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (UserWidgetClass == nullptr || WidgetBlueprintClass == nullptr)
	{
		AddInfo(TEXT("UMG not available; skipping the Widget to Actor reparent test"));
		return true;
	}

	UBlueprint* Blueprint = MakeBlueprint(TEXT("WBP_ReparentFamily"), UserWidgetClass, nullptr, WidgetBlueprintClass);
	if (!TestNotNull(TEXT("Widget Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bCanBeDamaged"), BoolPin(), TEXT("true"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UPackage* Package = Blueprint->GetOutermost();
	Package->SetDirtyFlag(false);
	const FString FingerprintBefore = FingerprintText(Blueprint);

	TSharedPtr<FJsonObject> Params = ReparentParams(Blueprint, TEXT("/Script/Engine.Actor"));
	Params->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Result = FCortexBPReparentOps::Execute(Params);
	TestFalse(TEXT("Widget to Actor refused"), Result.bSuccess);
	TestEqual(TEXT("Family refusal code"), Result.ErrorCode, CortexErrorCodes::InvalidParentClass);
	if (TestTrue(TEXT("Refusal carries details"), Result.ErrorDetails.IsValid()))
	{
		TestEqual(TEXT("from_family"), Result.ErrorDetails->GetStringField(TEXT("from_family")), FString(TEXT("Widget")));
		TestEqual(TEXT("to_family"), Result.ErrorDetails->GetStringField(TEXT("to_family")), FString(TEXT("Actor")));
		TestNotNull(TEXT("Collision with the Actor property is reported"),
			FindByKey(Result.ErrorDetails, TEXT("collisions"), TEXT("variable:bCanBeDamaged")).Get());
	}

	TestTrue(TEXT("Parent unchanged"), Blueprint->ParentClass == UserWidgetClass);
	TestFalse(TEXT("Package not dirtied"), Package->IsDirty());
	TestEqual(TEXT("Fingerprint unchanged"), FingerprintText(Blueprint), FingerprintBefore);

	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentAcknowledgmentTest,
	"Cortex.Blueprint.Reparent.AcknowledgmentAndScsRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentAcknowledgmentTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("ReparentAcknowledgment"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	AddVariable(Blueprint, TEXT("bNotSave"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	AddVariable(Blueprint, TEXT("Speed"), DoublePin(), TEXT("2.5"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();
	TSharedPtr<FJsonObject> DryRun = ReparentParams(Blueprint, Target);
	DryRun->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Plan = FCortexBPReparentOps::Execute(DryRun);
	TArray<FString> Required;
	if (TestTrue(TEXT("Dry run answers"), Plan.bSuccess))
	{
		Required = ReadStrings(Plan.Data, TEXT("required_acknowledgment"));
	}
	const TArray<FString> Expected = {TEXT("save_game:bNotSave"), TEXT("variable:bNotSave"), TEXT("variable:bPowerOn"), TEXT("variable:Speed")};
	TestTrue(TEXT("Required acknowledgment is the sorted union"), Required == Expected);
	TestFalse(TEXT("A SaveGame variable that keeps name, type and flag raises nothing"), Required.Contains(TEXT("save_game:bPowerOn")));

	// No acknowledgment, a partial one and one with a stale extra key all refuse.
	TSharedPtr<FJsonObject> Apply = ReparentParams(Blueprint, Target);
	FCortexCommandResult None = FCortexBPReparentOps::Execute(Apply);
	TestFalse(TEXT("Missing acknowledgment refuses"), None.bSuccess);
	TestEqual(TEXT("Missing acknowledgment code"), None.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	if (None.ErrorDetails.IsValid())
	{
		TestTrue(TEXT("Refusal lists the required set"), ReadStrings(None.ErrorDetails, TEXT("required_acknowledgment")) == Expected);
	}

	TArray<FString> Partial = Expected;
	Partial.RemoveAt(0);
	SetStrings(Apply, TEXT("acknowledged_collisions"), Partial);
	TestEqual(TEXT("Partial acknowledgment code"), FCortexBPReparentOps::Execute(Apply).ErrorCode, CortexErrorCodes::PotentialDataLoss);

	TArray<FString> Stale = Expected;
	Stale.Add(TEXT("variable:NoSuchVariable"));
	SetStrings(Apply, TEXT("acknowledged_collisions"), Stale);
	TestEqual(TEXT("Stale acknowledgment code"), FCortexBPReparentOps::Execute(Apply).ErrorCode, CortexErrorCodes::PotentialDataLoss);
	TestTrue(TEXT("Parent unchanged by refusals"), Blueprint->ParentClass == AActor::StaticClass());

	// A stale expected_fingerprint refuses before anything else.
	TSharedPtr<FJsonObject> Stamped = ReparentParams(Blueprint, Target);
	SetStrings(Stamped, TEXT("acknowledged_collisions"), Expected);
	TSharedPtr<FJsonObject> StaleFingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	StaleFingerprint->SetStringField(TEXT("migration_state_hash"), TEXT("stale"));
	Stamped->SetObjectField(TEXT("expected_fingerprint"), StaleFingerprint);
	const FCortexCommandResult StaleResult = FCortexBPReparentOps::Execute(Stamped);
	TestFalse(TEXT("Stale fingerprint refuses"), StaleResult.bSuccess);
	TestEqual(TEXT("Stale fingerprint code"), StaleResult.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("Parent unchanged by the stale fingerprint"), Blueprint->ParentClass == AActor::StaticClass());

	Discard(Blueprint);

	// An adoptable SCS collision needs compile:false.
	UBlueprint* Scs = MakeBlueprint(TEXT("ReparentScsRule"), AActor::StaticClass());
	if (!TestNotNull(TEXT("SCS Blueprint"), Scs))
	{
		return false;
	}
	USCS_Node* Mirror = Scs->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("NativeMirror"));
	Scs->SimpleConstructionScript->AddNode(Mirror);
	FKismetEditorUtilities::CompileBlueprint(Scs);

	const FString ComponentTarget = ACortexBPAdoptComponentParent::StaticClass()->GetPathName();
	TSharedPtr<FJsonObject> ScsDryRun = ReparentParams(Scs, ComponentTarget);
	ScsDryRun->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult ScsPlan = FCortexBPReparentOps::Execute(ScsDryRun);
	if (TestTrue(TEXT("SCS dry run answers"), ScsPlan.bSuccess))
	{
		const TSharedPtr<FJsonObject> Collision = FindByKey(ScsPlan.Data, TEXT("collisions"), TEXT("scs:NativeMirror"));
		if (TestNotNull(TEXT("SCS collision reported"), Collision.Get()))
		{
			TestEqual(TEXT("SCS collision is adoptable"), Collision->GetStringField(TEXT("resolution")), FString(TEXT("adoptable")));
		}
		TestTrue(TEXT("SCS key is acknowledgeable"), ReadStrings(ScsPlan.Data, TEXT("required_acknowledgment")).Contains(TEXT("scs:NativeMirror")));
	}

	TSharedPtr<FJsonObject> ScsApply = ReparentParams(Scs, ComponentTarget);
	SetStrings(ScsApply, TEXT("acknowledged_collisions"), {TEXT("scs:NativeMirror")});
	ScsApply->SetBoolField(TEXT("compile"), true);
	const FCortexCommandResult ScsRefused = FCortexBPReparentOps::Execute(ScsApply);
	TestFalse(TEXT("SCS collision with compile:true refuses"), ScsRefused.bSuccess);
	TestEqual(TEXT("SCS collision code"), ScsRefused.ErrorCode, CortexErrorCodes::NameCollision);
	TestTrue(TEXT("Refusal names compile:false"), ScsRefused.ErrorMessage.Contains(TEXT("compile:false")));
	TestTrue(TEXT("Parent unchanged by the SCS refusal"), Scs->ParentClass == AActor::StaticClass());

	Discard(Scs);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentBlueprintParentNamesTest,
	"Cortex.Blueprint.Reparent.BlueprintParentNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentBlueprintParentNamesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	// The parent Blueprint uses `ReparentIndex` only as a local variable of one of its functions: no field lookup finds
	// it, but the compiler's name validator does and renames a child variable of that name.
	UBlueprint* ParentBlueprint = MakeBlueprint(TEXT("ReparentNamesParent"), AActor::StaticClass());
	UBlueprint* Child = MakeBlueprint(TEXT("ReparentNamesChild"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Parent Blueprint"), ParentBlueprint) || !TestNotNull(TEXT("Child Blueprint"), Child))
	{
		return false;
	}
	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		ParentBlueprint, TEXT("ParentFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(ParentBlueprint, Function, /*bIsUserCreated=*/true, nullptr);
	TestTrue(TEXT("Local variable added"),
		FBlueprintEditorUtils::AddLocalVariable(ParentBlueprint, Function, TEXT("ReparentIndex"), StringPin(), FString()));
	FKismetEditorUtilities::CompileBlueprint(ParentBlueprint);

	AddVariable(Child, TEXT("ReparentIndex"), StringPin(), TEXT("text"));
	FKismetEditorUtilities::CompileBlueprint(Child);

	TSharedPtr<FJsonObject> Params = ReparentParams(Child, ParentBlueprint->GetPathName());
	Params->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Result = FCortexBPReparentOps::Execute(Params);
	if (TestTrue(TEXT("Dry run answers"), Result.bSuccess))
	{
		const TSharedPtr<FJsonObject> Collision = FindByKey(Result.Data, TEXT("collisions"), TEXT("variable:ReparentIndex"));
		if (TestNotNull(TEXT("Name collision with the parent's local variable"), Collision.Get()))
		{
			TestEqual(TEXT("Resolution"), Collision->GetStringField(TEXT("resolution")), FString(TEXT("engine_renames")));
			TestTrue(TEXT("Blocking"), Collision->GetBoolField(TEXT("blocking")));
		}
	}
	TSharedPtr<FJsonObject> Apply = ReparentParams(Child, ParentBlueprint->GetPathName());
	const FCortexCommandResult Refused = FCortexBPReparentOps::Execute(Apply);
	TestEqual(TEXT("Apply refuses"), Refused.ErrorCode, CortexErrorCodes::NameCollision);
	TestTrue(TEXT("Parent unchanged"), Child->ParentClass == AActor::StaticClass());

	Discard(Child);
	Discard(ParentBlueprint);
	return true;
}
