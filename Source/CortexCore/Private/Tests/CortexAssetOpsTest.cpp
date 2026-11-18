#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexAssetMutationGuard.h"
#include "Engine/DataTable.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/CurveTable.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Operations/CortexAssetOps.h"
#include "PackageTools.h"

namespace
{
TSharedPtr<FJsonObject> ExecuteSingleFingerprintRequest(FCortexCommandRouter& Router, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueString>(AssetPath));
	RequestParams->SetArrayField(TEXT("paths"), Paths);

	const FCortexCommandResult Result = Router.Execute(TEXT("core.asset_fingerprint"), RequestParams);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Fingerprints = nullptr;
	if (!Result.Data->TryGetArrayField(TEXT("fingerprints"), Fingerprints) || Fingerprints == nullptr || Fingerprints->Num() != 1)
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Entry = nullptr;
	if (!(*Fingerprints)[0]->TryGetObject(Entry) || Entry == nullptr)
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	if (!(*Entry)->TryGetObjectField(TEXT("fingerprint"), Fingerprint) || Fingerprint == nullptr)
	{
		return nullptr;
	}

	return *Fingerprint;
}
}

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
	FCortexAssetSaveWorldMapExtensionTest,
	"Cortex.Core.Asset.SaveAsset.WorldUsesMapExtension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveWorldMapExtensionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	const FString MapPackageName = TEXT("/Game/Maps/TestMap");

	FString MapFilename;
	FString AssetFilename;
	TestTrue(TEXT("Map package resolves to .umap filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			MapPackageName, MapFilename, FPackageName::GetMapPackageExtension()));
	TestTrue(TEXT("Map package resolves to .uasset filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			MapPackageName, AssetFilename, FPackageName::GetAssetPackageExtension()));

	// Precondition: no shadow .uasset twin exists beside the tracked .umap.
	TestFalse(TEXT("No .uasset twin exists before save"), FPaths::FileExists(AssetFilename));

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), MapPackageName);
	RequestParams->SetBoolField(TEXT("force"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestTrue(TEXT("save_asset on World package should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		TestTrue(TEXT("Response has results array"), Result.Data->TryGetArrayField(TEXT("results"), Results));
		if (Results != nullptr && Results->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstResult = nullptr;
			(*Results)[0]->TryGetObject(FirstResult);
			if (FirstResult != nullptr)
			{
				bool bSaved = false;
				(*FirstResult)->TryGetBoolField(TEXT("saved"), bSaved);
				TestTrue(TEXT("World package should be saved"), bSaved);
			}
		}
	}

	TestTrue(TEXT(".umap file exists after save"), FPaths::FileExists(MapFilename));
	TestFalse(TEXT("Save must not create a .uasset twin for a World package"),
		FPaths::FileExists(AssetFilename));

	// Cleanup: if a regression created the twin, remove it so one failing run
	// does not poison subsequent runs.
	if (FPaths::FileExists(AssetFilename))
	{
		IFileManager::Get().Delete(*AssetFilename);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveBatchExpectedFingerprintDirtyDataTableTest,
	"Cortex.Core.Asset.SaveAsset.BatchExpectedFingerprint.DirtyDataTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveBatchExpectedFingerprintDirtyDataTableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/Data/DT_TestSimple.DT_TestSimple"));
	TestNotNull(TEXT("Test data table loads"), DataTable);
	if (!DataTable)
	{
		return false;
	}

	UPackage* Package = DataTable->GetPackage();
	TestNotNull(TEXT("Test data table package exists"), Package);
	if (!Package)
	{
		return false;
	}

	const FString OriginalImportKeyField = DataTable->ImportKeyField;
	Package->SetDirtyFlag(false);

	DataTable->Modify();
	DataTable->ImportKeyField = TEXT("BatchFingerprintOriginal");
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> ExpectedFingerprint = ExecuteSingleFingerprintRequest(Router, TEXT("/Game/Data/DT_TestSimple"));
	TestTrue(TEXT("Expected fingerprint captured"), ExpectedFingerprint.IsValid());
	if (!ExpectedFingerprint.IsValid())
	{
		DataTable->ImportKeyField = OriginalImportKeyField;
		Package->SetDirtyFlag(false);
		return false;
	}

	DataTable->ImportKeyField = TEXT("BatchFingerprintChanged");
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("target"), TEXT("/Game/Data/DT_TestSimple"));
	Item->SetBoolField(TEXT("dry_run"), true);
	Item->SetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(Item));

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("core.save_asset"), RequestParams);
	TestFalse(TEXT("stale dirty data table batch save is rejected"), Result.bSuccess);
	TestEqual(TEXT("stale error code"), Result.ErrorCode, TEXT("STALE_PRECONDITION"));

	DataTable->ImportKeyField = OriginalImportKeyField;
	Package->SetDirtyFlag(false);
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

// save_asset under dry_run must not load glob/path matches that are not already in memory
// (issue #224 item 8): an unloaded package cannot be dirty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetSaveDryRunNoLoadTest,
	"Cortex.Core.Asset.SaveAsset.DryRunNoLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetSaveDryRunNoLoadTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString AssetName = FString::Printf(TEXT("M_SaveNoLoad_%s"), *Suffix);
	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexSaveNoLoad_%s"), *Suffix);
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* Asset = NewObject<UMaterial>(Package, FName(*AssetName), RF_Public | RF_Standalone);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("Fixture saved clean"), UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs)))
	{
		Asset->MarkAsGarbage();
		return false;
	}

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous({PackageFilename}, true);

	TArray<UPackage*> ToUnload = {Package};
	FText UnloadError;
	if (!TestTrue(TEXT("Fixture package unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true)))
	{
		TArray<UPackage*> Loaded = {FindPackage(nullptr, *PackageName)};
		if (Loaded[0] != nullptr)
		{
			UPackageTools::UnloadPackages(Loaded, UnloadError, true);
		}
		IFileManager::Get().Delete(*PackageFilename, false, true);
		Registry.ScanModifiedAssetFiles({PackageFilename});
		return false;
	}

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), ObjectPath);
	RequestParams->SetBoolField(TEXT("dry_run"), true);

	const FCortexCommandResult Result = FCortexAssetOps::SaveAsset(RequestParams);

	const bool bSucceeded = TestTrue(TEXT("save_asset dry_run should succeed"), Result.bSuccess)
		&& Result.Data.IsValid();

	if (bSucceeded)
	{
		bool bCanSave = true;
		Result.Data->TryGetBoolField(TEXT("can_save"), bCanSave);
		TestFalse(TEXT("Clean unloaded package cannot be saved"), bCanSave);

		bool bWasDirty = true;
		Result.Data->TryGetBoolField(TEXT("was_dirty"), bWasDirty);
		TestFalse(TEXT("Unloaded package is not dirty"), bWasDirty);
	}

	TestNull(TEXT("Package stays unloaded under dry_run"), FindPackage(nullptr, *PackageName));

	TArray<UPackage*> Loaded = {FindPackage(nullptr, *PackageName)};
	if (Loaded[0] != nullptr)
	{
		UPackageTools::UnloadPackages(Loaded, UnloadError, true);
	}
	IFileManager::Get().Delete(*PackageFilename, false, true);
	Registry.ScanModifiedAssetFiles({PackageFilename});
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetOpenWorldTest,
	"Cortex.Core.Asset.OpenAsset.WorldAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetOpenWorldTest::RunTest(const FString& Parameters)
{
	const FString BaselineMapPackage = TEXT("/Game/Maps/TestMap");
	FString BaselineMapFilePath;
	const bool bBaselineMapReady = FPackageName::TryConvertLongPackageNameToFilename(
		BaselineMapPackage,
		BaselineMapFilePath,
		FPackageName::GetMapPackageExtension());
	TestTrue(TEXT("Baseline map package should resolve to filename"), bBaselineMapReady);
	if (bBaselineMapReady)
	{
		UWorld* BaselineWorld = UEditorLoadingAndSavingUtils::LoadMap(BaselineMapFilePath);
		TestNotNull(TEXT("Baseline map should load before test"), BaselineWorld);
	}

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	RequestParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Maps/TestRoomMap"));

	FCortexCommandResult Result = Router.Execute(TEXT("core.open_asset"), RequestParams);
	TestTrue(TEXT("open_asset World should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		TestTrue(TEXT("Response has results array"), Result.Data->TryGetArrayField(TEXT("results"), Results));
		if (Results != nullptr)
		{
			TestEqual(TEXT("Results should contain exactly one entry"), Results->Num(), 1);
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			TestTrue(TEXT("First result should be an object"), (*Results)[0]->TryGetObject(Entry));
			if (Entry != nullptr)
			{
				bool bOpened = false;
				(*Entry)->TryGetBoolField(TEXT("editor_opened"), bOpened);
				TestTrue(TEXT("editor_opened should be true for World"), bOpened);

				FString AssetType;
				(*Entry)->TryGetStringField(TEXT("asset_type"), AssetType);
				TestTrue(TEXT("asset_type should be World"), AssetType.Contains(TEXT("World")));

				bool bWasAlreadyOpen = true;
				(*Entry)->TryGetBoolField(TEXT("was_already_open"), bWasAlreadyOpen);
				TestTrue(TEXT("was_already_open field should exist"),
					(*Entry)->HasField(TEXT("was_already_open")));
			}
		}
	}

	if (bBaselineMapReady)
	{
		UWorld* RestoredWorld = UEditorLoadingAndSavingUtils::LoadMap(BaselineMapFilePath);
		TestNotNull(TEXT("Baseline map should restore after world open test"), RestoredWorld);
	}

	if (GEditor != nullptr)
	{
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		TestNotNull(TEXT("Editor world should be valid"), EditorWorld);
		if (EditorWorld != nullptr)
		{
			TestEqual(
				TEXT("World-open test should restore baseline map to avoid leaking global editor state"),
				EditorWorld->GetOutermost()->GetName(),
				BaselineMapPackage);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreBlockedSaveDispatchTest,
	"Cortex.Core.ZBlockedSaveDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCoreBlockedSaveDispatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString AssetPath = TEXT("/Game/Data/DT_TestSimple");
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	TestNotNull(TEXT("persisted fixture asset resolves"), Asset);
	if (!Asset) return false;
	FCortexAssetMutationGuard::Block(Asset, TEXT("forced recovery verification failure"));
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();
	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult SaveResult = Router.Execute(TEXT("core.save_asset"), SaveParams);
	TestTrue(TEXT("core save command returns its per-asset refusal result"), SaveResult.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	TestTrue(TEXT("core save result contains one asset"), SaveResult.Data.IsValid()
		&& SaveResult.Data->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() == 1);
	if (Results && Results->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Entry = (*Results)[0]->AsObject();
		FString ErrorCode;
		TestTrue(TEXT("core save is refused before save side effects"),
			Entry.IsValid() && Entry->TryGetStringField(TEXT("error"), ErrorCode));
		TestEqual(TEXT("core blocked save error code"), ErrorCode, CortexErrorCodes::InvalidOperation);
	}
	TestTrue(TEXT("core fingerprint read remains available"),
		ExecuteSingleFingerprintRequest(Router, AssetPath).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreBlockedReloadClearsBlockTest,
	"Cortex.Core.ZBlockedReloadClearsBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCoreBlockedReloadClearsBlockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString AssetPath = TEXT("/Game/Data/DT_TestSimple");
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	TestNotNull(TEXT("persisted fixture asset resolves"), Asset);
	if (!Asset) return false;
	FCortexAssetMutationGuard::Block(Asset, TEXT("forced recovery verification failure"));
	FCortexCommandRouter& Router =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore")).GetCommandRouter();
	FString Reason;

	TSharedPtr<FJsonObject> DryRunParams = MakeShared<FJsonObject>();
	DryRunParams->SetStringField(TEXT("asset_path"), AssetPath);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	TestTrue(TEXT("dry-run reload succeeds"), Router.Execute(TEXT("core.reload_asset"), DryRunParams).bSuccess);
	TestTrue(TEXT("dry-run reload keeps the block"), FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason));

	TSharedPtr<FJsonObject> ReloadParams = MakeShared<FJsonObject>();
	ReloadParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult Result = Router.Execute(TEXT("core.reload_asset"), ReloadParams);
	TestTrue(TEXT("reload succeeds"), Result.bSuccess);
	bool bReloaded = false;
	bool bBlockCleared = false;
	TestTrue(TEXT("reload reports reloaded"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("reloaded"), bReloaded) && bReloaded);
	TestTrue(TEXT("reload reports the cleared block"),
		Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("block_cleared"), bBlockCleared) && bBlockCleared);
	TestFalse(TEXT("reload from disk clears the block"), FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreBlockedPartialReloadKeepsBlockTest,
	"Cortex.Core.ZBlockedPartialReloadKeepsBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCoreBlockedPartialReloadKeepsBlockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString ReloadablePath = TEXT("/Game/Data/DT_TestSimple");
	const FString SkippedPath = TEXT("/Game/Data/CT_TestCurve");
	UObject* Reloadable = StaticLoadObject(UObject::StaticClass(), nullptr, *ReloadablePath);
	UObject* Skipped = StaticLoadObject(UObject::StaticClass(), nullptr, *SkippedPath);
	TestNotNull(TEXT("reloadable fixture resolves"), Reloadable);
	TestNotNull(TEXT("skipped fixture resolves"), Skipped);
	if (!Reloadable || !Skipped) return false;
	FCortexAssetMutationGuard::Block(Reloadable, TEXT("forced recovery verification failure"));
	FCortexAssetMutationGuard::Block(Skipped, TEXT("forced recovery verification failure"));

	// The engine refuses to reload an in-memory-only package even though its file exists, so one
	// batch member stays unreloaded while the other succeeds.
	UPackage* SkippedPackage = Skipped->GetOutermost();
	const bool bHadInMemoryOnly = SkippedPackage->HasAnyPackageFlags(PKG_InMemoryOnly);
	SkippedPackage->SetPackageFlags(PKG_InMemoryOnly);
	AddExpectedMessage(TEXT("cannot reload '/Game/Data/CT_TestCurve' as it is marked PKG_InMemoryOnly"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("asset_path"),
		{ MakeShared<FJsonValueString>(ReloadablePath), MakeShared<FJsonValueString>(SkippedPath) });
	const FCortexCommandResult Result = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRouter().Execute(TEXT("core.reload_asset"), Params);
	if (!bHadInMemoryOnly)
	{
		SkippedPackage->ClearPackageFlags(PKG_InMemoryOnly);
	}

	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	TestTrue(TEXT("batch reload returns two entries"), Result.Data.IsValid()
		&& Result.Data->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() == 2);
	if (Results && Results->Num() == 2)
	{
		bool bReloaded = true;
		(*Results)[1]->AsObject()->TryGetBoolField(TEXT("reloaded"), bReloaded);
		TestFalse(TEXT("the unreloaded package is reported as not reloaded"), bReloaded);
	}
	FString Reason;
	TestFalse(TEXT("the reloaded package is unblocked"), FCortexAssetMutationGuard::IsPathBlocked(ReloadablePath, Reason));
	TestTrue(TEXT("the unreloaded package stays blocked"), FCortexAssetMutationGuard::IsPathBlocked(SkippedPath, Reason));
	FCortexAssetMutationGuard::ClearPackage(FPackageName::ObjectPathToPackageName(SkippedPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreBlockedCorruptReloadKeepsBlockTest,
	"Cortex.Core.ZBlockedCorruptReloadKeepsBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCoreBlockedCorruptReloadKeepsBlockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString PackageName = TEXT("/Game/Temp/CortexCoreTest_CorruptReload/CT_CorruptReload");
	const FString AssetPath = PackageName + TEXT(".CT_CorruptReload");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*Filename, false, true, true);
	UPackage* Package = CreatePackage(*PackageName);
	UCurveTable* Asset = NewObject<UCurveTable>(Package, TEXT("CT_CorruptReload"), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Asset);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	TestTrue(TEXT("fixture saved to disk"), UPackage::SavePackage(Package, Asset, *Filename, SaveArgs));
	FCortexAssetMutationGuard::Block(Asset, TEXT("forced recovery verification failure"));

	// Expected engine diagnostics for the corrupted file (asset registry rescans may repeat them).
	AddExpectedMessage(TEXT("Package is unloadable: .*CT_CorruptReload\\.uasset"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 0, true);
	AddExpectedMessage(TEXT("The summary for the package '/Game/Temp/CortexCoreTest_CorruptReload/CT_CorruptReload' is invalid"),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0, false);
	AddExpectedMessage(TEXT("ReloadPackage failed to find a replacement object for 'CT_CorruptReload'"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0, false);
	// The file on disk can no longer produce the asset, so a reload must not count as a recovery.
	TArray<uint8> Garbage;
	Garbage.Init(0xAB, 64);
	TestTrue(TEXT("fixture file corrupted"), FFileHelper::SaveArrayToFile(Garbage, *Filename));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult Result = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"))
		.GetCommandRouter().Execute(TEXT("core.reload_asset"), Params);

	bool bReloaded = true;
	TestTrue(TEXT("reload reports an entry"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("reloaded"), bReloaded));
	TestFalse(TEXT("a corrupt package is not reported as reloaded"), bReloaded);
	FString Reason;
	TestTrue(TEXT("a corrupt package stays blocked"), FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason));

	FCortexAssetMutationGuard::ClearPackage(PackageName);
	if (UPackage* Remaining = FindPackage(nullptr, *PackageName))
	{
		ForEachObjectWithOuter(Remaining, [](UObject* Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		});
		Remaining->ClearFlags(RF_Standalone);
		Remaining->MarkAsGarbage();
	}
	IFileManager::Get().Delete(*Filename, false, true, true);
	IAssetRegistry::GetChecked().ScanPathsSynchronous({ TEXT("/Game/Temp/CortexCoreTest_CorruptReload") }, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreBlockedBatchSaveDispatchTest,
	"Cortex.Core.ZBlockedBatchSaveDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCoreBlockedBatchSaveDispatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString AssetPath = TEXT("/Game/Data/DT_TestSimple");
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	TestNotNull(TEXT("batch fixture asset resolves"), Asset);
	if (!Asset) return false;
	Asset->GetOutermost()->SetDirtyFlag(false);
	FCortexAssetMutationGuard::Block(Asset, TEXT("forced recovery verification failure"));
	FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("target"), AssetPath);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("items"), { MakeShared<FJsonValueObject>(Item) });
	const FCortexCommandResult Result = CoreModule.GetCommandRouter().Execute(TEXT("core.save_asset"), Params);
	TestFalse(TEXT("blocked batch save rejects before commit"), Result.bSuccess);
	TestEqual(TEXT("blocked batch save error code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestFalse(TEXT("blocked batch save leaves package clean"), Asset->GetOutermost()->IsDirty());
	return true;
}
