#include "Misc/AutomationTest.h"
#include "Operations/CortexDataAssetOps.h"
#include "Dom/JsonObject.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "ObjectTools.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/TextBuffer.h"
#include "Tests/CortexTestDataAsset.h"

namespace
{
	/** Clean up a test DataAsset from memory and disk. */
	void CleanupTestAsset(const FString& PackagePath)
	{
		// Guard LoadObject to prevent SkipPackage warnings.
		const FString PkgName = FPackageName::ObjectPathToPackageName(PackagePath);
		const FString ObjName = FPackageName::GetShortName(PkgName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PkgName, *ObjName);

		const bool bPackageLoaded = FindPackage(nullptr, *PkgName) != nullptr;
		const bool bPackageOnDisk = FPackageName::DoesPackageExist(PkgName);
		if (!bPackageLoaded && !bPackageOnDisk)
		{
			return;
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PkgName,
			FPackageName::GetAssetPackageExtension());

		UDataAsset* Asset = FindObject<UDataAsset>(nullptr, *ObjectPath);
		if (Asset == nullptr && bPackageOnDisk)
		{
			Asset = LoadObject<UDataAsset>(nullptr, *ObjectPath);
		}

		// Delete file first to avoid asset registry file watcher races.
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}

		if (Asset != nullptr)
		{
			UTextBuffer* DeleteGuard = nullptr;
			if (UPackage* Package = Asset->GetOutermost())
			{
				DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
			}

			TArray<UObject*> ToDelete;
			ToDelete.Add(Asset);
			ObjectTools::ForceDeleteObjects(ToDelete, false);

			if (DeleteGuard != nullptr)
			{
				DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetBasicTest,
	"Cortex.Data.DataAsset.CreateBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetBasicTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Basic");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestTrue(TEXT("Create should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		FString ReturnedPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), ReturnedPath);
		TestFalse(TEXT("Returned path should not be empty"), ReturnedPath.IsEmpty());

		FString ReturnedClass;
		Result.Data->TryGetStringField(TEXT("asset_class"), ReturnedClass);
		TestEqual(TEXT("Class should be CortexTestDataAsset"), ReturnedClass, TEXT("CortexTestDataAsset"));

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created flag should be true"), bCreated);
	}

	// Verify the asset can be loaded.
	UDataAsset* LoadedAsset = LoadObject<UDataAsset>(nullptr, *TestPath);
	TestNotNull(TEXT("Asset should be loadable after create"), LoadedAsset);

	// Verify on-disk file exists.
	const FString Filename = FPackageName::LongPackageNameToFilename(
		TestPath,
		FPackageName::GetAssetPackageExtension());
	TestTrue(
		TEXT("File should exist on disk"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename));

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetInvalidClassTest,
	"Cortex.Data.DataAsset.CreateInvalidClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetInvalidClassTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("NonExistentDataAssetClass_XYZ"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_CreateDA_Invalid"));

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestFalse(TEXT("Create with invalid class should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be CLASS_NOT_FOUND"), Result.ErrorCode, TEXT("CLASS_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetAbstractClassTest,
	"Cortex.Data.DataAsset.CreateAbstractClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetAbstractClassTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("DataAsset"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_CreateDA_Abstract"));

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestFalse(TEXT("Create with abstract class should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_OPERATION"), Result.ErrorCode, TEXT("INVALID_OPERATION"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetDuplicateTest,
	"Cortex.Data.DataAsset.CreateDuplicatePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetDuplicateTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Dup");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result1 = FCortexDataAssetOps::CreateDataAsset(Params);
	TestTrue(TEXT("First create should succeed"), Result1.bSuccess);

	FCortexCommandResult Result2 = FCortexDataAssetOps::CreateDataAsset(Params);
	TestFalse(TEXT("Duplicate create should fail"), Result2.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_ALREADY_EXISTS"), Result2.ErrorCode, TEXT("ASSET_ALREADY_EXISTS"));

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetWithPropertiesTest,
	"Cortex.Data.DataAsset.CreateWithProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetWithPropertiesTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Props");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("TestProperty"), TEXT("Hello from CRUD"));
	Props->SetNumberField(TEXT("TestNumber"), 42);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);
	Params->SetObjectField(TEXT("properties"), Props);

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestTrue(TEXT("Create with properties should succeed"), Result.bSuccess);

	// Load and verify properties were applied.
	const FString ObjPath = FString::Printf(TEXT("%s.%s"),
		*TestPath, *FPackageName::GetShortName(TestPath));
	UCortexTestDataAsset* Asset = LoadObject<UCortexTestDataAsset>(nullptr, *ObjPath);
	TestNotNull(TEXT("Asset should be loadable"), Asset);

	if (Asset != nullptr)
	{
		TestEqual(TEXT("TestProperty should be set"), Asset->TestProperty, TEXT("Hello from CRUD"));
		TestEqual(TEXT("TestNumber should be set"), Asset->TestNumber, 42);
	}

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteDataAssetBasicTest,
	"Cortex.Data.DataAsset.DeleteBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteDataAssetBasicTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_DeleteDA");

	CleanupTestAsset(TestPath);

	// Create a DataAsset manually to delete.
	const FString AssetName = FPackageName::GetShortName(TestPath);
	UPackage* TestPkg = CreatePackage(*TestPath);
	UDataAsset* TestAsset = NewObject<UDataAsset>(
		TestPkg,
		UCortexTestDataAsset::StaticClass(),
		FName(*AssetName),
		RF_Public | RF_Standalone);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		TestPkg->GetName(),
		FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(TestPkg, TestAsset, *Filename, SaveArgs);
	TestTrue(TEXT("Setup: asset should save to disk"), bSaved);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result = FCortexDataAssetOps::DeleteDataAsset(Params);
	TestTrue(TEXT("Delete should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bDeleted = false;
		Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
		TestTrue(TEXT("deleted flag should be true"), bDeleted);
	}

	TestFalse(
		TEXT("File should be deleted from disk"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteDataAssetNotFoundTest,
	"Cortex.Data.DataAsset.DeleteNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteDataAssetNotFoundTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_DeleteDA_NotExist_XYZ"));

	FCortexCommandResult Result = FCortexDataAssetOps::DeleteDataAsset(Params);

	TestFalse(TEXT("Delete of non-existent asset should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}
