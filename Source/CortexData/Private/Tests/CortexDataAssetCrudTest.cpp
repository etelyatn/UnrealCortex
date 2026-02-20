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
