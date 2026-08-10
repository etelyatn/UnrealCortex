#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
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
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
constexpr const TCHAR* MontageSourceSkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

class FCortexMontageWarningCapture final : public FOutputDevice
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

FCortexCommandRouter CreateMontageRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

FString MakeMontageSuffix()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
}

void CleanupMontageTestAssets(UAnimMontage*& Montage, UAnimSequence*& Sequence)
{
	UPackage* Package = Montage != nullptr ? Montage->GetPackage() : (Sequence != nullptr ? Sequence->GetPackage() : nullptr);
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

	if (Montage != nullptr)
	{
		Montage->ClearFlags(RF_Public | RF_Standalone);
		Montage->MarkAsGarbage();
	}
	if (Sequence != nullptr)
	{
		Sequence->ClearFlags(RF_Public | RF_Standalone);
		Sequence->MarkAsGarbage();
	}
	if (Package != nullptr)
	{
		Package->SetDirtyFlag(false);
	}

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Montage = nullptr;
	Sequence = nullptr;
}

struct FCortexMontageTestAssetGuard
{
	UAnimMontage* Montage = nullptr;
	UAnimSequence* Sequence = nullptr;

	~FCortexMontageTestAssetGuard()
	{
		CleanupMontageTestAssets(Montage, Sequence);
	}
};

UAnimMontage* CreateMontageTestAsset(FAutomationTestBase& Test, FCortexMontageTestAssetGuard& Guard, FString& OutAssetPath)
{
	USkeleton* SourceSkeleton = LoadObject<USkeleton>(nullptr, MontageSourceSkeletonPath);
	Test.TestNotNull(TEXT("source skeleton exists"), SourceSkeleton);
	if (SourceSkeleton == nullptr)
	{
		return nullptr;
	}

	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimMontage_%s/AM_MontageTest"), *MakeMontageSuffix());
	UPackage* Package = CreatePackage(*PackageName);
	Test.TestNotNull(TEXT("test package created"), Package);
	if (Package == nullptr)
	{
		return nullptr;
	}

	UAnimSequence* Sequence = NewObject<UAnimSequence>(
		Package,
		UAnimSequence::StaticClass(),
		TEXT("AS_MontageSource"),
		RF_Public | RF_Standalone | RF_Transactional);
	Test.TestNotNull(TEXT("test montage source sequence created"), Sequence);
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

	UAnimMontage* Montage = NewObject<UAnimMontage>(
		Package,
		UAnimMontage::StaticClass(),
		TEXT("AM_MontageTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	Test.TestNotNull(TEXT("test montage created"), Montage);
	if (Montage == nullptr)
	{
		return nullptr;
	}

	Montage->SetSkeleton(SourceSkeleton);
	Montage->SlotAnimTracks.Reset();
	FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	SlotTrack.SlotName = FName(TEXT("DefaultSlot"));
	FAnimSegment& Segment = SlotTrack.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Sequence);
	Segment.StartPos = 0.0f;
	Segment.AnimStartTime = 0.0f;
	Segment.AnimEndTime = 1.0f;
	Segment.AnimPlayRate = 1.0f;
	Segment.LoopingCount = 1;
	Montage->CalculateSequenceLength();
	Montage->UpdateLinkableElements();
	Montage->RefreshCacheData();
	FAssetRegistryModule::AssetCreated(Sequence);
	FAssetRegistryModule::AssetCreated(Montage);
	Package->SetDirtyFlag(false);

	Guard.Sequence = Sequence;
	Guard.Montage = Montage;
	OutAssetPath = Montage->GetPathName();
	return Montage;
}

TSharedPtr<FJsonObject> MontageFingerprintFor(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

TSharedPtr<FJsonObject> MontageCurrentFingerprint(const FCortexCommandResult& Result)
{
	if (!Result.bSuccess || !Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("current_fingerprint")))
	{
		return MakeShared<FJsonObject>();
	}

	return Result.Data->GetObjectField(TEXT("current_fingerprint"));
}

TSharedPtr<FJsonObject> SectionSelector(int32 Index, const FString& Name, double StartTime)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetNumberField(TEXT("index"), Index);
	Selector->SetStringField(TEXT("name"), Name);
	Selector->SetNumberField(TEXT("start_time"), StartTime);
	return Selector;
}

