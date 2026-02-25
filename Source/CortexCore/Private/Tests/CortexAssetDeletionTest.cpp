#include "Misc/AutomationTest.h"
#include "CortexCoreCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetNotFoundTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCoreCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/FakeAsset"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("delete_asset"), Params);

	TestFalse(TEXT("Should fail for non-existent asset"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::AssetNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetMissingPathTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexCoreCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("delete_asset"), Params);

	TestFalse(TEXT("Should fail without path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderEmptyTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderEmptyTest::RunTest(const FString& Parameters)
{
	FCortexCoreCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("folder_path"), TEXT("/Game/Temp/NonExistentFolder"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("delete_folder"), Params);

	TestTrue(TEXT("Empty folder delete should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		double DeletedCount = -1;
		Result.Data->TryGetNumberField(TEXT("deleted_count"), DeletedCount);
		TestEqual(TEXT("deleted_count should be 0"), static_cast<int32>(DeletedCount), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderMissingPathTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexCoreCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("delete_folder"), Params);

	TestFalse(TEXT("Should fail without folder_path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}
