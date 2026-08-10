#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
constexpr const TCHAR* CurveSourceSkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

class FCortexCurveWarningCapture final : public FOutputDevice
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

FCortexCommandRouter CreateCurveRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

FString MakeCurveSuffix()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
}

UAnimSequence* CreateCurveTestSequence(FAutomationTestBase& Test, FString& OutAssetPath)
{
	USkeleton* SourceSkeleton = LoadObject<USkeleton>(nullptr, CurveSourceSkeletonPath);
	Test.TestNotNull(TEXT("source skeleton exists"), SourceSkeleton);
	if (SourceSkeleton == nullptr)
	{
		return nullptr;
	}

	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimCurve_%s/AS_CurveTest"), *MakeCurveSuffix());
	UPackage* Package = CreatePackage(*PackageName);
	Test.TestNotNull(TEXT("test package created"), Package);
	if (Package == nullptr)
	{
		return nullptr;
	}

	UAnimSequence* Sequence = NewObject<UAnimSequence>(
		Package,
		UAnimSequence::StaticClass(),
		TEXT("AS_CurveTest"),
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
	FAssetRegistryModule::AssetCreated(Sequence);
	Package->SetDirtyFlag(false);
	OutAssetPath = Sequence->GetPathName();
	return Sequence;
}

void CleanupCurveTestSequence(UAnimSequence*& Sequence)
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

struct FCortexCurveTestSequenceGuard
{
	UAnimSequence* Sequence = nullptr;

	~FCortexCurveTestSequenceGuard()
	{
		CleanupCurveTestSequence(Sequence);
	}
};

TSharedPtr<FJsonObject> CurveFingerprintFor(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

TSharedPtr<FJsonObject> CurrentFingerprint(const FCortexCommandResult& Result)
{
	if (!Result.bSuccess || !Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("current_fingerprint")))
	{
		return MakeShared<FJsonObject>();
	}

	return Result.Data->GetObjectField(TEXT("current_fingerprint"));
}

TSharedPtr<FJsonObject> AddCurveParams(
	const FString& AssetPath,
	const FString& CurveName,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("curve_name"), CurveName);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonValue> CurveKey(double Time, double Value)
{
	TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
	Key->SetNumberField(TEXT("time"), Time);
	Key->SetNumberField(TEXT("value"), Value);
	return MakeShared<FJsonValueObject>(Key);
}

TSharedPtr<FJsonObject> CurveKeysParams(
	const FString& AssetPath,
	const FString& CurveName,
	const TArray<TSharedPtr<FJsonValue>>& Keys,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("curve_name"), CurveName);
	Params->SetArrayField(TEXT("keys"), Keys);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonObject> RemoveCurveParams(
	const FString& AssetPath,
	const FString& CurveName,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = AddCurveParams(AssetPath, CurveName, Fingerprint, bDryRun, bSave);
	return Params;
}

TSharedPtr<FJsonObject> SequenceInfoParams(
	const FString& AssetPath,
	bool bIncludeKeys = false,
	int32 CurveKeyLimit = 100,
	const FString& CurveName = FString())
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetBoolField(TEXT("include_curve_keys"), bIncludeKeys);
	Params->SetNumberField(TEXT("curve_key_limit"), CurveKeyLimit);
	if (!CurveName.IsEmpty())
	{
		Params->SetStringField(TEXT("curve_name"), CurveName);
	}
	return Params;
}

const TArray<TSharedPtr<FJsonValue>>* JsonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object.IsValid())
	{
		Object->TryGetArrayField(FieldName, Values);
	}
	return Values;
}

int32 JsonInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	double Value = 0.0;
	return Object.IsValid() && Object->TryGetNumberField(FieldName, Value) ? static_cast<int32>(Value) : INDEX_NONE;
}

int32 FloatCurveKeyCount(const UAnimSequence* Sequence, const FString& CurveName)
{
	if (Sequence == nullptr || Sequence->GetDataModel() == nullptr)
	{
		return INDEX_NONE;
	}

	for (const FFloatCurve& Curve : Sequence->GetDataModel()->GetCurveData().FloatCurves)
	{
		if (Curve.GetName().ToString() == CurveName)
		{
			return Curve.FloatCurve.GetConstRefOfKeys().Num();
		}
	}

	return INDEX_NONE;
}

