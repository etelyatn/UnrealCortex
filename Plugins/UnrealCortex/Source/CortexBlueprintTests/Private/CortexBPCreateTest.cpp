// Copyright Andrei Sudarikov. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "CortexBPCommandHandler.h"
#include "CortexErrorCodes.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexBPTests, Log, All);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateWidgetTest,
	"Cortex.Blueprint.Create.Widget",
	EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter
)

bool FCortexBPCreateWidgetTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestWidget"));
	Params->SetStringField(TEXT("type"), TEXT("Widget"));
	Params->SetBoolField(TEXT("force"), false);

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);
	TestEqual(TEXT("Should have success status"), Result.Status, ECortexCommandStatus::Success);

	// Verify asset is loadable
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/TestWidget.TestWidget"));
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
	EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter
)

bool FCortexBPCreateMissingParamsTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// Missing asset_path and type

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should have error status"), Result.Status, ECortexCommandStatus::Error);
	TestEqual(TEXT("Should return INVALID_FIELD"), Result.ErrorCode, ECortexErrorCode::INVALID_FIELD);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateInvalidTypeTest,
	"Cortex.Blueprint.Create.InvalidType",
	EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter
)

bool FCortexBPCreateInvalidTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestInvalidType"));
	Params->SetStringField(TEXT("type"), TEXT("InvalidType"));
	Params->SetBoolField(TEXT("force"), false);

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should have error status"), Result.Status, ECortexCommandStatus::Error);
	TestEqual(TEXT("Should return INVALID_BLUEPRINT_TYPE"), Result.ErrorCode, ECortexErrorCode::INVALID_BLUEPRINT_TYPE);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateDuplicateTest,
	"Cortex.Blueprint.Create.Duplicate",
	EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter
)

bool FCortexBPCreateDuplicateTest::RunTest(const FString& Parameters)
{
	// Setup - Create first asset
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestDuplicate"));
	Params->SetStringField(TEXT("type"), TEXT("Actor"));
	Params->SetBoolField(TEXT("force"), false);

	// Execute first creation
	FCortexCommandResult Result1 = Handler.Execute(TEXT("create"), Params);
	TestTrue(TEXT("First creation should succeed"), Result1.bSuccess);

	// Try to create again
	FCortexCommandResult Result2 = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestFalse(TEXT("Second creation should fail"), Result2.bSuccess);
	TestEqual(TEXT("Should have error status"), Result2.Status, ECortexCommandStatus::Error);
	TestEqual(TEXT("Should return BLUEPRINT_ALREADY_EXISTS"), Result2.ErrorCode, ECortexErrorCode::BLUEPRINT_ALREADY_EXISTS);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/TestDuplicate.TestDuplicate"));
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}
