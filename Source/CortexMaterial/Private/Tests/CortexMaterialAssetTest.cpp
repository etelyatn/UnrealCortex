#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateTest,
	"Cortex.Material.Asset.CreateMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestCreate_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), MatDir);
	Params->SetStringField(TEXT("name"), MatName);

	FCortexCommandResult Result = Handler.Execute(TEXT("create_material"), Params);

	TestTrue(TEXT("create_material should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("asset_path should match"), AssetPath, MatPath);

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created should be true"), bCreated);
	}

	// Verify loadable
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	TestNotNull(TEXT("Material should be loadable"), LoadedAsset);

	// Cleanup
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateMissingParamsTest,
	"Cortex.Material.Asset.CreateMaterial.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateMissingParamsTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("create_material"), Params);

	TestFalse(TEXT("Should fail with missing params"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateDuplicateTest,
	"Cortex.Material.Asset.CreateMaterial.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateDuplicateTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestDup_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Dup_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), MatDir);
	Params->SetStringField(TEXT("name"), MatName);

	FCortexCommandResult Result1 = Handler.Execute(TEXT("create_material"), Params);
	TestTrue(TEXT("First create should succeed"), Result1.bSuccess);

	FCortexCommandResult Result2 = Handler.Execute(TEXT("create_material"), Params);
	TestFalse(TEXT("Duplicate create should fail"), Result2.bSuccess);
	TestEqual(TEXT("Error should be ASSET_ALREADY_EXISTS"),
		Result2.ErrorCode, CortexErrorCodes::AssetAlreadyExists);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}