void AddFloatCurveDirect(UAnimSequence* Sequence, const FString& CurveName, const TArray<TPair<double, double>>& Keys)
{
	IAnimationDataController& Controller = Sequence->GetController();
	const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
	Controller.AddCurve(CurveId, AACF_Editable, false);
	TArray<FRichCurveKey> CurveKeys;
	CurveKeys.Reserve(Keys.Num());
	for (const TPair<double, double>& Key : Keys)
	{
		CurveKeys.Add(FRichCurveKey(static_cast<float>(Key.Key), static_cast<float>(Key.Value)));
	}
	Controller.SetCurveKeys(CurveId, CurveKeys, false);
	Sequence->RefreshCacheData();
	Sequence->GetPackage()->SetDirtyFlag(false);
}

void AddTransformCurveDirect(UAnimSequence* Sequence, const FString& CurveName)
{
	FAnimationCurveData& CurveData = const_cast<FAnimationCurveData&>(Sequence->GetDataModel()->GetCurveData());
	CurveData.TransformCurves.Add(FTransformCurve(FName(*CurveName), AACF_Editable));
	Sequence->GetPackage()->SetDirtyFlag(false);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCurveAuthoringAddSetRemoveTest,
	"Cortex.Animation.CurveAuthoring.AddSetRemoveReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCurveAuthoringAddSetRemoveTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexCurveTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateCurveTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateCurveRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_AimOffset"), CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("add float curve succeeds"), Add.bSuccess);
	if (Add.bSuccess && Add.Data.IsValid())
	{
		TestEqual(TEXT("curve selector type"), Add.Data->GetObjectField(TEXT("selector"))->GetStringField(TEXT("curve_type")), FString(TEXT("float")));
		TestTrue(TEXT("add after exists"), Add.Data->GetObjectField(TEXT("after"))->GetBoolField(TEXT("exists")));
	}

	FCortexCommandResult Set = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_AimOffset"), { CurveKey(0.0, 0.0), CurveKey(0.5, 1.0) }, CurrentFingerprint(Add)));
	TestTrue(TEXT("set sorted finite keys succeeds"), Set.bSuccess);
	if (Set.bSuccess && Set.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Keys = JsonArray(Set.Data->GetObjectField(TEXT("after")), TEXT("keys"));
		TestEqual(TEXT("canonical after contains two keys"), Keys != nullptr ? Keys->Num() : INDEX_NONE, 2);
	}

	Sequence->GetPackage()->SetDirtyFlag(false);
	FCortexCommandResult IdenticalSet = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_AimOffset"), { CurveKey(0.0, 0.0), CurveKey(0.5, 1.0) }, CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("identical curve key set succeeds"), IdenticalSet.bSuccess);
	if (IdenticalSet.bSuccess && IdenticalSet.Data.IsValid())
	{
		TestFalse(TEXT("identical curve key set reports unchanged"), IdenticalSet.Data->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("identical curve key set does not dirty package"), Sequence->GetPackage()->IsDirty());
	}

	FCortexCommandResult Inspect = Router.Execute(
		TEXT("anim.get_sequence_info"),
		SequenceInfoParams(AssetPath, true, 10, TEXT("Cortex_AimOffset")));
	TestTrue(TEXT("inspection with key readback succeeds"), Inspect.bSuccess);
	if (Inspect.bSuccess && Inspect.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Curves = Inspect.Data->GetObjectField(TEXT("curves"));
		const TArray<TSharedPtr<FJsonValue>>* Items = JsonArray(Curves, TEXT("items"));
		TestEqual(TEXT("exact curve filter returns one curve"), Items != nullptr ? Items->Num() : INDEX_NONE, 1);
		if (Items != nullptr && Items->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Keys = (*Items)[0]->AsObject()->GetObjectField(TEXT("keys"));
			TestEqual(TEXT("curve has two total keys"), JsonInt(Keys, TEXT("count")), 2);
			TestEqual(TEXT("curve returns two keys"), JsonInt(Keys, TEXT("returned")), 2);
		}
	}

	FCortexCommandResult Remove = Router.Execute(
		TEXT("anim.remove_curve"),
		RemoveCurveParams(AssetPath, TEXT("Cortex_AimOffset"), CurrentFingerprint(IdenticalSet)));
	TestTrue(TEXT("remove curve succeeds"), Remove.bSuccess);
	if (Remove.bSuccess && Remove.Data.IsValid())
	{
		TestFalse(TEXT("remove after no longer exists"), Remove.Data->GetObjectField(TEXT("after"))->GetBoolField(TEXT("exists")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCurveAuthoringValidationTest,
	"Cortex.Animation.CurveAuthoring.ValidationAndDryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCurveAuthoringValidationTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexCurveTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateCurveTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateCurveRouter();
	FCortexCommandResult DryRun = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_DryRun"), CurveFingerprintFor(Sequence), true));
	TestTrue(TEXT("dry-run add succeeds"), DryRun.bSuccess);
	TestEqual(TEXT("dry-run does not add model curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 0);
	TestFalse(TEXT("dry-run does not dirty package"), Sequence->GetPackage()->IsDirty());

	TSharedPtr<FJsonObject> StaleFingerprint = CurveFingerprintFor(Sequence);
	StaleFingerprint->SetBoolField(TEXT("is_dirty"), true);
	FCortexCommandResult Stale = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_Stale"), StaleFingerprint));
	TestFalse(TEXT("stale add is rejected"), Stale.bSuccess);
	TestEqual(TEXT("stale add error code"), Stale.ErrorCode, FString(CortexErrorCodes::StalePrecondition));
	TestEqual(TEXT("stale add does not mutate"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 0);

	Sequence->GetPackage()->SetDirtyFlag(false);
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_Validate"), CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("baseline add succeeds"), Add.bSuccess);

	FCortexCommandResult Duplicate = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_Validate"), CurrentFingerprint(Add)));
	TestFalse(TEXT("duplicate float curve rejected"), Duplicate.bSuccess);
	TestEqual(TEXT("duplicate error code"), Duplicate.ErrorCode, FString(CortexErrorCodes::AssetAlreadyExists));

	FString CollisionAssetPath;
	FCortexCurveTestSequenceGuard CollisionGuard;
	UAnimSequence* CollisionSequence = CollisionGuard.Sequence = CreateCurveTestSequence(*this, CollisionAssetPath);
	TestNotNull(TEXT("transform collision sequence created"), CollisionSequence);
	if (CollisionSequence == nullptr)
	{
		return false;
	}
	AddTransformCurveDirect(CollisionSequence, TEXT("root"));
	FCortexCommandResult TransformCollision = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(CollisionAssetPath, TEXT("root"), CurveFingerprintFor(CollisionSequence)));
	TestFalse(TEXT("same-name transform curve collision rejected"), TransformCollision.bSuccess);
	TestEqual(TEXT("transform collision error code"), TransformCollision.ErrorCode, FString(CortexErrorCodes::InvalidField));

	FCortexCommandResult DuplicateTimes = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_Validate"), { CurveKey(0.0, 0.0), CurveKey(0.0, 1.0) }, CurveFingerprintFor(Sequence)));
	TestFalse(TEXT("duplicate key times rejected"), DuplicateTimes.bSuccess);
	TestEqual(TEXT("duplicate time error code"), DuplicateTimes.ErrorCode, FString(CortexErrorCodes::InvalidField));

	FCortexCommandResult Unsorted = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_Validate"), { CurveKey(0.5, 1.0), CurveKey(0.25, 0.5) }, CurveFingerprintFor(Sequence)));
	TestFalse(TEXT("unsorted key times rejected"), Unsorted.bSuccess);
	TestEqual(TEXT("unsorted error code"), Unsorted.ErrorCode, FString(CortexErrorCodes::InvalidField));

	TArray<TSharedPtr<FJsonValue>> TooManyKeys;
	for (int32 Index = 0; Index < 501; ++Index)
	{
		TooManyKeys.Add(CurveKey(Index / 1000.0, 0.0));
	}
	FCortexCommandResult TooMany = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_Validate"), TooManyKeys, CurveFingerprintFor(Sequence)));
	TestFalse(TEXT("more than 500 keys rejected"), TooMany.bSuccess);
	TestEqual(TEXT("too many keys error code"), TooMany.ErrorCode, FString(CortexErrorCodes::InvalidField));

	FCortexCommandResult OutOfRange = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_Validate"), { CurveKey(1.1, 1.0) }, CurveFingerprintFor(Sequence)));
	TestFalse(TEXT("out-of-range key time rejected"), OutOfRange.bSuccess);
	TestEqual(TEXT("out-of-range error code"), OutOfRange.ErrorCode, FString(CortexErrorCodes::InvalidField));

	FCortexCommandResult FloatOverflow = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_Validate"), { CurveKey(0.5, 1e300) }, CurveFingerprintFor(Sequence)));
	TestFalse(TEXT("key value that overflows float is rejected"), FloatOverflow.bSuccess);
	TestEqual(TEXT("float overflow error code"), FloatOverflow.ErrorCode, FString(CortexErrorCodes::InvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCurveAuthoringSaveReloadTest,
	"Cortex.Animation.CurveAuthoring.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCurveAuthoringSaveReloadTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexCurveTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateCurveTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateCurveRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_SaveReload"), CurveFingerprintFor(Sequence), false, true));
	TestTrue(TEXT("saved add succeeds"), Add.bSuccess);
	TestFalse(TEXT("saved add leaves package clean"), Sequence->GetPackage()->IsDirty());

	FCortexCommandResult Set = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_SaveReload"), { CurveKey(0.0, 2.0), CurveKey(1.0, 4.0) }, CurrentFingerprint(Add), false, true));
	TestTrue(TEXT("saved set succeeds"), Set.bSuccess);
	TestFalse(TEXT("saved set leaves package clean"), Sequence->GetPackage()->IsDirty());
	if (Set.bSuccess && Set.Data.IsValid())
	{
		bool bSaved = false;
		TestTrue(TEXT("save=true reports saved"), Set.Data->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
		TestTrue(TEXT("saved packages returned"), Set.Data->HasTypedField<EJson::Array>(TEXT("saved_packages")));
	}

	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	Guard.Sequence = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UAnimSequence* Reloaded = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	Guard.Sequence = Reloaded;
	TestNotNull(TEXT("saved curve reloads as a new object"), Reloaded);
	if (Reloaded == nullptr)
	{
		return false;
	}

	FCortexCommandResult Inspect = Router.Execute(
		TEXT("anim.get_sequence_info"),
		SequenceInfoParams(AssetPath, true, 10, TEXT("Cortex_SaveReload")));
	TestTrue(TEXT("saved curve can be read back"), Inspect.bSuccess);
	if (Inspect.bSuccess && Inspect.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = JsonArray(Inspect.Data->GetObjectField(TEXT("curves")), TEXT("items"));
		TestEqual(TEXT("saved curve inspection returns one curve"), Items != nullptr ? Items->Num() : INDEX_NONE, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCurveAuthoringUndoRedoTest,
	"Cortex.Animation.CurveAuthoring.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCurveAuthoringUndoRedoTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Curve Undo Setup")));

	FString AssetPath;
	FCortexCurveTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateCurveTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateCurveRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_curve"),
		AddCurveParams(AssetPath, TEXT("Cortex_UndoCurve"), CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("add succeeds"), Add.bSuccess);
	TestEqual(TEXT("add created curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 1);
	TestTrue(TEXT("undo add succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo add removes curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 0);
	TestTrue(TEXT("redo add succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo add restores curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 1);

	FCortexCommandResult Set = Router.Execute(
		TEXT("anim.set_curve_keys"),
		CurveKeysParams(AssetPath, TEXT("Cortex_UndoCurve"), { CurveKey(0.0, 0.0), CurveKey(0.5, 1.0) }, CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("set succeeds"), Set.bSuccess);
	TestEqual(TEXT("set creates two keys"), FloatCurveKeyCount(Sequence, TEXT("Cortex_UndoCurve")), 2);
	TestTrue(TEXT("undo set succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo set restores empty key state"), FloatCurveKeyCount(Sequence, TEXT("Cortex_UndoCurve")), 0);
	TestTrue(TEXT("redo set succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo set restores two-key state"), FloatCurveKeyCount(Sequence, TEXT("Cortex_UndoCurve")), 2);

	FCortexCommandResult Remove = Router.Execute(
		TEXT("anim.remove_curve"),
		RemoveCurveParams(AssetPath, TEXT("Cortex_UndoCurve"), CurveFingerprintFor(Sequence)));
	TestTrue(TEXT("remove succeeds"), Remove.bSuccess);
	TestEqual(TEXT("remove deletes curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 0);
	TestTrue(TEXT("undo remove succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo remove restores curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 1);
	TestTrue(TEXT("redo remove succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo remove deletes curve"), Sequence->GetDataModel()->GetNumberOfFloatCurves(), 0);

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Curve Undo Cleanup")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationCurveInspectionKeyBudgetTest,
	"Cortex.Animation.CurveAuthoring.InspectionKeyBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationCurveInspectionKeyBudgetTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexCurveTestSequenceGuard Guard;
	UAnimSequence* Sequence = Guard.Sequence = CreateCurveTestSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCurveWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);
	{
		IAnimationDataController::FScopedBracket Bracket(Sequence->GetController(), FText::FromString(TEXT("Cortex: Populate Test Float Curves")), false);
		AddFloatCurveDirect(Sequence, TEXT("Cortex_B"), { { 0.0, 0.0 }, { 0.25, 1.0 }, { 0.5, 2.0 } });
		AddFloatCurveDirect(Sequence, TEXT("Cortex_A"), { { 0.0, 10.0 }, { 0.5, 20.0 } });
	}

	FCortexCommandRouter Router = CreateCurveRouter();
	FCortexCommandResult Inspect = Router.Execute(
		TEXT("anim.get_sequence_info"),
		SequenceInfoParams(AssetPath, true, 3));
	TestTrue(TEXT("inspection with bounded key readback succeeds"), Inspect.bSuccess);
	if (!Inspect.bSuccess || !Inspect.Data.IsValid())
	{
		return true;
	}

	const TSharedPtr<FJsonObject> Curves = Inspect.Data->GetObjectField(TEXT("curves"));
	const TArray<TSharedPtr<FJsonValue>>* Items = JsonArray(Curves, TEXT("items"));
	TestTrue(TEXT("curve items returned"), Items != nullptr && Items->Num() >= 2);
	if (Items == nullptr || Items->Num() < 2)
	{
		return true;
	}

	const TSharedPtr<FJsonObject> First = (*Items)[0]->AsObject();
	const TSharedPtr<FJsonObject> Second = (*Items)[1]->AsObject();
	TestEqual(TEXT("curves are sorted by name"), First->GetStringField(TEXT("name")), FString(TEXT("Cortex_A")));
	TestEqual(TEXT("second sorted curve"), Second->GetStringField(TEXT("name")), FString(TEXT("Cortex_B")));

	const TSharedPtr<FJsonObject> FirstKeys = First->GetObjectField(TEXT("keys"));
	const TSharedPtr<FJsonObject> SecondKeys = Second->GetObjectField(TEXT("keys"));
	TestEqual(TEXT("first curve total count"), JsonInt(FirstKeys, TEXT("count")), 2);
	TestEqual(TEXT("first curve returned count"), JsonInt(FirstKeys, TEXT("returned")), 2);
	TestFalse(TEXT("first curve not truncated"), FirstKeys->GetBoolField(TEXT("truncated")));
	TestEqual(TEXT("second curve total count"), JsonInt(SecondKeys, TEXT("count")), 3);
	TestEqual(TEXT("second curve receives remaining global budget"), JsonInt(SecondKeys, TEXT("returned")), 1);
	TestTrue(TEXT("second curve truncated"), SecondKeys->GetBoolField(TEXT("truncated")));
	TestEqual(TEXT("global returned key budget"), JsonInt(FirstKeys, TEXT("returned")) + JsonInt(SecondKeys, TEXT("returned")), 3);

	FCortexCommandResult NoKeys = Router.Execute(
		TEXT("anim.get_sequence_info"),
		SequenceInfoParams(AssetPath, false, 3));
	TestTrue(TEXT("default inspection succeeds"), NoKeys.bSuccess);
	if (NoKeys.bSuccess && NoKeys.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NoKeyItems = JsonArray(NoKeys.Data->GetObjectField(TEXT("curves")), TEXT("items"));
		TestTrue(TEXT("curve items present without key arrays"), NoKeyItems != nullptr && NoKeyItems->Num() >= 1);
		if (NoKeyItems != nullptr && NoKeyItems->Num() >= 1)
		{
			TestFalse(TEXT("key arrays omitted by default"), (*NoKeyItems)[0]->AsObject()->HasField(TEXT("keys")));
		}
	}
	GLog->RemoveOutputDevice(&Capture);
	TestEqual(TEXT("curve inspection fixture is warning-clean"), Capture.WarningCount, 0);
	return true;
}
