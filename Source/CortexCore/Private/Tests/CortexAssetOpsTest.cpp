#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveSingleTest,
	"Cortex.Core.Asset.SaveAsset.SinglePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveSingleTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestTrue(TEXT("save_asset single path should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		TestTrue(TEXT("Response has results array"), Result.Data->TryGetArrayField(TEXT("results"), Results));

		double Count = 0.0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestEqual(TEXT("Count should be 1"), static_cast<int32>(Count), 1);

		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstResult = nullptr;
			(*Results)[0]->TryGetObject(FirstResult);
			if (FirstResult != nullptr)
			{
				bool bSaved = false;
				(*FirstResult)->TryGetBoolField(TEXT("saved"), bSaved);
				TestTrue(TEXT("Asset should be saved"), bSaved);

				FString AssetType;
				(*FirstResult)->TryGetStringField(TEXT("asset_type"), AssetType);
				TestFalse(TEXT("asset_type should not be empty"), AssetType.IsEmpty());
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveNotFoundTest,
	"Cortex.Core.Asset.SaveAsset.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_DoesNotExist_ABCXYZ"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestFalse(TEXT("save_asset nonexistent path should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveGlobTest,
	"Cortex.Core.Asset.SaveAsset.Glob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveGlobTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/*"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestTrue(TEXT("save_asset glob should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		double Count = 0.0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("Glob should match at least one asset"), Count >= 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveDryRunTest,
	"Cortex.Core.Asset.SaveAsset.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveDryRunTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	RequestParams->SetBoolField(TEXT("dry_run"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestTrue(TEXT("save_asset dry_run should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bDryRun = false;
		Result.Data->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("dry_run should be true in response"), bDryRun);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveArrayTest,
	"Cortex.Core.Asset.SaveAsset.Array",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveArrayTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Data/DT_TestSimple")));
	Paths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Data/DT_DoesNotExist_ABCXYZ")));
	RequestParams->SetArrayField(TEXT("asset_path"), Paths);

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestTrue(TEXT("save_asset array should succeed at top level"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		double Count = 0.0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestEqual(TEXT("Count should be 2"), static_cast<int32>(Count), 2);

		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("results"), Results) && Results != nullptr && Results->Num() == 2)
		{
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Results)[0]->TryGetObject(First);
			if (First != nullptr)
			{
				bool bSaved = false;
				(*First)->TryGetBoolField(TEXT("saved"), bSaved);
				TestTrue(TEXT("First asset should be saved"), bSaved);
			}

			const TSharedPtr<FJsonObject>* Second = nullptr;
			(*Results)[1]->TryGetObject(Second);
			if (Second != nullptr)
			{
				FString Error;
				(*Second)->TryGetStringField(TEXT("error"), Error);
				TestEqual(TEXT("Second should be ASSET_NOT_FOUND"), Error, TEXT("ASSET_NOT_FOUND"));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveMissingParamTest,
	"Cortex.Core.Asset.SaveAsset.MissingParam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveMissingParamTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), MakeShared<FJsonObject>());
	TestFalse(TEXT("save_asset without asset_path should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetOpenSingleTest,
	"Cortex.Core.Asset.OpenAsset.SinglePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetOpenSingleTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.open_asset"), RequestParams);
	TestTrue(TEXT("open_asset should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bOpened = false;
				(*Entry)->TryGetBoolField(TEXT("editor_opened"), bOpened);
				TestTrue(TEXT("editor_opened should be true"), bOpened);

				bool bWasAlreadyOpen = false;
				TestTrue(TEXT("was_already_open should be present"), (*Entry)->TryGetBoolField(TEXT("was_already_open"), bWasAlreadyOpen));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetOpenNotFoundTest,
	"Cortex.Core.Asset.OpenAsset.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetOpenNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_DoesNotExist_ABCXYZ"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.open_asset"), RequestParams);
	TestFalse(TEXT("open_asset nonexistent path should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetOpenDryRunTest,
	"Cortex.Core.Asset.OpenAsset.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetOpenDryRunTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	RequestParams->SetBoolField(TEXT("dry_run"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("core.open_asset"), RequestParams);
	TestTrue(TEXT("open_asset dry_run should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bDryRun = false;
		Result.Data->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("dry_run should be true in response"), bDryRun);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetOpenAlreadyOpenTest,
	"Cortex.Core.Asset.OpenAsset.AlreadyOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetOpenAlreadyOpenTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));

	// Open the first time
	Router.Execute(TEXT("core.open_asset"), RequestParams);

	// Open again — should report was_already_open=true
	FCortexCommandResult Result = Router.Execute(TEXT("core.open_asset"), RequestParams);
	TestTrue(TEXT("open_asset second call should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bWasAlreadyOpen = false;
				(*Entry)->TryGetBoolField(TEXT("was_already_open"), bWasAlreadyOpen);
				TestTrue(TEXT("was_already_open should be true on second open"), bWasAlreadyOpen);
			}
		}
	}

	// Clean up: close the asset
	Router.Execute(TEXT("core.close_asset"), RequestParams);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetCloseNotOpenTest,
	"Cortex.Core.Asset.CloseAsset.NotOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetCloseNotOpenTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	// Ensure the asset is not open by closing it first
	TSharedPtr<FJsonObject> CloseParams = MakeShared<FJsonObject>();
	CloseParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	Router.Execute(TEXT("core.close_asset"), CloseParams);

	// Now close again — should succeed but report closed=false
	FCortexCommandResult Result = Router.Execute(TEXT("core.close_asset"), CloseParams);
	TestTrue(TEXT("close_asset on non-open asset should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bClosed = true;
				(*Entry)->TryGetBoolField(TEXT("closed"), bClosed);
				TestFalse(TEXT("closed should be false when asset was not open"), bClosed);

				bool bWasOpen = true;
				(*Entry)->TryGetBoolField(TEXT("was_open"), bWasOpen);
				TestFalse(TEXT("was_open should be false"), bWasOpen);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetCloseSingleTest,
	"Cortex.Core.Asset.CloseAsset.SinglePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetCloseSingleTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	Router.Execute(TEXT("core.open_asset"), OpenParams);

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.close_asset"), RequestParams);
	TestTrue(TEXT("close_asset should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bClosed = false;
				TestTrue(TEXT("closed should be present"), (*Entry)->TryGetBoolField(TEXT("closed"), bClosed));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetCloseSaveBeforeCloseTest,
	"Cortex.Core.Asset.CloseAsset.SaveBeforeClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetCloseSaveBeforeCloseTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	Router.Execute(TEXT("core.open_asset"), OpenParams);

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	RequestParams->SetBoolField(TEXT("save"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("core.close_asset"), RequestParams);
	TestTrue(TEXT("close_asset save=true should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bSaved = false;
				TestTrue(TEXT("saved should be present"), (*Entry)->TryGetBoolField(TEXT("saved"), bSaved));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetReloadSingleTest,
	"Cortex.Core.Asset.ReloadAsset.SinglePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetReloadSingleTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.reload_asset"), RequestParams);
	TestTrue(TEXT("reload_asset should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		Result.Data->TryGetArrayField(TEXT("results"), Results);
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			(*Results)[0]->TryGetObject(Entry);
			if (Entry != nullptr)
			{
				bool bReloaded = false;
				(*Entry)->TryGetBoolField(TEXT("reloaded"), bReloaded);
				TestTrue(TEXT("reloaded should be true"), bReloaded);

				FString AssetType;
				(*Entry)->TryGetStringField(TEXT("asset_type"), AssetType);
				TestFalse(TEXT("asset_type should not be empty"), AssetType.IsEmpty());
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetReloadNotFoundTest,
	"Cortex.Core.Asset.ReloadAsset.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetReloadNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_DoesNotExist_ABCXYZ"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.reload_asset"), RequestParams);
	TestFalse(TEXT("reload_asset nonexistent should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetReloadDryRunTest,
	"Cortex.Core.Asset.ReloadAsset.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetReloadDryRunTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Data/DT_TestSimple"));
	RequestParams->SetBoolField(TEXT("dry_run"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("core.reload_asset"), RequestParams);
	TestTrue(TEXT("reload_asset dry_run should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bDryRun = false;
		Result.Data->TryGetBoolField(TEXT("dry_run"), bDryRun);
		TestTrue(TEXT("dry_run should be true in response"), bDryRun);
	}

	return true;
}
