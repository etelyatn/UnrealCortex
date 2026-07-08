#include "Misc/AutomationTest.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/OutputDevice.h"

namespace
{
class FCortexSkipPackageWarningCapture final : public FOutputDevice
{
public:
	int32 SkipPackageWarnings = 0;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		(void)Category;

		const ELogVerbosity::Type VerbosityLevel =
			static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (VerbosityLevel != ELogVerbosity::Warning || V == nullptr)
		{
			return;
		}

		const FString Message(V);
		if (Message.Contains(TEXT("SkipPackage")))
		{
			++SkipPackageWarnings;
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}
};

FCortexCommandRouter CreateAnimRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
	return Router;
}

TSharedPtr<FJsonObject> ParamsWithAssetPath(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationListAssetsTest,
	"Cortex.Animation.ListAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationListAssetsTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_type"), TEXT("AnimSequence"));
	Params->SetStringField(TEXT("path"), TEXT("/Game/Characters/Mannequins/Anims/Pistol"));
	Params->SetStringField(TEXT("query"), TEXT("Fire"));
	Params->SetNumberField(TEXT("limit"), 10);

	FCortexCommandResult Result = Router.Execute(TEXT("anim.list_assets"), Params);
	TestTrue(TEXT("list_assets should succeed"), Result.bSuccess);
	TestTrue(TEXT("data should be valid"), Result.Data.IsValid());

	const TSharedPtr<FJsonObject>* Assets = nullptr;
	TestTrue(TEXT("assets collection exists"), Result.Data->TryGetObjectField(TEXT("assets"), Assets));
	int32 Count = 0;
	int32 Returned = 0;
	bool bTruncated = true;
	TestTrue(TEXT("assets.count exists"), (*Assets)->TryGetNumberField(TEXT("count"), Count));
	TestTrue(TEXT("assets.returned exists"), (*Assets)->TryGetNumberField(TEXT("returned"), Returned));
	TestTrue(TEXT("assets.truncated exists"), (*Assets)->TryGetBoolField(TEXT("truncated"), bTruncated));
	TestTrue(TEXT("Pistol fire assets should exist"), Count >= 1);
	TestTrue(TEXT("Returned is capped by limit"), Returned <= 10);
	TestFalse(TEXT("Query result should not be truncated at limit 10"), bTruncated);

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("assets.items exists"), (*Assets)->TryGetArrayField(TEXT("items"), Items));
	bool bFoundFireSequence = false;
	for (const TSharedPtr<FJsonValue>& Value : *Items)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
		{
			FString AssetPath;
			(*Obj)->TryGetStringField(TEXT("asset_path"), AssetPath);
			bFoundFireSequence |= AssetPath.Contains(TEXT("MM_Pistol_Fire"));
		}
	}
	TestTrue(TEXT("MM_Pistol_Fire should be listed"), bFoundFireSequence);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMissingPackageNoSkipWarningTest,
	"Cortex.Animation.Resolve.MissingPackageNoSkipWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMissingPackageNoSkipWarningTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimRouter();
	FCortexSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);

	FCortexCommandResult Result = Router.Execute(
		TEXT("anim.get_sequence_info"),
		ParamsWithAssetPath(TEXT("/Game/Characters/Mannequins/Anims/Pistol/NoSuchSequence"))
	);

	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("missing sequence should fail"), Result.bSuccess);
	TestEqual(TEXT("error code should be asset_not_found"), Result.ErrorCode, FString(CortexErrorCodes::AssetNotFound));
	TestTrue(TEXT("error details should be present"), Result.ErrorDetails.IsValid());
	TestEqual(TEXT("missing sequence should not emit SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationWrongClassTest,
	"Cortex.Animation.Resolve.WrongClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationWrongClassTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimRouter();
	FCortexCommandResult Result = Router.Execute(
		TEXT("anim.get_sequence_info"),
		ParamsWithAssetPath(TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin"))
	);

	TestFalse(TEXT("skeleton mesh should fail sequence inspection"), Result.bSuccess);
	TestEqual(TEXT("error code should be invalid field"), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
	TestTrue(TEXT("error details should include expected type"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("expected_type")));
	TestTrue(TEXT("error details should include actual type"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("actual_type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationWrongObjectNameTest,
	"Cortex.Animation.Resolve.WrongObjectName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationWrongObjectNameTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateAnimRouter();
	FCortexCommandResult Result = Router.Execute(
		TEXT("anim.get_sequence_info"),
		ParamsWithAssetPath(TEXT("/Game/Characters/Mannequins/Anims/Pistol/MM_Pistol_Fire.NoSuchObject"))
	);

	TestFalse(TEXT("wrong object name should fail sequence inspection"), Result.bSuccess);
	TestEqual(TEXT("error code should be asset_not_found"), Result.ErrorCode, FString(CortexErrorCodes::AssetNotFound));
	TestTrue(TEXT("error details should be present"), Result.ErrorDetails.IsValid());
	return true;
}
