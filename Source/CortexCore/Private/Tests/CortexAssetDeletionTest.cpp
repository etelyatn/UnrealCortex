#include "Misc/AutomationTest.h"
#include "CortexCoreCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/ObjectLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetNotFoundTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
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
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
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
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
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
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("delete_folder"), Params);

	TestFalse(TEXT("Should fail without folder_path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

namespace CortexAssetDeletionTest
{
/** Owned fixtures live under /Game/Temp/CortexIssue218_<guid>/ so a failing test can never
 *  delete real project or engine content. */
struct FIssue218Fixture
{
	FString Root;
	TArray<FString> SavedFiles;
	TArray<TWeakObjectPtr<UObject>> CreatedObjects;

	FIssue218Fixture()
	{
		Root = FString::Printf(
			TEXT("/Game/Temp/CortexIssue218_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	~FIssue218Fixture()
	{
		TArray<UObject*> Remaining;
		for (const TWeakObjectPtr<UObject>& WeakObject : CreatedObjects)
		{
			if (UObject* Object = WeakObject.Get())
			{
				Remaining.Add(Object);
			}
		}
		if (Remaining.Num() > 0)
		{
			// Deleted through the shared helper so the cleanup also gets its empty-package guard and no
			// "marked as deleted in editor, but modified on disk" warnings reach the test report.
			FCortexEditorUtils::DeleteLoadedAssets(Remaining);
		}

		const FString Directory = FPackageName::LongPackageNameToFilename(Root);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	}

	FString MakePackageName(const FString& RelativePath) const
	{
		return FString::Printf(TEXT("%s/%s"), *Root, *RelativePath);
	}

	static FString PackageFilename(const FString& PackageName)
	{
		return FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
	}

	/** Saves a UObjectLibrary asset. A non-null ReferencedObject becomes a strong entry of the
	 *  library, which serializes as a package dependency of the saved asset. */
	UObjectLibrary* CreateLibrary(const FString& RelativePath, UObject* ReferencedObject)
	{
		const FString PackageName = MakePackageName(RelativePath);
		UPackage* Package = CreatePackage(*PackageName);
		UObjectLibrary* Library = NewObject<UObjectLibrary>(
			Package,
			*FPackageName::GetShortName(PackageName),
			RF_Public | RF_Standalone);
		if (Library == nullptr)
		{
			return nullptr;
		}

		Library->ObjectBaseClass = UObject::StaticClass();
		Library->bHasBlueprintClasses = false;
		Library->UseWeakReferences(false);
		if (ReferencedObject != nullptr)
		{
			Library->AddObject(ReferencedObject);
		}

		const FString Filename = PackageFilename(PackageName);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Library, *Filename, SaveArgs))
		{
			return nullptr;
		}

		SavedFiles.Add(Filename);
		CreatedObjects.Add(Library);
		return Library;
	}

	/** Re-reads the saved fixtures from disk so the asset registry knows their dependencies. */
	void ScanSavedFiles() const
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().ScanFilesSynchronous(SavedFiles, true);
	}

	static FCortexCommandResult RunDeleteAsset(
		FCortexCoreCommandHandler& Handler,
		const FString& AssetPath,
		const bool bForce)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("force"), bForce);
		return Handler.Execute(TEXT("delete_asset"), Params);
	}

	static FCortexCommandResult RunDeleteFolder(
		FCortexCoreCommandHandler& Handler,
		const FString& FolderPath,
		const bool bForce)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("folder_path"), FolderPath);
		Params->SetBoolField(TEXT("force"), bForce);
		return Handler.Execute(TEXT("delete_folder"), Params);
	}

	static TArray<FString> ReadReferencers(const FCortexCommandResult& Result)
	{
		TArray<FString> Referencers;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Result.ErrorDetails.IsValid()
			&& Result.ErrorDetails->TryGetArrayField(TEXT("referencers"), Values)
			&& Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Referencers.Add(Value->AsString());
			}
		}
		return Referencers;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetRejectsNonWritableMountTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.RejectsNonWritableMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetRejectsNonWritableMountTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);

	const FCortexCommandResult Result = CortexAssetDeletionTest::FIssue218Fixture::RunDeleteAsset(
		Handler,
		TEXT("/Engine/CortexIssue218Missing/Asset"),
		/*bForce=*/false);

	TestFalse(TEXT("Non-writable mount is refused"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderRejectsNonWritableMountTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.RejectsNonWritableMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderRejectsNonWritableMountTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);

	const FCortexCommandResult Result = CortexAssetDeletionTest::FIssue218Fixture::RunDeleteFolder(
		Handler,
		TEXT("/Engine/CortexIssue218Missing"),
		/*bForce=*/false);

	TestFalse(TEXT("Non-writable mount is refused"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderRejectsContentRootTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.RejectsContentRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderRejectsContentRootTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	const FString TestRoot = TEXT("/CortexIssue218Root");

	FCortexEditorUtils::AddTestWritableContentRoot(TestRoot);
	const FCortexCommandResult Result =
		CortexAssetDeletionTest::FIssue218Fixture::RunDeleteFolder(Handler, TestRoot, /*bForce=*/false);
	FCortexEditorUtils::RemoveTestWritableContentRoot(TestRoot);

	TestFalse(TEXT("A bare content root is refused"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetRefusesReferencedWithoutForceTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.RefusesReferencedWithoutForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetRefusesReferencedWithoutForceTest::RunTest(const FString& Parameters)
{
	using namespace CortexAssetDeletionTest;

	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	FIssue218Fixture Fixture;

	UObjectLibrary* Target = Fixture.CreateLibrary(TEXT("Targets/B"), nullptr);
	if (!TestNotNull(TEXT("Target asset"), Target))
	{
		return false;
	}
	UObjectLibrary* Referencer = Fixture.CreateLibrary(TEXT("Referrers/A"), Target);
	if (!TestNotNull(TEXT("Referencing asset"), Referencer))
	{
		return false;
	}
	Fixture.ScanSavedFiles();

	const FString TargetPackage = Fixture.MakePackageName(TEXT("Targets/B"));
	const FString ReferencerPackage = Fixture.MakePackageName(TEXT("Referrers/A"));
	const FString TargetFilename = FIssue218Fixture::PackageFilename(TargetPackage);
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"), *TargetPackage, *FPackageName::GetShortName(TargetPackage));

	const FCortexCommandResult Refused = FIssue218Fixture::RunDeleteAsset(Handler, ObjectPath, /*bForce=*/false);

	TestFalse(TEXT("A referenced asset is refused without force"), Refused.bSuccess);
	TestEqual(TEXT("Error code should be HAS_REFERENCES"),
		Refused.ErrorCode, CortexErrorCodes::HasReferences);
	const TArray<FString> Referencers = FIssue218Fixture::ReadReferencers(Refused);
	TestTrue(TEXT("The referencer package is reported"), Referencers.Contains(ReferencerPackage));
	TestTrue(TEXT("The asset file survives the refusal"), IFileManager::Get().FileExists(*TargetFilename));

	const FCortexCommandResult Forced = FIssue218Fixture::RunDeleteAsset(Handler, ObjectPath, /*bForce=*/true);

	TestTrue(TEXT("The forced delete succeeds"), Forced.bSuccess);
	TestFalse(TEXT("The asset file is removed by the forced delete"), IFileManager::Get().FileExists(*TargetFilename));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteAssetKeepsFileWhenEngineRefusesTest,
	"Cortex.Core.AssetDeletion.DeleteAsset.KeepsFileWhenEngineRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteAssetKeepsFileWhenEngineRefusesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAssetDeletionTest;

	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	FIssue218Fixture Fixture;

	UObjectLibrary* Target = Fixture.CreateLibrary(TEXT("Targets/B"), nullptr);
	if (!TestNotNull(TEXT("Target asset"), Target))
	{
		return false;
	}
	Fixture.ScanSavedFiles();

	const FString TargetPackage = Fixture.MakePackageName(TEXT("Targets/B"));
	const FString TargetFilename = FIssue218Fixture::PackageFilename(TargetPackage);

	FDelegateHandle VetoHandle = FEditorDelegates::OnAssetsCanDelete.AddLambda(
		[Target](const TArray<UObject*>& ObjectsToDelete, FCanDeleteAssetResult& Result)
		{
			if (ObjectsToDelete.Contains(Target))
			{
				Result.Set(false);
			}
		});

	const FCortexCommandResult Result = FIssue218Fixture::RunDeleteAsset(
		Handler, Target->GetPathName(), /*bForce=*/false);
	FEditorDelegates::OnAssetsCanDelete.Remove(VetoHandle);

	TestFalse(TEXT("An engine veto fails the command"), Result.bSuccess);
	TestEqual(TEXT("Error code should be SERIALIZATION_ERROR"),
		Result.ErrorCode, CortexErrorCodes::SerializationError);
	TestTrue(TEXT("The asset file survives the engine veto"), IFileManager::Get().FileExists(*TargetFilename));
	TestTrue(TEXT("The asset object survives the engine veto"), IsValid(Target));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderBatchesWithInternalReferencesTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.BatchesWithInternalReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderBatchesWithInternalReferencesTest::RunTest(const FString& Parameters)
{
	using namespace CortexAssetDeletionTest;

	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	FIssue218Fixture Fixture;

	const FString Folder = Fixture.MakePackageName(TEXT("Folder"));
	UObjectLibrary* Target = Fixture.CreateLibrary(TEXT("Folder/B"), nullptr);
	if (!TestNotNull(TEXT("Target asset"), Target))
	{
		return false;
	}
	UObjectLibrary* Referencer = Fixture.CreateLibrary(TEXT("Folder/A"), Target);
	if (!TestNotNull(TEXT("Referencing asset"), Referencer))
	{
		return false;
	}
	Fixture.ScanSavedFiles();

	const FString TargetFilename = FIssue218Fixture::PackageFilename(Fixture.MakePackageName(TEXT("Folder/B")));
	const FString ReferencerFilename = FIssue218Fixture::PackageFilename(Fixture.MakePackageName(TEXT("Folder/A")));

	int32 BroadcastCount = 0;
	FDelegateHandle BroadcastHandle = FEditorDelegates::OnPreForceDeleteObjects.AddLambda(
		[&BroadcastCount, Target, Referencer](const TArray<UObject*>& ObjectsToDelete)
		{
			if (ObjectsToDelete.Contains(Target) || ObjectsToDelete.Contains(Referencer))
			{
				++BroadcastCount;
			}
		});

	const FCortexCommandResult Result = FIssue218Fixture::RunDeleteFolder(Handler, Folder, /*bForce=*/false);
	FEditorDelegates::OnPreForceDeleteObjects.Remove(BroadcastHandle);

	TestTrue(TEXT("Internal references do not block the folder delete"), Result.bSuccess);
	if (TestTrue(TEXT("Folder delete returns data"), Result.Data.IsValid()))
	{
		double DeletedCount = -1;
		Result.Data->TryGetNumberField(TEXT("deleted_count"), DeletedCount);
		TestEqual(TEXT("Both assets are deleted"), static_cast<int32>(DeletedCount), 2);
	}
	TestFalse(TEXT("The target file is gone"), IFileManager::Get().FileExists(*TargetFilename));
	TestFalse(TEXT("The referencer file is gone"), IFileManager::Get().FileExists(*ReferencerFilename));
	TestEqual(TEXT("The batch delete broadcasts OnPreForceDeleteObjects once"), BroadcastCount, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteFolderRefusesExternalReferencesWithoutForceTest,
	"Cortex.Core.AssetDeletion.DeleteFolder.RefusesExternalReferencesWithoutForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteFolderRefusesExternalReferencesWithoutForceTest::RunTest(const FString& Parameters)
{
	using namespace CortexAssetDeletionTest;

	FCortexCommandRouter TestRouter;
	FCortexCoreCommandHandler Handler(&TestRouter);
	FIssue218Fixture Fixture;

	UObjectLibrary* Target = Fixture.CreateLibrary(TEXT("Folder/B"), nullptr);
	if (!TestNotNull(TEXT("Target asset"), Target))
	{
		return false;
	}
	UObjectLibrary* Referencer = Fixture.CreateLibrary(TEXT("Outside/A"), Target);
	if (!TestNotNull(TEXT("Referencing asset"), Referencer))
	{
		return false;
	}
	Fixture.ScanSavedFiles();

	const FString Folder = Fixture.MakePackageName(TEXT("Folder"));
	const FString TargetPackage = Fixture.MakePackageName(TEXT("Folder/B"));
	const FString TargetFilename = FIssue218Fixture::PackageFilename(TargetPackage);
	const FString ReferencerPackage = Fixture.MakePackageName(TEXT("Outside/A"));

	const FCortexCommandResult Result = FIssue218Fixture::RunDeleteFolder(Handler, Folder, /*bForce=*/false);

	TestFalse(TEXT("An externally referenced folder is refused without force"), Result.bSuccess);
	TestEqual(TEXT("Error code should be HAS_REFERENCES"),
		Result.ErrorCode, CortexErrorCodes::HasReferences);
	const TArray<FString> Referencers = FIssue218Fixture::ReadReferencers(Result);
	TestTrue(TEXT("The external referencer package is reported"), Referencers.Contains(ReferencerPackage));
	TestTrue(TEXT("The asset file survives the refusal"), IFileManager::Get().FileExists(*TargetFilename));

	return true;
}
