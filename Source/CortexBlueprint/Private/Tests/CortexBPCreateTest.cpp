#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Guid.h"
#include "Engine/Blueprint.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateWidgetTest,
	"Cortex.Blueprint.Create.Widget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateWidgetTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("WBP_TestCreate_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Create_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Widget"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("asset_path should match"),
			AssetPath, BlueprintPath);

		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("type should be Widget"), Type, TEXT("Widget"));

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created should be true"), bCreated);
	}

	// Verify asset is loadable
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedAsset);

	// Cleanup
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateMissingParamsTest,
	"Cortex.Blueprint.Create.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateMissingParamsTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// Missing name, path, and type

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should return INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateInvalidTypeTest,
	"Cortex.Blueprint.Create.InvalidType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateInvalidTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("BP_TestInvalidType"));
	Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_InvalidType"));
	Params->SetStringField(TEXT("type"), TEXT("InvalidType"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should return INVALID_BLUEPRINT_TYPE"), Result.ErrorCode, CortexErrorCodes::InvalidBlueprintType);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateDuplicateTest,
	"Cortex.Blueprint.Create.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateDuplicateTest::RunTest(const FString& Parameters)
{
	// Setup - Create first asset
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestDuplicate_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Duplicate_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Actor"));

	// Execute first creation
	FCortexCommandResult Result1 = Handler.Execute(TEXT("create"), Params);
	TestTrue(TEXT("First creation should succeed"), Result1.bSuccess);

	// Try to create again
	FCortexCommandResult Result2 = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Second creation should fail"), Result2.bSuccess);
	TestEqual(TEXT("Should return BLUEPRINT_ALREADY_EXISTS"), Result2.ErrorCode, CortexErrorCodes::BlueprintAlreadyExists);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}
