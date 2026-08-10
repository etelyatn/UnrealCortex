#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace
{
constexpr const TCHAR* ObjectNotifySourceSkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

FCortexCommandRouter CreateRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

UAnimSequence* CreateSequence(FAutomationTestBase& Test, FString& OutPath)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, ObjectNotifySourceSkeletonPath);
	Test.TestNotNull(TEXT("source skeleton exists"), Skeleton);
	if (Skeleton == nullptr)
	{
		return nullptr;
	}

	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimObject_%s/AS_Test"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	UPackage* Package = CreatePackage(*PackageName);
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, UAnimSequence::StaticClass(), TEXT("AS_Test"), RF_Public | RF_Standalone | RF_Transactional);
	Test.TestNotNull(TEXT("scratch sequence exists"), Sequence);
	if (Sequence == nullptr)
	{
		return nullptr;
	}
	Sequence->SetSkeleton(Skeleton);
	Sequence->AssetImportData = NewObject<UAssetImportData>(Sequence, TEXT("AssetImportData"));
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	Sequence->RefreshCacheData();
	FAssetRegistryModule::AssetCreated(Sequence);
	Package->SetDirtyFlag(false);
	OutPath = Sequence->GetPathName();
	return Sequence;
}

TSharedPtr<FJsonObject> AddParams(const FString& AssetPath, const FString& ClassPath, double Time, UAnimSequence* Sequence)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("notify_class_path"), ClassPath);
	Params->SetNumberField(TEXT("time"), Time);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
	return Params;
}

TSharedPtr<FJsonObject> SelectorParams(
	const FString& AssetPath,
	const TCHAR* Command,
	const TSharedPtr<FJsonObject>& Selector,
	UAnimSequence* Sequence,
	double NewTime = -1.0)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetObjectField(TEXT("selector"), Selector);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
	if (FString(Command) == TEXT("anim.update_notify"))
	{
		Params->SetNumberField(TEXT("new_time"), NewTime);
	}
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationObjectNotifyAddTest,
	"Cortex.Animation.ObjectNotifyAuthoring.AddCanonicalReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationObjectNotifyAddTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	UAnimSequence* Sequence = CreateSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandResult Result = CreateRouter().Execute(
		TEXT("anim.add_notify"),
		AddParams(AssetPath, TEXT("/Script/Engine.AnimNotify_PlaySound"), 0.1, Sequence));
	TestTrue(TEXT("object notify add succeeds"), Result.bSuccess);
	TestEqual(TEXT("exactly one event was created"), Sequence->Notifies.Num(), 1);
	TestTrue(TEXT("canonical after state exists"), Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("after")));
	if (Result.Data.IsValid() && Result.Data->HasTypedField<EJson::Object>(TEXT("after")))
	{
		TestEqual(TEXT("canonical class path"), Result.Data->GetObjectField(TEXT("after"))->GetStringField(TEXT("class_path")), FString(TEXT("/Script/Engine.AnimNotify_PlaySound")));
	}

	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationObjectNotifyUpdateRemoveTest,
	"Cortex.Animation.ObjectNotifyAuthoring.UpdateRemoveCanonicalReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationObjectNotifyUpdateRemoveTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	UAnimSequence* Sequence = CreateSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}

	FCortexCommandRouter Router = CreateRouter();
	FCortexCommandResult Add = Router.Execute(
		TEXT("anim.add_notify"),
		AddParams(AssetPath, TEXT("/Script/Engine.AnimNotify_PlaySound"), 0.1, Sequence));
	TestTrue(TEXT("add succeeds before update"), Add.bSuccess);
	if (!Add.bSuccess || !Add.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> AddSelector = Add.Data->GetObjectField(TEXT("selector"));
	FCortexCommandResult Update = Router.Execute(
		TEXT("anim.update_notify"),
		SelectorParams(AssetPath, TEXT("anim.update_notify"), AddSelector, Sequence, 0.2));
	TestTrue(TEXT("object notify update succeeds"), Update.bSuccess);
	if (Update.bSuccess && Update.Data.IsValid())
	{
		TestEqual(TEXT("updated canonical time"), Update.Data->GetObjectField(TEXT("after"))->GetNumberField(TEXT("time")), 0.2);
		FCortexCommandResult Remove = Router.Execute(
			TEXT("anim.remove_notify"),
			SelectorParams(AssetPath, TEXT("anim.remove_notify"), Update.Data->GetObjectField(TEXT("selector")), Sequence));
		TestTrue(TEXT("object notify remove succeeds"), Remove.bSuccess);
		TestEqual(TEXT("remove leaves no events"), Sequence->Notifies.Num(), 0);
	}

	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}