TSharedPtr<FJsonObject> AddSectionParams(
	const FString& AssetPath,
	const FString& Name,
	double StartTime,
	const FString& NextSection,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("name"), Name);
	Params->SetNumberField(TEXT("start_time"), StartTime);
	Params->SetStringField(TEXT("next_section"), NextSection);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonObject> UpdateSectionParams(
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& Selector,
	const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), Selector);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("save"), false);
	return Params;
}

TSharedPtr<FJsonObject> RemoveSectionParams(
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& Selector,
	const TSharedPtr<FJsonObject>& Fingerprint,
	bool bDryRun = false,
	bool bSave = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), Selector);
	Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	Params->SetBoolField(TEXT("dry_run"), bDryRun);
	Params->SetBoolField(TEXT("save"), bSave);
	return Params;
}

TSharedPtr<FJsonObject> MontageInfoParams(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetNumberField(TEXT("section_limit"), 20);
	return Params;
}

const TArray<TSharedPtr<FJsonValue>>* MontageJsonArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object.IsValid())
	{
		Object->TryGetArrayField(FieldName, Values);
	}
	return Values;
}

int32 SectionIndexByName(const UAnimMontage* Montage, const FString& Name)
{
	if (Montage == nullptr)
	{
		return INDEX_NONE;
	}

	for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
	{
		if (Montage->CompositeSections[Index].SectionName.ToString() == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void AddSectionDirect(UAnimMontage* Montage, const FString& Name, double StartTime, const FString& NextSection = FString())
{
	FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
	Section.SectionName = FName(*Name);
	Section.NextSectionName = NextSection.IsEmpty() ? NAME_None : FName(*NextSection);
	Section.Link(Montage, static_cast<float>(StartTime));
	Montage->CompositeSections.Sort([](const FCompositeSection& Left, const FCompositeSection& Right)
	{
		return Left.GetTime() < Right.GetTime();
	});
	Montage->UpdateLinkableElements();
	Montage->RefreshCacheData();
	Montage->GetPackage()->SetDirtyFlag(false);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageAuthoringAddReadbackTest,
	"Cortex.Animation.MontageAuthoring.AddReadbackAndLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageAuthoringAddReadbackTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexMontageTestAssetGuard Guard;
	UAnimMontage* Montage = CreateMontageTestAsset(*this, Guard, AssetPath);
	if (Montage == nullptr)
	{
		return false;
	}

	AddSectionDirect(Montage, TEXT("Cortex_A"), 0.5);
	FCortexCommandRouter Router = CreateMontageRouter();

	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_B"), 0.25, TEXT(""), MontageFingerprintFor(Montage)));
	TestTrue(TEXT("add section succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		TestEqual(TEXT("red add fails because command is not registered yet"), Add.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
	}
	if (!Add.bSuccess || !Add.Data.IsValid())
	{
		return true;
	}

	const TSharedPtr<FJsonObject> After = Add.Data->GetObjectField(TEXT("after"));
	const TSharedPtr<FJsonObject> Selector = Add.Data->GetObjectField(TEXT("selector"));
	TestTrue(TEXT("add after exists"), After->GetBoolField(TEXT("exists")));
	TestEqual(TEXT("new section is canonically sorted before original"), static_cast<int32>(After->GetNumberField(TEXT("index"))), 0);
	TestTrue(TEXT("add selector includes canonical index"), Selector->HasTypedField<EJson::Number>(TEXT("index")));
	if (Selector->HasTypedField<EJson::Number>(TEXT("index")))
	{
		TestEqual(TEXT("add selector uses canonical index"), static_cast<int32>(Selector->GetNumberField(TEXT("index"))), 0);
	}
	TestEqual(TEXT("new section name"), After->GetStringField(TEXT("name")), FString(TEXT("Cortex_B")));
	TestEqual(TEXT("explicit empty next link is preserved"), After->GetStringField(TEXT("next_section")), FString());
	TestEqual(TEXT("implicit predecessor link was not introduced"), Montage->CompositeSections[0].NextSectionName, NAME_None);

	FCortexCommandResult Inspect = Router.Execute(TEXT("anim.get_montage_info"), MontageInfoParams(AssetPath));
	TestTrue(TEXT("montage inspection succeeds"), Inspect.bSuccess);
	if (Inspect.bSuccess && Inspect.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = MontageJsonArray(Inspect.Data->GetObjectField(TEXT("sections")), TEXT("items"));
		TestEqual(TEXT("inspection returns two sections"), Items != nullptr ? Items->Num() : INDEX_NONE, 2);
		if (Items != nullptr && Items->Num() == 2)
		{
			TestEqual(TEXT("inspection uses start_time field"), (*Items)[0]->AsObject()->GetNumberField(TEXT("start_time")), 0.25);
			TestFalse(TEXT("inspection no longer exposes time field"), (*Items)[0]->AsObject()->HasField(TEXT("time")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageAuthoringValidationTest,
	"Cortex.Animation.MontageAuthoring.ValidationDryRunAndReferenceGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageAuthoringValidationTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexMontageTestAssetGuard Guard;
	UAnimMontage* Montage = CreateMontageTestAsset(*this, Guard, AssetPath);
	if (Montage == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateMontageRouter();
	FCortexCommandResult DryRun = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_DryRun"), 0.1, TEXT(""), MontageFingerprintFor(Montage), true));
	TestTrue(TEXT("dry-run add succeeds"), DryRun.bSuccess);
	if (!DryRun.bSuccess)
	{
		TestEqual(TEXT("red dry-run fails because command is not registered yet"), DryRun.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
		return true;
	}
	TestEqual(TEXT("dry-run does not mutate sections"), Montage->CompositeSections.Num(), 0);
	TestFalse(TEXT("dry-run does not dirty package"), Montage->GetPackage()->IsDirty());
	if (DryRun.bSuccess && DryRun.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> DryRunSelector = DryRun.Data->GetObjectField(TEXT("selector"));
		TestTrue(TEXT("dry-run add selector includes planned canonical index"), DryRunSelector->HasTypedField<EJson::Number>(TEXT("index")));
		if (DryRunSelector->HasTypedField<EJson::Number>(TEXT("index")))
		{
			TestEqual(TEXT("dry-run add selector uses planned canonical index"), static_cast<int32>(DryRunSelector->GetNumberField(TEXT("index"))), 0);
		}
	}

	TSharedPtr<FJsonObject> StaleFingerprint = MontageFingerprintFor(Montage);
	StaleFingerprint->SetBoolField(TEXT("is_dirty"), true);
	FCortexCommandResult Stale = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_Stale"), 0.1, TEXT(""), StaleFingerprint));
	TestFalse(TEXT("stale add is rejected"), Stale.bSuccess);
	TestEqual(TEXT("stale add error code"), Stale.ErrorCode, FString(CortexErrorCodes::StalePrecondition));

	FCortexMontageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);
	FCortexCommandResult InvalidTime = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_InvalidTime"), -1.0, TEXT(""), MontageFingerprintFor(Montage)));
	FCortexCommandResult OutOfRange = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_OutOfRange"), 2.0, TEXT(""), MontageFingerprintFor(Montage)));
	FCortexCommandResult InvalidNext = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_InvalidNext"), 0.1, TEXT("Missing"), MontageFingerprintFor(Montage)));
	GLog->RemoveOutputDevice(&Capture);
	TestFalse(TEXT("invalid negative time fails"), InvalidTime.bSuccess);
	TestFalse(TEXT("out-of-range time fails"), OutOfRange.bSuccess);
	TestFalse(TEXT("invalid next section fails"), InvalidNext.bSuccess);
	TestEqual(TEXT("bad inputs are warning-clean"), Capture.WarningCount, 0);

	FCortexCommandResult AddA = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_A"), 0.1, TEXT(""), MontageFingerprintFor(Montage)));
	TestTrue(TEXT("baseline section add succeeds"), AddA.bSuccess);
	FCortexCommandResult Duplicate = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_A"), 0.2, TEXT(""), MontageCurrentFingerprint(AddA)));
	TestFalse(TEXT("duplicate section rejected before engine warning"), Duplicate.bSuccess);
	TestEqual(TEXT("duplicate error code"), Duplicate.ErrorCode, FString(CortexErrorCodes::AssetAlreadyExists));

	TSharedPtr<FJsonObject> OversizedSelectorParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(TNumericLimits<int32>::Max() + 1.0, TEXT("Cortex_A"), 0.1),
		MontageCurrentFingerprint(AddA));
	OversizedSelectorParams->SetStringField(TEXT("new_name"), TEXT("Cortex_TooLarge"));
	FCortexCommandResult OversizedSelector = Router.Execute(TEXT("anim.update_montage_section"), OversizedSelectorParams);
	TestFalse(TEXT("oversized selector index is rejected"), OversizedSelector.bSuccess);
	TestEqual(TEXT("oversized selector error code"), OversizedSelector.ErrorCode, FString(CortexErrorCodes::InvalidField));

	FCortexCommandResult AddB = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_B"), 0.2, TEXT("Cortex_A"), MontageCurrentFingerprint(AddA)));
	TestTrue(TEXT("referencing section add succeeds"), AddB.bSuccess);

	FCortexCommandResult ReferencedRemove = Router.Execute(
		TEXT("anim.remove_montage_section"),
		RemoveSectionParams(AssetPath, SectionSelector(0, TEXT("Cortex_A"), 0.1), MontageCurrentFingerprint(AddB)));
	TestFalse(TEXT("referenced section removal rejects"), ReferencedRemove.bSuccess);
	TestEqual(TEXT("referenced section error"), ReferencedRemove.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageAuthoringUpdateRemoveTest,
	"Cortex.Animation.MontageAuthoring.UpdateRemoveAndRenameLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageAuthoringUpdateRemoveTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexMontageTestAssetGuard Guard;
	UAnimMontage* Montage = CreateMontageTestAsset(*this, Guard, AssetPath);
	if (Montage == nullptr)
	{
		return false;
	}

	AddSectionDirect(Montage, TEXT("Cortex_A"), 0.1);
	AddSectionDirect(Montage, TEXT("Cortex_B"), 0.2, TEXT("Cortex_A"));
	FCortexCommandRouter Router = CreateMontageRouter();

	TSharedPtr<FJsonObject> RenameParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(0, TEXT("Cortex_A"), 0.1),
		MontageFingerprintFor(Montage));
	RenameParams->SetStringField(TEXT("new_name"), TEXT("Cortex_Renamed"));
	FCortexCommandResult Rename = Router.Execute(TEXT("anim.update_montage_section"), RenameParams);
	TestTrue(TEXT("rename succeeds"), Rename.bSuccess);
	if (!Rename.bSuccess)
	{
		TestEqual(TEXT("red update fails because command is not registered yet"), Rename.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
		return true;
	}
	TestEqual(TEXT("inbound next link is rewritten"), Montage->CompositeSections[SectionIndexByName(Montage, TEXT("Cortex_B"))].NextSectionName, FName(TEXT("Cortex_Renamed")));

	TSharedPtr<FJsonObject> NoNextChangeParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(1, TEXT("Cortex_B"), 0.2),
		MontageCurrentFingerprint(Rename));
	NoNextChangeParams->SetNumberField(TEXT("new_start_time"), 0.3);
	FCortexCommandResult NoNextChange = Router.Execute(TEXT("anim.update_montage_section"), NoNextChangeParams);
	TestTrue(TEXT("omitted new_next_section keeps existing link"), NoNextChange.bSuccess);
	TestEqual(TEXT("existing next link is kept"), Montage->CompositeSections[SectionIndexByName(Montage, TEXT("Cortex_B"))].NextSectionName, FName(TEXT("Cortex_Renamed")));

	Montage->GetPackage()->SetDirtyFlag(false);
	TSharedPtr<FJsonObject> IdenticalUpdateParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(1, TEXT("Cortex_B"), 0.3),
		MontageFingerprintFor(Montage));
	IdenticalUpdateParams->SetNumberField(TEXT("new_start_time"), 0.3);
	FCortexCommandResult IdenticalUpdate = Router.Execute(TEXT("anim.update_montage_section"), IdenticalUpdateParams);
	TestTrue(TEXT("identical montage update succeeds"), IdenticalUpdate.bSuccess);
	if (IdenticalUpdate.bSuccess && IdenticalUpdate.Data.IsValid())
	{
		TestFalse(TEXT("identical montage update reports unchanged"), IdenticalUpdate.Data->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("identical montage update does not dirty package"), Montage->GetPackage()->IsDirty());
	}

	TSharedPtr<FJsonObject> ClearNextParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(1, TEXT("Cortex_B"), 0.3),
		MontageCurrentFingerprint(IdenticalUpdate));
	ClearNextParams->SetStringField(TEXT("new_next_section"), TEXT(""));
	FCortexCommandResult ClearNext = Router.Execute(TEXT("anim.update_montage_section"), ClearNextParams);
	TestTrue(TEXT("present empty new_next_section clears link"), ClearNext.bSuccess);
	TestEqual(TEXT("next link cleared"), Montage->CompositeSections[SectionIndexByName(Montage, TEXT("Cortex_B"))].NextSectionName, NAME_None);

	FCortexCommandResult Remove = Router.Execute(
		TEXT("anim.remove_montage_section"),
		RemoveSectionParams(AssetPath, SectionSelector(1, TEXT("Cortex_B"), 0.3), MontageCurrentFingerprint(ClearNext)));
	TestTrue(TEXT("unreferenced section removes"), Remove.bSuccess);
	TestEqual(TEXT("one section remains"), Montage->CompositeSections.Num(), 1);
	TestEqual(TEXT("remaining section is renamed section"), Montage->CompositeSections[0].SectionName, FName(TEXT("Cortex_Renamed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageAuthoringSaveReloadTest,
	"Cortex.Animation.MontageAuthoring.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageAuthoringSaveReloadTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FCortexMontageTestAssetGuard Guard;
	UAnimMontage* Montage = CreateMontageTestAsset(*this, Guard, AssetPath);
	if (Montage == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateMontageRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_montage_section"),
		AddSectionParams(AssetPath, TEXT("Cortex_SaveReload"), 0.4, TEXT(""), MontageFingerprintFor(Montage), false, true));
	TestTrue(TEXT("saved add succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		TestEqual(TEXT("red saved add fails because command is not registered yet"), Add.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
		return true;
	}
	TestFalse(TEXT("saved add leaves package clean"), Montage->GetPackage()->IsDirty());
	if (Add.bSuccess && Add.Data.IsValid())
	{
		bool bSaved = false;
		TestTrue(TEXT("save=true reports saved"), Add.Data->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
		TestTrue(TEXT("saved packages returned"), Add.Data->HasTypedField<EJson::Array>(TEXT("saved_packages")));
	}

	Montage->ClearFlags(RF_Public | RF_Standalone);
	Montage->MarkAsGarbage();
	Guard.Montage = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UAnimMontage* Reloaded = LoadObject<UAnimMontage>(nullptr, *AssetPath);
	Guard.Montage = Reloaded;
	TestNotNull(TEXT("saved montage reloads as a new object"), Reloaded);
	if (Reloaded == nullptr)
	{
		return false;
	}

	FCortexCommandResult Inspect = Router.Execute(TEXT("anim.get_montage_info"), MontageInfoParams(AssetPath));
	TestTrue(TEXT("saved section can be read back"), Inspect.bSuccess);
	if (Inspect.bSuccess && Inspect.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = MontageJsonArray(Inspect.Data->GetObjectField(TEXT("sections")), TEXT("items"));
		TestEqual(TEXT("saved section inspection returns one section"), Items != nullptr ? Items->Num() : INDEX_NONE, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMontageAuthoringUndoRedoTest,
	"Cortex.Animation.MontageAuthoring.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMontageAuthoringUndoRedoTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || !GEditor->CanTransact())
	{
		AddInfo(TEXT("Editor undo system not available - skipping"));
		return true;
	}

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Montage Undo Setup")));

	FString AssetPath;
	FCortexMontageTestAssetGuard Guard;
	UAnimMontage* Montage = CreateMontageTestAsset(*this, Guard, AssetPath);
	if (Montage == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateMontageRouter();
	FCortexCommandResult Add = Router.Execute(TEXT("anim.add_montage_section"), AddSectionParams(AssetPath, TEXT("Cortex_Undo"), 0.1, TEXT(""), MontageFingerprintFor(Montage)));
	TestTrue(TEXT("add succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		TestEqual(TEXT("red undo add fails because command is not registered yet"), Add.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
		return true;
	}
	TestEqual(TEXT("add created section"), Montage->CompositeSections.Num(), 1);
	TestTrue(TEXT("undo add succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo add removes section"), Montage->CompositeSections.Num(), 0);
	TestTrue(TEXT("redo add succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo add restores section"), Montage->CompositeSections.Num(), 1);

	TSharedPtr<FJsonObject> UpdateParams = UpdateSectionParams(
		AssetPath,
		SectionSelector(0, TEXT("Cortex_Undo"), 0.1),
		MontageFingerprintFor(Montage));
	UpdateParams->SetStringField(TEXT("new_name"), TEXT("Cortex_UndoUpdated"));
	FCortexCommandResult Update = Router.Execute(TEXT("anim.update_montage_section"), UpdateParams);
	TestTrue(TEXT("update succeeds"), Update.bSuccess);
	TestEqual(TEXT("update renamed section"), Montage->CompositeSections[0].SectionName, FName(TEXT("Cortex_UndoUpdated")));
	TestTrue(TEXT("undo update succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo update restores name"), Montage->CompositeSections[0].SectionName, FName(TEXT("Cortex_Undo")));
	TestTrue(TEXT("redo update succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo update reapplies name"), Montage->CompositeSections[0].SectionName, FName(TEXT("Cortex_UndoUpdated")));

	FCortexCommandResult Remove = Router.Execute(
		TEXT("anim.remove_montage_section"),
		RemoveSectionParams(AssetPath, SectionSelector(0, TEXT("Cortex_UndoUpdated"), 0.1), MontageFingerprintFor(Montage)));
	TestTrue(TEXT("remove succeeds"), Remove.bSuccess);
	TestEqual(TEXT("remove deletes section"), Montage->CompositeSections.Num(), 0);
	TestTrue(TEXT("undo remove succeeds"), GEditor->UndoTransaction());
	TestEqual(TEXT("undo remove restores section"), Montage->CompositeSections.Num(), 1);
	TestTrue(TEXT("redo remove succeeds"), GEditor->RedoTransaction());
	TestEqual(TEXT("redo remove deletes section"), Montage->CompositeSections.Num(), 0);

	GEditor->ResetTransaction(FText::FromString(TEXT("Cortex Animation Montage Undo Cleanup")));
	return true;
}
