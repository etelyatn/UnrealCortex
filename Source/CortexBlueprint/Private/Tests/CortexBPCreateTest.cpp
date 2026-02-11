// Copyright Andrei Sudarikov. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
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
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("WBP_TestCreate"));
	Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Create"));
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
			AssetPath, TEXT("/Game/Temp/CortexBPTest_Create/WBP_TestCreate"));

		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("type should be Widget"), Type, TEXT("Widget"));

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created should be true"), bCreated);
	}

	// Verify asset is loadable
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_Create/WBP_TestCreate"));
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
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("BP_TestDuplicate"));
	Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Duplicate"));
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
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_Duplicate/BP_TestDuplicate"));
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}
