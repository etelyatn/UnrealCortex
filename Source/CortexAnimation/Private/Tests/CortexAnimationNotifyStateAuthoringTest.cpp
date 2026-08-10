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
constexpr const TCHAR* NotifyStateSourceSkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

FCortexCommandRouter CreateNotifyStateRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

UAnimSequence* CreateNotifyStateSequence(FAutomationTestBase& Test, FString& OutPath)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, NotifyStateSourceSkeletonPath);
	Test.TestNotNull(TEXT("source skeleton exists"), Skeleton);
	if (Skeleton == nullptr)
	{
		return nullptr;
	}
	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexAnimState_%s/AS_Test"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
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

TSharedPtr<FJsonObject> AddNotifyStateParams(const FString& AssetPath, UAnimSequence* Sequence)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("notify_state_class_path"), TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"));
	Params->SetNumberField(TEXT("start_time"), 0.1);
	Params->SetNumberField(TEXT("duration"), 0.25);
	Params->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNotifyStateAddTest,
	"Cortex.Animation.NotifyStateAuthoring.AddCanonicalReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNotifyStateAddTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	UAnimSequence* Sequence = CreateNotifyStateSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}
	FCortexCommandResult Result = CreateNotifyStateRouter().Execute(TEXT("anim.add_notify_state"), AddNotifyStateParams(AssetPath, Sequence));
	TestTrue(TEXT("notify state add succeeds"), Result.bSuccess);
	TestEqual(TEXT("exactly one state event was created"), Sequence->Notifies.Num(), 1);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> After = Result.Data->GetObjectField(TEXT("after"));
		const TSharedPtr<FJsonObject> Selector = Result.Data->GetObjectField(TEXT("selector"));
		TestEqual(TEXT("state selector uses the state class path"), Selector->GetStringField(TEXT("class_path")), FString(TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect")));
		TestEqual(TEXT("canonical duration"), After->GetNumberField(TEXT("duration")), 0.25);
		TestTrue(TEXT("canonical end link path is present"), After->HasField(TEXT("end_link_asset_path")));
		TestTrue(TEXT("canonical end link time is present"), After->HasField(TEXT("end_link_time")));
	}
	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNotifyStateUpdateRemoveTest,
	"Cortex.Animation.NotifyStateAuthoring.UpdateRemoveCanonicalReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNotifyStateUpdateRemoveTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	UAnimSequence* Sequence = CreateNotifyStateSequence(*this, AssetPath);
	if (Sequence == nullptr)
	{
		return false;
	}
	FCortexCommandRouter Router = CreateNotifyStateRouter();
	FCortexCommandResult Add = Router.Execute(TEXT("anim.add_notify_state"), AddNotifyStateParams(AssetPath, Sequence));
	TestTrue(TEXT("state add succeeds before update"), Add.bSuccess);
	if (!Add.bSuccess || !Add.Data.IsValid())
	{
		return false;
	}
	TSharedPtr<FJsonObject> UpdateParams = MakeShared<FJsonObject>();
	UpdateParams->SetStringField(TEXT("asset_path"), AssetPath);
	UpdateParams->SetObjectField(TEXT("selector"), Add.Data->GetObjectField(TEXT("selector")));
	UpdateParams->SetNumberField(TEXT("new_start_time"), 0.2);
	UpdateParams->SetNumberField(TEXT("new_duration"), 0.3);
	UpdateParams->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
	FCortexCommandResult Update = Router.Execute(TEXT("anim.update_notify_state"), UpdateParams);
	TestTrue(TEXT("state update succeeds"), Update.bSuccess);
	if (Update.bSuccess && Update.Data.IsValid())
	{
		TestEqual(TEXT("updated state duration"), Update.Data->GetObjectField(TEXT("after"))->GetNumberField(TEXT("duration")), 0.3);
		TSharedPtr<FJsonObject> NoOpParams = MakeShared<FJsonObject>();
		NoOpParams->SetStringField(TEXT("asset_path"), AssetPath);
		NoOpParams->SetObjectField(TEXT("selector"), Update.Data->GetObjectField(TEXT("selector")));
		NoOpParams->SetNumberField(TEXT("new_duration"), 0.3);
		NoOpParams->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
		FCortexCommandResult NoOp = Router.Execute(TEXT("anim.update_notify_state"), NoOpParams);
		TestTrue(TEXT("explicit exact state no-op succeeds"), NoOp.bSuccess);
		TestFalse(TEXT("explicit exact state no-op reports unchanged"), NoOp.Data.IsValid() && NoOp.Data->GetBoolField(TEXT("changed")));
		TSharedPtr<FJsonObject> PreciseUpdateParams = MakeShared<FJsonObject>();
		PreciseUpdateParams->SetStringField(TEXT("asset_path"), AssetPath);
		PreciseUpdateParams->SetObjectField(TEXT("selector"), Update.Data->GetObjectField(TEXT("selector")));
		PreciseUpdateParams->SetNumberField(TEXT("new_duration"), 0.30005);
		PreciseUpdateParams->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
		FCortexCommandResult PreciseUpdate = Router.Execute(TEXT("anim.update_notify_state"), PreciseUpdateParams);
		TestTrue(TEXT("sub-millisecond state duration update succeeds"), PreciseUpdate.bSuccess);
		TestTrue(TEXT("sub-millisecond state duration update reports changed"), PreciseUpdate.Data.IsValid() && PreciseUpdate.Data->GetBoolField(TEXT("changed")));
		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetObjectField(TEXT("selector"), PreciseUpdate.Data->GetObjectField(TEXT("selector")));
		RemoveParams->SetObjectField(TEXT("expected_fingerprint"), MakeObjectAssetFingerprint(Sequence).ToJson());
		FCortexCommandResult Remove = Router.Execute(TEXT("anim.remove_notify_state"), RemoveParams);
		TestTrue(TEXT("state remove succeeds"), Remove.bSuccess);
		TestTrue(TEXT("state remove preserves its canonical selector"), Remove.Data.IsValid() && Remove.Data->HasField(TEXT("selector")) && Remove.Data->GetObjectField(TEXT("selector"))->HasField(TEXT("class_path")));
		TestEqual(TEXT("state remove leaves no events"), Sequence->Notifies.Num(), 0);
	}
	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}
