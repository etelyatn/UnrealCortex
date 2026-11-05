#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "CortexBPReparentTestSupport.h"
#include "CortexCommandRouter.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CustomEvent.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPReparentOps.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace
{
	/** Required acknowledgment of a dry run, echoed back as acknowledged_collisions. */
	TArray<FString> AcknowledgmentFor(UBlueprint* Blueprint, const FString& NewParent)
	{
		TSharedPtr<FJsonObject> DryRun = CortexReparentTest::ReparentParams(Blueprint, NewParent);
		DryRun->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Plan = FCortexBPReparentOps::Execute(DryRun);
		return Plan.bSuccess ? CortexReparentTest::ReadStrings(Plan.Data, TEXT("required_acknowledgment")) : TArray<FString>();
	}

	struct FPostApplyHookScope
	{
		explicit FPostApplyHookScope(TFunction<void(UBlueprint*)> Hook)
		{
			FCortexBPReparentOps::SetPostApplyHookForTest(MoveTemp(Hook));
		}
		~FPostApplyHookScope()
		{
			FCortexBPReparentOps::ClearPostApplyHookForTest();
		}
	};

	FString PackageFile(const UBlueprint* Blueprint)
	{
		return FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentAppliesWithReadbackTest,
	"Cortex.Blueprint.Reparent.AppliesWithReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentAppliesWithReadbackTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("ReparentApplies"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	AddVariable(Blueprint, TEXT("Speed"), DoublePin(), TEXT("2.5"));
	UK2Node_VariableGet* SpeedGet = AddVariableGet(Blueprint, TEXT("Speed"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestNotNull(TEXT("Speed get node"), SpeedGet))
	{
		Discard(Blueprint);
		return false;
	}
	const int32 NodesBefore = FBlueprintEditorUtils::FindEventGraph(Blueprint)->Nodes.Num();
	const AActor* OldDefaults = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
	TestEqual(TEXT("AActor default before"), OldDefaults->InitialLifeSpan, 0.f);

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();
	const TArray<FString> Acknowledgment = AcknowledgmentFor(Blueprint, Target);
	TestTrue(TEXT("Acknowledgment lists bPowerOn"), Acknowledgment.Contains(TEXT("variable:bPowerOn")));
	TestTrue(TEXT("Acknowledgment lists Speed"), Acknowledgment.Contains(TEXT("variable:Speed")));

	TSharedPtr<FJsonObject> Params = ReparentParams(Blueprint, Target);
	SetStrings(Params, TEXT("acknowledged_collisions"), Acknowledgment);
	const FCortexCommandResult Result = FCortexBPReparentOps::Execute(Params);
	if (TestTrue(TEXT("Reparent succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("reparented only after readback"), Result.Data->GetBoolField(TEXT("reparented")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("apply status"), Result.Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		const FString CompileStatus = Result.Data->GetStringField(TEXT("compile_status"));
		TestTrue(TEXT("compile_status reported"), CompileStatus == TEXT("success") || CompileStatus == TEXT("warning"));
		TestTrue(TEXT("error_count reported"), Result.Data->HasTypedField<EJson::Number>(TEXT("error_count")));
		TestTrue(TEXT("warning_count reported"), Result.Data->HasTypedField<EJson::Number>(TEXT("warning_count")));
		TestTrue(TEXT("diagnostics reported"), Result.Data->HasTypedField<EJson::Array>(TEXT("diagnostics")));
		TestTrue(TEXT("fingerprint_before reported"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint_before")));
		TestTrue(TEXT("fingerprint_after reported"), Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint_after")));
		const TSharedPtr<FJsonObject>* Before = nullptr;
		const TSharedPtr<FJsonObject>* After = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("fingerprint_before"), Before) && Result.Data->TryGetObjectField(TEXT("fingerprint_after"), After))
		{
			TestNotEqual(TEXT("Fingerprint moved with the parent"),
				(*Before)->GetStringField(TEXT("migration_state_hash")), (*After)->GetStringField(TEXT("migration_state_hash")));
		}

		TestEqual(TEXT("bPowerOn variable removed"), KindOf(FindDifference(Result.Data, TEXT("variables"), TEXT("bPowerOn"))), FString(TEXT("removed")));
		TestEqual(TEXT("Speed variable removed"), KindOf(FindDifference(Result.Data, TEXT("variables"), TEXT("Speed"))), FString(TEXT("removed")));
		TestEqual(TEXT("bPowerOn CDO member moved"), KindOf(FindDifference(Result.Data, TEXT("cdo"), TEXT("bPowerOn"))), FString(TEXT("moved")));
		TestEqual(TEXT("Speed CDO member moved"), KindOf(FindDifference(Result.Data, TEXT("cdo"), TEXT("Speed"))), FString(TEXT("moved")));
	}

	TestTrue(TEXT("Parent is the target"), Blueprint->ParentClass == ACortexBPAdoptParent::StaticClass());
	TestFalse(TEXT("bPowerOn variable gone"), HasVariable(Blueprint, TEXT("bPowerOn")));
	TestFalse(TEXT("Speed variable gone"), HasVariable(Blueprint, TEXT("Speed")));
	TestFalse(TEXT("No Speed_0 rename"), HasVariable(Blueprint, TEXT("Speed_0")));
	if (const ACortexBPAdoptParent* Defaults = Cast<ACortexBPAdoptParent>(Blueprint->GeneratedClass->GetDefaultObject()))
	{
		TestTrue(TEXT("bPowerOn default (true) survived the move"), Defaults->bPowerOn);
		TestEqual(TEXT("Speed default survived as native float"), Defaults->Speed, 2.5f);
		TestEqual(TEXT("The new native parent's AActor default wins (E5)"), Defaults->InitialLifeSpan, 7.f);
	}
	else
	{
		AddError(TEXT("Generated class CDO is not an ACortexBPAdoptParent"));
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	TestEqual(TEXT("No node was destroyed"), EventGraph->Nodes.Num(), NodesBefore);
	TestTrue(TEXT("The get node is still in the graph"), EventGraph->Nodes.Contains(SpeedGet));
	TestEqual(TEXT("The get node still names Speed"), SpeedGet->VariableReference.GetMemberName(), FName(TEXT("Speed")));
	const FProperty* Resolved = SpeedGet->VariableReference.ResolveMember<FProperty>(Blueprint->SkeletonGeneratedClass);
	if (TestNotNull(TEXT("The get node resolves"), Resolved))
	{
		TestTrue(TEXT("It resolves to the native member"), Resolved->GetOwnerClass() == ACortexBPAdoptParent::StaticClass());
	}

	// Control (E5): the same change without the editor's delta flags keeps the old inherited value.
	UBlueprint* Control = MakeBlueprint(TEXT("ReparentControl"), AActor::StaticClass());
	if (TestNotNull(TEXT("Control Blueprint"), Control))
	{
		Control->ParentClass = ACortexBPAdoptParent::StaticClass();
		FBlueprintEditorUtils::RefreshAllNodes(Control);
		FKismetEditorUtilities::CompileBlueprint(Control);
		if (const AActor* ControlDefaults = Cast<AActor>(Control->GeneratedClass->GetDefaultObject()))
		{
			TestEqual(TEXT("Without the flags the old AActor value overwrites the native default"), ControlDefaults->InitialLifeSpan, 0.f);
		}
	}

	// A discarded Blueprint stays reachable until the next collection: drop the node so other suites that scan every
	// loaded Blueprint never resolve its self-context member without a scope.
	EventGraph->RemoveNode(SpeedGet);
	Discard(Control);
	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentFailsClosedTest,
	"Cortex.Blueprint.Reparent.FailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();

	// A blocking collision is refused before apply.
	UBlueprint* Incompatible = MakeBlueprint(TEXT("ReparentIncompatible"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Incompatible Blueprint"), Incompatible))
	{
		return false;
	}
	AddVariable(Incompatible, TEXT("TypeMismatch"), StringPin(), TEXT("text"));
	FKismetEditorUtilities::CompileBlueprint(Incompatible);
	const FCortexCommandResult Blocked = FCortexBPReparentOps::Execute(ReparentParams(Incompatible, Target));
	TestFalse(TEXT("Incompatible collision refused"), Blocked.bSuccess);
	TestEqual(TEXT("Incompatible collision code"), Blocked.ErrorCode, CortexErrorCodes::NameCollision);
	if (Blocked.ErrorDetails.IsValid())
	{
		TestTrue(TEXT("Refusal lists the blocking collision"), ReadStrings(Blocked.ErrorDetails, TEXT("blocking")).Contains(TEXT("variable:TypeMismatch")));
	}
	TestTrue(TEXT("Parent unchanged"), Incompatible->ParentClass == AActor::StaticClass());
	TestTrue(TEXT("Variable untouched"), HasVariable(Incompatible, TEXT("TypeMismatch")));
	Discard(Incompatible);

	// A forced compile failure rolls the Blueprint back.
	UBlueprint* Blueprint = MakeBlueprint(TEXT("ReparentFailsClosed"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	AddVariable(Blueprint, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FString SnapshotError;
	const TSharedPtr<FJsonObject> SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);
	TestNotNull(TEXT("Pre-call snapshot"), SnapshotBefore.Get());
	const AActor* DefaultsBefore = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
	const float LifeSpanBefore = DefaultsBefore->InitialLifeSpan;

	TSharedPtr<FJsonObject> Params = ReparentParams(Blueprint, Target);
	SetStrings(Params, TEXT("acknowledged_collisions"), AcknowledgmentFor(Blueprint, Target));
	FCortexCommandResult Result;
	{
		FPostApplyHookScope Hook([](UBlueprint* BlueprintToBreak)
		{
			// Two custom events with one name: the compile after the parent change reports an error.
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BlueprintToBreak);
			for (int32 Index = 0; Index < 2; ++Index)
			{
				UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
				Event->CreateNewGuid();
				Event->CustomFunctionName = FName(TEXT("ReparentDuplicateEvent"));
				EventGraph->AddNode(Event, false, false);
				Event->AllocateDefaultPins();
			}
		});
		Result = FCortexBPReparentOps::Execute(Params);
	}
	TestFalse(TEXT("Forced compile failure fails"), Result.bSuccess);
	TestEqual(TEXT("Forced compile failure code"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
	if (TestTrue(TEXT("Failure carries details"), Result.ErrorDetails.IsValid()))
	{
		TestFalse(TEXT("reparented is never true"), Result.ErrorDetails->GetBoolField(TEXT("reparented")));
		TestEqual(TEXT("Rolled back"), Result.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		TestTrue(TEXT("Diagnostics returned"),
			Result.ErrorDetails->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && Diagnostics != nullptr && Diagnostics->Num() > 0);
	}

	TestTrue(TEXT("Parent restored"), Blueprint->ParentClass == AActor::StaticClass());
	TestTrue(TEXT("Variable restored"), HasVariable(Blueprint, TEXT("bPowerOn")));
	FString BlockReason;
	TestFalse(TEXT("Asset is not blocked"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));

	const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), SnapshotError);
	if (SnapshotBefore.IsValid() && TestNotNull(TEXT("Post-call snapshot"), SnapshotAfter.Get()))
	{
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(SnapshotBefore, SnapshotAfter, TSet<FString>(), Differences, Counts);
		TestTrue(TEXT("Snapshots comparable"), Diff.bSuccess);
		TestEqual(TEXT("Snapshot after equals snapshot before"), Differences.Num(), 0);
	}
	if (const AActor* DefaultsAfter = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
	{
		TestEqual(TEXT("The rolled-back CDO keeps the pre-call AActor default"), DefaultsAfter->InitialLifeSpan, LifeSpanBefore);
	}

	Discard(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReparentSaveAndReportTest,
	"Cortex.Blueprint.Reparent.SaveAndReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReparentSaveAndReportTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexReparentTest;

	const FString Target = ACortexBPAdoptParent::StaticClass()->GetPathName();
	const FString ReportRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CortexTests"));
	const FString ReportPath = ReportRoot / FString::Printf(TEXT("ReparentReport_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
	const FString OutsidePath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Intermediate/CortexReparentOutside.json"));

	// save:true writes the package only after success.
	UBlueprint* Saved = MakeBlueprint(TEXT("ReparentSaved"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Saved Blueprint"), Saved))
	{
		return false;
	}
	AddVariable(Saved, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Saved);
	TSharedPtr<FJsonObject> SaveParams = ReparentParams(Saved, Target);
	SetStrings(SaveParams, TEXT("acknowledged_collisions"), AcknowledgmentFor(Saved, Target));
	SaveParams->SetBoolField(TEXT("save"), true);
	const FCortexCommandResult SaveResult = FCortexBPReparentOps::Execute(SaveParams);
	if (TestTrue(TEXT("Reparent with save succeeds"), SaveResult.bSuccess) && SaveResult.Data.IsValid())
	{
		TestTrue(TEXT("saved reported"), SaveResult.Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Inline CDO delta is present without report_path"), SaveResult.Data->GetObjectField(TEXT("cdo_delta"))->HasField(TEXT("differences")));
	}
	TestTrue(TEXT("Package file written"), IFileManager::Get().FileExists(*PackageFile(Saved)));
	TestFalse(TEXT("Saved package is clean"), Saved->GetOutermost()->IsDirty());

	// save:false leaves the package dirty and nothing on disk.
	UBlueprint* Unsaved = MakeBlueprint(TEXT("ReparentUnsaved"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Unsaved Blueprint"), Unsaved))
	{
		return false;
	}
	AddVariable(Unsaved, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Unsaved);
	TSharedPtr<FJsonObject> UnsavedParams = ReparentParams(Unsaved, Target);
	SetStrings(UnsavedParams, TEXT("acknowledged_collisions"), AcknowledgmentFor(Unsaved, Target));
	const FCortexCommandResult UnsavedResult = FCortexBPReparentOps::Execute(UnsavedParams);
	if (TestTrue(TEXT("Reparent without save succeeds"), UnsavedResult.bSuccess) && UnsavedResult.Data.IsValid())
	{
		TestFalse(TEXT("saved is false"), UnsavedResult.Data->GetBoolField(TEXT("saved")));
	}
	TestTrue(TEXT("Package stays dirty"), Unsaved->GetOutermost()->IsDirty());
	TestFalse(TEXT("Nothing written"), IFileManager::Get().FileExists(*PackageFile(Unsaved)));

	// report_path writes the full delta and omits the inline list.
	UBlueprint* Reported = MakeBlueprint(TEXT("ReparentReported"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Reported Blueprint"), Reported))
	{
		return false;
	}
	AddVariable(Reported, TEXT("bPowerOn"), BoolPin(), TEXT("true"), /*bSaveGame=*/true);
	FKismetEditorUtilities::CompileBlueprint(Reported);
	TSharedPtr<FJsonObject> ReportParams = ReparentParams(Reported, Target);
	SetStrings(ReportParams, TEXT("acknowledged_collisions"), AcknowledgmentFor(Reported, Target));

	// A path outside the allowed roots refuses before anything is mutated.
	TSharedPtr<FJsonObject> OutsideParams = ReparentParams(Reported, Target);
	SetStrings(OutsideParams, TEXT("acknowledged_collisions"), AcknowledgmentFor(Reported, Target));
	OutsideParams->SetStringField(TEXT("report_path"), OutsidePath);
	const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Reported);
	const FCortexCommandResult OutsideResult = FCortexBPReparentOps::Execute(OutsideParams);
	TestFalse(TEXT("A report path outside the allowed roots refuses"), OutsideResult.bSuccess);
	TestTrue(TEXT("Parent unchanged by the refusal"), Reported->ParentClass == AActor::StaticClass());
	TestTrue(TEXT("Fingerprint unchanged by the refusal"),
		FingerprintBefore->GetStringField(TEXT("migration_state_hash")) == FCortexBPMigrationGuard::ComputeFingerprint(Reported)->GetStringField(TEXT("migration_state_hash")));
	TestFalse(TEXT("Nothing written outside the roots"), IFileManager::Get().FileExists(*OutsidePath));

	ReportParams->SetStringField(TEXT("report_path"), ReportPath);
	const FCortexCommandResult ReportResult = FCortexBPReparentOps::Execute(ReportParams);
	if (TestTrue(TEXT("Reparent with report_path succeeds"), ReportResult.bSuccess) && ReportResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (TestTrue(TEXT("cdo_delta present"), ReportResult.Data->TryGetObjectField(TEXT("cdo_delta"), Delta)) && Delta != nullptr)
		{
			TestFalse(TEXT("The inline list is omitted"), (*Delta)->HasField(TEXT("differences")));
			TestTrue(TEXT("The report path is echoed"), (*Delta)->HasField(TEXT("report_path")));
			TestTrue(TEXT("Counts stay inline"), (*Delta)->HasField(TEXT("counts")));
		}
	}
	FString Contents;
	if (TestTrue(TEXT("Report file written"), FFileHelper::LoadFileToString(Contents, *ReportPath)))
	{
		TSharedPtr<FJsonObject> Report;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (TestTrue(TEXT("Report parses"), FJsonSerializer::Deserialize(Reader, Report)) && Report.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Differences = nullptr;
			TestTrue(TEXT("Report holds the full difference list"),
				Report->TryGetArrayField(TEXT("differences"), Differences) && Differences != nullptr && Differences->Num() > 0);
		}
	}

	IFileManager::Get().Delete(*ReportPath, false, true, true);
	IFileManager::Get().Delete(*PackageFile(Saved), false, true, true);
	Discard(Reported);
	Discard(Unsaved);
	Discard(Saved);
	return true;
}
