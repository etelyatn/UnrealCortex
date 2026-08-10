#include "Misc/AutomationTest.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
constexpr const TCHAR* SourceSkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

class FCortexAnimWarningCapture final : public FOutputDevice
{
public:
	int32 WarningCount = 0;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		(void)V;
		(void)Category;

		const ELogVerbosity::Type VerbosityLevel =
			static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (VerbosityLevel == ELogVerbosity::Warning)
		{
			++WarningCount;
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}
};

FCortexCommandRouter CreateAnimAuthorRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

FString MakeSuffix()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
}

UAnimSequence* CreateNotifyTestSequence(FAutomationTestBase& Test, FString& OutAssetPath)
{
	USkeleton* SourceSkeleton = LoadObject<USkeleton>(nullptr, SourceSkeletonPath);
	Test.TestNotNull(TEXT("source skeleton exists"), SourceSkeleton);
	if (SourceSkeleton == nullptr)
	{
		return nullptr;
	}

	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimNotify_%s/AS_NotifyTest"), *MakeSuffix());
	UPackage* Package = CreatePackage(*PackageName);
	Test.TestNotNull(TEXT("test package created"), Package);
	if (Package == nullptr)
	{
		return nullptr;
	}

	UAnimSequence* Sequence = NewObject<UAnimSequence>(
		Package,
		UAnimSequence::StaticClass(),
		TEXT("AS_NotifyTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	Test.TestNotNull(TEXT("test sequence created"), Sequence);
	if (Sequence == nullptr)
	{
		return nullptr;
	}

	Sequence->SetSkeleton(SourceSkeleton);
	Sequence->AssetImportData = NewObject<UAssetImportData>(Sequence, TEXT("AssetImportData"));
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	Sequence->Notifies.Reset();
	Sequence->SortNotifies();
	Sequence->RefreshCacheData();
	FAssetRegistryModule::AssetCreated(Sequence);
	Package->SetDirtyFlag(false);
	OutAssetPath = Sequence->GetPathName();
	return Sequence;
}

void CleanupNotifyTestSequence(UAnimSequence*& Sequence)
{
	if (Sequence == nullptr)
	{
		return;
	}

	UPackage* Package = Sequence->GetPackage();
	const FString PackageName = Package != nullptr ? Package->GetName() : FString();

	if (!PackageName.IsEmpty())
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (FPaths::FileExists(Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true, true);
		}
		const FString Directory = FPaths::GetPath(Filename);
		if (FPaths::DirectoryExists(Directory))
		{
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}
	}

	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	if (Package != nullptr)
	{
		Package->SetDirtyFlag(false);
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	Sequence = nullptr;
}

struct FCortexNotifyTestSequenceGuard
{
	UAnimSequence* Sequence = nullptr;

	~FCortexNotifyTestSequenceGuard()
	{
		CleanupNotifyTestSequence(Sequence);
	}
};

TSharedPtr<FJsonObject> FingerprintFor(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

TSharedPtr<FJsonObject> NotifySelector(int32 Index, const FString& Name, double Time)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetNumberField(TEXT("index"), Index);
	Selector->SetStringField(TEXT("name"), Name);
	Selector->SetNumberField(TEXT("time"), Time);
	return Selector;
}

TSharedPtr<FJsonObject> AddParams(
	const FString& AssetPath,
	const FString& Name,
	double Time,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("notify_name"), Name);
	Params->SetNumberField(TEXT("time"), Time);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

bool TryGetCurrentFingerprint(const FCortexCommandResult& Result, TSharedPtr<FJsonObject>& OutFingerprint)
{
	if (!Result.bSuccess || !Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("current_fingerprint")))
	{
		return false;
	}

	OutFingerprint = Result.Data->GetObjectField(TEXT("current_fingerprint"));
	return OutFingerprint.IsValid();
}

void AddObjectNotify(UAnimSequence* Sequence, const FName& Name, float Time)
{
	FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
	Event.NotifyName = Name;
	Event.Notify = NewObject<UAnimNotify_PlaySound>(Sequence, UAnimNotify_PlaySound::StaticClass(), NAME_None, RF_Transactional);
	Event.NotifyStateClass = nullptr;
	Event.Link(Sequence, Time);
	Event.TriggerTimeOffset = Sequence->CalculateOffsetForNotify(Time);
	Event.TrackIndex = 0;
	Event.Guid = FGuid::NewGuid();
	Sequence->SortNotifies();
	Sequence->RefreshCacheData();
	Sequence->GetPackage()->SetDirtyFlag(false);
}

void AddNotifyState(UAnimSequence* Sequence, const FName& Name, float Time)
{
	FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
	Event.NotifyName = Name;
	Event.Notify = nullptr;
	Event.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(
		Sequence,
		UAnimNotifyState_TimedParticleEffect::StaticClass(),
		NAME_None,
		RF_Transactional);
	Event.Link(Sequence, Time);
	Event.EndLink.Link(Sequence, Time + 0.1f);
	Event.SetDuration(0.1f);
	Event.TriggerTimeOffset = Sequence->CalculateOffsetForNotify(Time);
	Event.TrackIndex = 0;
	Event.Guid = FGuid::NewGuid();
	Sequence->SortNotifies();
	Sequence->RefreshCacheData();
	Sequence->GetPackage()->SetDirtyFlag(false);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyDryRunAddTest,
	"Cortex.Animation.NotifyAuthoring.DryRunAddDoesNotDirty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyDryRunAddTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	FCortexCommandResult Result = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_DryRun"), 0.1, FingerprintFor(Sequence), true));

	TestTrue(TEXT("dry-run add succeeds"), Result.bSuccess);
	TestEqual(TEXT("sequence still has no notifies"), Sequence->Notifies.Num(), 0);
	TestEqual(TEXT("dry-run does not dirty package"), Sequence->GetPackage()->IsDirty(), bDirtyBefore);
	TestTrue(TEXT("response contains before state"), Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("before")));
	TestTrue(TEXT("response contains planned after state"), Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("after")));
	TestTrue(TEXT("response contains current fingerprint"), Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("current_fingerprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyAddStaleAndSaveTest,
	"Cortex.Animation.NotifyAuthoring.AddStaleAndSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyAddStaleAndSaveTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	TSharedPtr<FJsonObject> Expected = FingerprintFor(Sequence);
	Sequence->Modify();
	Sequence->MarkPackageDirty();

	FCortexCommandResult Stale = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_Stale"), 0.1, Expected));
	TestFalse(TEXT("stale add is rejected"), Stale.bSuccess);
	TestEqual(TEXT("stale add error code"), Stale.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
	TestEqual(TEXT("stale rejection does not mutate"), Sequence->Notifies.Num(), 0);

	Sequence->GetPackage()->SetDirtyFlag(false);
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_Save"), 0.1, FingerprintFor(Sequence), false, true));

	TestTrue(TEXT("add succeeds"), Add.bSuccess);
	TestEqual(TEXT("sequence has one notify"), Sequence->Notifies.Num(), 1);
	TestEqual(TEXT("saved add leaves package clean"), Sequence->GetPackage()->IsDirty(), false);
	bool bSaved = false;
	TestTrue(TEXT("saved field exists"), Add.Data.IsValid() && Add.Data->TryGetBoolField(TEXT("saved"), bSaved));
	TestTrue(TEXT("save=true reports saved"), bSaved);
	TestTrue(TEXT("saved packages returned"), Add.Data.IsValid() && Add.Data->HasTypedField<EJson::Array>(TEXT("saved_packages")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyUpdatePreciseSelectorTest,
	"Cortex.Animation.NotifyAuthoring.UpdateUsesPreciseSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyUpdatePreciseSelectorTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	FCortexCommandResult AddA = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_Dupe"), 0.1, FingerprintFor(Sequence)));
	TSharedPtr<FJsonObject> Fingerprint;
	TestTrue(TEXT("first add fingerprint"), TryGetCurrentFingerprint(AddA, Fingerprint));
	FCortexCommandResult AddB = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_Dupe"), 0.2, Fingerprint));
	TestTrue(TEXT("second add succeeds"), AddB.bSuccess);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), NotifySelector(1, TEXT("Cortex_Dupe"), 0.2));
	Params->SetStringField(TEXT("new_name"), TEXT("Cortex_Updated"));
	Params->SetObjectField(TEXT("expected_fingerprint"), AddB.Data->GetObjectField(TEXT("current_fingerprint")));

	FCortexCommandResult Update = Router.Execute(TEXT("anim.update_named_notify"), Params);
	TestTrue(TEXT("update succeeds"), Update.bSuccess);
	TestEqual(TEXT("first duplicate unchanged"), Sequence->Notifies[0].NotifyName, FName(TEXT("Cortex_Dupe")));
	TestEqual(TEXT("selected duplicate renamed"), Sequence->Notifies[1].NotifyName, FName(TEXT("Cortex_Updated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyRemovePreciseSelectorTest,
	"Cortex.Animation.NotifyAuthoring.RemoveUsesPreciseSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyRemovePreciseSelectorTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	FCortexCommandResult AddA = Router.Execute(TEXT("anim.add_named_notify"), AddParams(AssetPath, TEXT("Cortex_Remove"), 0.1, FingerprintFor(Sequence)));
	FCortexCommandResult AddB = Router.Execute(TEXT("anim.add_named_notify"), AddParams(AssetPath, TEXT("Cortex_Remove"), 0.2, AddA.Data->GetObjectField(TEXT("current_fingerprint"))));

	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveParams->SetObjectField(TEXT("selector"), NotifySelector(0, TEXT("Cortex_Remove"), 0.1));
	RemoveParams->SetObjectField(TEXT("expected_fingerprint"), AddB.Data->GetObjectField(TEXT("current_fingerprint")));

	FCortexCommandResult Remove = Router.Execute(TEXT("anim.remove_named_notify"), RemoveParams);
	TestTrue(TEXT("remove succeeds"), Remove.bSuccess);
	TestEqual(TEXT("only one duplicate remains"), Sequence->Notifies.Num(), 1);
	TestEqual(TEXT("remaining duplicate is the later notify"), Sequence->Notifies[0].GetTime(), 0.2f);

	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("asset_path"), AssetPath);
	MissingParams->SetObjectField(TEXT("selector"), NotifySelector(42, TEXT("Cortex_Remove"), 0.1));
	MissingParams->SetObjectField(TEXT("expected_fingerprint"), Remove.Data->GetObjectField(TEXT("current_fingerprint")));
	FCortexCommandResult Missing = Router.Execute(TEXT("anim.remove_named_notify"), MissingParams);
	TestFalse(TEXT("missing remove target is explicit error"), Missing.bSuccess);
	TestEqual(TEXT("missing target error code"), Missing.ErrorCode, FString(CortexErrorCodes::AssetNotFound));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyValidationWarningCleanTest,
	"Cortex.Animation.NotifyAuthoring.ValidationWarningClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyValidationWarningCleanTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	FCortexAnimWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);

	FCortexCommandResult InvalidTime = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(AssetPath, TEXT("Cortex_Invalid"), -1.0, FingerprintFor(Sequence)));
	FCortexCommandResult Missing = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(TEXT("/Game/Temp/CortexAnimNotify_NoSuchAsset"), TEXT("Cortex_Missing"), 0.1, FingerprintFor(Sequence)));
	FCortexCommandResult WrongClass = Router.Execute(
		TEXT("anim.add_named_notify"),
		AddParams(TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin"), TEXT("Cortex_Wrong"), 0.1, FingerprintFor(Sequence)));

	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("invalid time fails"), InvalidTime.bSuccess);
	TestFalse(TEXT("missing asset fails"), Missing.bSuccess);
	TestFalse(TEXT("wrong class fails"), WrongClass.bSuccess);
	TestEqual(TEXT("bad inputs do not mutate"), Sequence->Notifies.Num(), 0);
	TestEqual(TEXT("bad inputs are warning-clean"), Capture.WarningCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyStrictBoolValidationTest,
	"Cortex.Animation.NotifyAuthoring.StrictBoolValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyStrictBoolValidationTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();

	TSharedPtr<FJsonObject> BadDryRun = AddParams(AssetPath, TEXT("Cortex_BadDryRun"), 0.1, FingerprintFor(Sequence));
	BadDryRun->SetStringField(TEXT("dry_run"), TEXT("true"));
	FCortexCommandResult DryRunResult = Router.Execute(TEXT("anim.add_named_notify"), BadDryRun);
	TestFalse(TEXT("string dry_run fails"), DryRunResult.bSuccess);
	TestEqual(TEXT("string dry_run returns invalid field"), DryRunResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("string dry_run does not mutate"), Sequence->Notifies.Num(), 0);

	TSharedPtr<FJsonObject> BadSave = AddParams(AssetPath, TEXT("Cortex_BadSave"), 0.1, FingerprintFor(Sequence));
	BadSave->SetStringField(TEXT("save"), TEXT("true"));
	FCortexCommandResult SaveResult = Router.Execute(TEXT("anim.add_named_notify"), BadSave);
	TestFalse(TEXT("string save fails"), SaveResult.bSuccess);
	TestEqual(TEXT("string save returns invalid field"), SaveResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("string save does not mutate"), Sequence->Notifies.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyRejectsObjectAndStateTargetsTest,
	"Cortex.Animation.NotifyAuthoring.RejectsObjectAndStateTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyRejectsObjectAndStateTargetsTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	AddObjectNotify(Sequence, FName(TEXT("Cortex_ObjectNotify")), 0.1f);
	AddNotifyState(Sequence, FName(TEXT("Cortex_StateNotify")), 0.2f);
	const int32 OriginalNotifyCount = Sequence->Notifies.Num();

	FCortexCommandRouter Router = CreateAnimAuthorRouter();

	TSharedPtr<FJsonObject> UpdateObjectParams = MakeShared<FJsonObject>();
	UpdateObjectParams->SetStringField(TEXT("asset_path"), AssetPath);
	UpdateObjectParams->SetObjectField(TEXT("selector"), NotifySelector(0, TEXT("Cortex_ObjectNotify"), 0.1));
	UpdateObjectParams->SetStringField(TEXT("new_name"), TEXT("Cortex_ObjectMutated"));
	UpdateObjectParams->SetObjectField(TEXT("expected_fingerprint"), FingerprintFor(Sequence));
	FCortexCommandResult UpdateObject = Router.Execute(TEXT("anim.update_named_notify"), UpdateObjectParams);
	TestFalse(TEXT("object notify update rejected"), UpdateObject.bSuccess);
	TestEqual(TEXT("object notify update code"), UpdateObject.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("object notify name unchanged"), Sequence->Notifies[0].NotifyName, FName(TEXT("Cortex_ObjectNotify")));

	TSharedPtr<FJsonObject> RemoveStateParams = MakeShared<FJsonObject>();
	RemoveStateParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveStateParams->SetObjectField(TEXT("selector"), NotifySelector(1, TEXT("Cortex_StateNotify"), 0.2));
	RemoveStateParams->SetObjectField(TEXT("expected_fingerprint"), FingerprintFor(Sequence));
	FCortexCommandResult RemoveState = Router.Execute(TEXT("anim.remove_named_notify"), RemoveStateParams);
	TestFalse(TEXT("notify state remove rejected"), RemoveState.bSuccess);
	TestEqual(TEXT("notify state remove code"), RemoveState.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("notify state not removed"), Sequence->Notifies.Num(), OriginalNotifyCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNamedNotifyUndoRedoTest,
	"Cortex.Animation.NotifyAuthoring.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNamedNotifyUndoRedoTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Notify Undo Setup")));

	FString AssetPath;
	FCortexNotifyTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateNotifyTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateAnimAuthorRouter();
	FCortexCommandResult Add = Router.Execute(TEXT("anim.add_named_notify"), AddParams(AssetPath, TEXT("Cortex_Undo"), 0.1, FingerprintFor(Sequence)));
	TestTrue(TEXT("add succeeds"), Add.bSuccess);
	TestEqual(TEXT("add created notify"), Sequence->Notifies.Num(), 1);
	TestTrue(TEXT("undo add succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo add removes notify"), Sequence->Notifies.Num(), 0);
	TestTrue(TEXT("redo add succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo add restores notify"), Sequence->Notifies.Num(), 1);

	TSharedPtr<FJsonObject> UpdateParams = MakeShared<FJsonObject>();
	UpdateParams->SetStringField(TEXT("asset_path"), AssetPath);
	UpdateParams->SetObjectField(TEXT("selector"), NotifySelector(0, TEXT("Cortex_Undo"), 0.1));
	UpdateParams->SetStringField(TEXT("new_name"), TEXT("Cortex_UndoUpdated"));
	UpdateParams->SetObjectField(TEXT("expected_fingerprint"), FingerprintFor(Sequence));
	FCortexCommandResult Update = Router.Execute(TEXT("anim.update_named_notify"), UpdateParams);
	TestTrue(TEXT("update succeeds"), Update.bSuccess);
	TestEqual(TEXT("update renamed notify"), Sequence->Notifies[0].NotifyName, FName(TEXT("Cortex_UndoUpdated")));
	TestTrue(TEXT("undo update succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo update restores name"), Sequence->Notifies[0].NotifyName, FName(TEXT("Cortex_Undo")));
	TestTrue(TEXT("redo update succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo update reapplies name"), Sequence->Notifies[0].NotifyName, FName(TEXT("Cortex_UndoUpdated")));

	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveParams->SetObjectField(TEXT("selector"), NotifySelector(0, TEXT("Cortex_UndoUpdated"), 0.1));
	RemoveParams->SetObjectField(TEXT("expected_fingerprint"), FingerprintFor(Sequence));
	FCortexCommandResult Remove = Router.Execute(TEXT("anim.remove_named_notify"), RemoveParams);
	TestTrue(TEXT("remove succeeds"), Remove.bSuccess);
	TestEqual(TEXT("remove deletes notify"), Sequence->Notifies.Num(), 0);
	TestTrue(TEXT("undo remove succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo remove restores notify"), Sequence->Notifies.Num(), 1);
	TestTrue(TEXT("redo remove succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo remove deletes notify"), Sequence->Notifies.Num(), 0);

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Notify Undo Cleanup")));
	return true;
}
