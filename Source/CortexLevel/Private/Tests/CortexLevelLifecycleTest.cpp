#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "GameFramework/Actor.h"

namespace
{
	FCortexCommandRouter CreateLifecycleRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
			MakeShared<FCortexLevelCommandHandler>());
		return Router;
	}

	/** Registers a throwaway plugin-style mount backed by a directory under project Saved.
	 *  Only the writable variant is added to the shared writable-root policy.
	 *  When the backing directory cannot be created the fixture stays unmounted and
	 *  invalid, so the calling test fails instead of passing without a real root. */
	class FScopedLifecycleTestMount
	{
	public:
		FScopedLifecycleTestMount(
			const FString& InRoot,
			const FString& InDirectory,
			bool bInWritable)
			: Root(InRoot)
			, Directory(InDirectory)
			, bWritable(bInWritable)
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
			bSetupSucceeded = IFileManager::Get().DirectoryExists(*Directory);
			if (!bSetupSucceeded)
			{
				return;
			}

			FPackageName::RegisterMountPoint(Root, Directory);
			if (bWritable)
			{
				FCortexEditorUtils::AddTestWritableContentRoot(Root);
			}
		}

		~FScopedLifecycleTestMount()
		{
			if (!bSetupSucceeded)
			{
				return;
			}

			if (bWritable)
			{
				FCortexEditorUtils::RemoveTestWritableContentRoot(Root);
			}
			FPackageName::UnRegisterMountPoint(Root, Directory);
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		/** True when the backing directory exists and the mount is registered. */
		bool IsValid() const
		{
			return bSetupSucceeded;
		}

	private:
		FString Root;
		FString Directory;
		bool bWritable = false;
		bool bSetupSucceeded = false;
	};

	class FWorldAssetLifecycleCommand : public IAutomationLatentCommand
	{
	public:
		FWorldAssetLifecycleCommand(FAutomationTestBase* InTest, TSharedPtr<FScopedLifecycleTestMount> InMount, FString InRoot)
			: Test(InTest), Mount(MoveTemp(InMount)), Root(MoveTemp(InRoot)), Router(CreateLifecycleRouter())
		{
		}

		virtual bool Update() override
		{
			const FString Path = Root / (CaseIndex == 0 ? TEXT("Blank") : TEXT("Template"));
			const FString RenamedPath = Path + TEXT("Renamed");
			switch (Phase)
			{
			case 0:
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("path"), Path);
				Params->SetBoolField(TEXT("open"), false);
				if (CaseIndex == 1)
				{
					Params->SetStringField(TEXT("template"), TEXT("/Engine/Maps/Templates/Template_Default"));
				}
				const FCortexCommandResult Created = Router.Execute(TEXT("level.create_level"), Params);
				bCreated = Test->TestTrue(FString::Printf(TEXT("Create %s: %s"), *Path, *Created.ErrorMessage), Created.bSuccess);
				if (bCreated)
				{
					UPackage* Package = FindPackage(nullptr, *Path);
					UWorld* World = Package ? FindObject<UWorld>(Package, *FPackageName::GetShortName(Path)) : nullptr;
					if (Test->TestNotNull(TEXT("Created world remains resident"), World))
					{
						Test->TestTrue(TEXT("Resident world is immediately an asset"), World->IsAsset());
					}
				}
				break;
			}
			case 1:
			{
				if (bCreated)
				{
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("path"), Path);
					Params->SetStringField(TEXT("new_path"), RenamedPath);
					const FCortexCommandResult Renamed = Router.Execute(TEXT("level.rename_level"), Params);
					Test->TestTrue(FString::Printf(TEXT("Same-session rename %s: %s"), *Path, *Renamed.ErrorMessage), Renamed.bSuccess);
					Test->TestTrue(TEXT("Renamed map exists"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(RenamedPath, FPackageName::GetMapPackageExtension())));
					Test->TestFalse(TEXT("Original map no longer exists"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Path, FPackageName::GetMapPackageExtension())));
				}
				break;
			}
			case 2:
			{
				// Gathering may still hold map files opened by the previous frame's rename.
				IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				AssetRegistry.WaitForCompletion();
				FlushAsyncLoading();
				AssetRegistry.Tick(-1.0f);
				for (const FString& PackagePath : { Path, RenamedPath })
				{
					IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetMapPackageExtension()), false, true);
					if (UPackage* Package = FindPackage(nullptr, *PackagePath))
					{
						if (UObject* Asset = FindObject<UObject>(Package, *FPackageName::GetShortName(PackagePath)))
						{
							if (Asset->IsAsset())
							{
								FAssetRegistryModule::AssetDeleted(Asset);
							}
							Asset->ClearFlags(RF_Public | RF_Standalone);
							Asset->MarkAsGarbage();
						}
						Package->SetDirtyFlag(false);
						Package->MarkAsGarbage();
					}
				}
				break;
			}
			case 3:
			{
				if (++CaseIndex == 2)
				{
					Mount.Reset();
					return true;
				}
				Phase = 0;
				return false;
			}
			}
			// Let queued editor and filesystem events settle before the next lifecycle operation.
			++Phase;
			return false;
		}

	private:
		FAutomationTestBase* Test;
		TSharedPtr<FScopedLifecycleTestMount> Mount;
		FString Root;
		FCortexCommandRouter Router;
		int32 CaseIndex = 0;
		int32 Phase = 0;
		bool bCreated = false;
	};

	/** Drives one Level lifecycle command under test across engine frames: create the
	 *  fixture map, exercise rename/delete, drain AssetRegistry gathers and retire only the
	 *  fixture assets/packages, then release the backing mount. Create, the command under
	 *  test and fixture retirement must land in separate frames; otherwise the registry's
	 *  asynchronous gather re-reports the just-saved map after the same frame already
	 *  deleted it, and warns that the deleted package was "modified on disk". */
	class FLevelMutationFixtureCommand : public IAutomationLatentCommand
	{
	public:
		enum class EOperation
		{
			Rename,
			Delete
		};

		FLevelMutationFixtureCommand(FAutomationTestBase* InTest, TSharedPtr<FScopedLifecycleTestMount> InMount,
			FString InRoot, EOperation InOperation)
			: Test(InTest), Mount(MoveTemp(InMount)), Root(MoveTemp(InRoot)), Operation(InOperation), Router(CreateLifecycleRouter())
		{
		}

		virtual bool Update() override
		{
			const FString OriginalPath = Root + TEXT("Original");
			const FString NewPath = Root + TEXT("Renamed");
			switch (Phase)
			{
			case 0:
			{
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("path"), OriginalPath);
				Params->SetBoolField(TEXT("open"), false);
				const FCortexCommandResult Created = Router.Execute(TEXT("level.create_level"), Params);
				bCreated = Test->TestTrue(FString::Printf(TEXT("Create %s: %s"), *OriginalPath, *Created.ErrorMessage), Created.bSuccess);
				break;
			}
			case 1:
			{
				if (!bCreated)
				{
					break;
				}

				if (Operation == EOperation::Rename)
				{
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("path"), OriginalPath);
					Params->SetStringField(TEXT("new_path"), NewPath);
					const FCortexCommandResult Renamed = Router.Execute(TEXT("level.rename_level"), Params);
					Test->TestTrue(FString::Printf(TEXT("rename_level should succeed: %s"), *Renamed.ErrorMessage), Renamed.bSuccess);
					if (Renamed.bSuccess && Renamed.Data.IsValid())
					{
						Test->TestEqual(TEXT("old_path should match"), Renamed.Data->GetStringField(TEXT("old_path")), OriginalPath);
						Test->TestEqual(TEXT("new_path should match"), Renamed.Data->GetStringField(TEXT("new_path")), NewPath);
					}
					Test->TestTrue(TEXT("Renamed map exists"),
						IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(NewPath, FPackageName::GetMapPackageExtension())));
					Test->TestFalse(TEXT("Original map no longer exists"),
						IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(OriginalPath, FPackageName::GetMapPackageExtension())));
				}
				else
				{
					TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
					Params->SetStringField(TEXT("path"), OriginalPath);
					const FCortexCommandResult Deleted = Router.Execute(TEXT("level.delete_level"), Params);
					Test->TestTrue(FString::Printf(TEXT("delete_level should succeed: %s"), *Deleted.ErrorMessage), Deleted.bSuccess);
					if (Deleted.bSuccess && Deleted.Data.IsValid())
					{
						Test->TestEqual(TEXT("deleted_path should match"), Deleted.Data->GetStringField(TEXT("deleted_path")), OriginalPath);
					}
					Test->TestFalse(TEXT("File should be deleted"),
						IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(OriginalPath, FPackageName::GetMapPackageExtension())));
				}
				break;
			}
			case 2:
			{
				// Gathering may still hold or re-report map files touched by the frame above.
				IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				AssetRegistry.WaitForCompletion();
				FlushAsyncLoading();
				AssetRegistry.Tick(-1.0f);
				for (const FString& PackagePath : { OriginalPath, NewPath })
				{
					IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetMapPackageExtension()), false, true);
					if (UPackage* Package = FindPackage(nullptr, *PackagePath))
					{
						if (UObject* Asset = FindObject<UObject>(Package, *FPackageName::GetShortName(PackagePath)))
						{
							if (Asset->IsAsset())
							{
								FAssetRegistryModule::AssetDeleted(Asset);
							}
							Asset->ClearFlags(RF_Public | RF_Standalone);
							Asset->MarkAsGarbage();
						}
						Package->SetDirtyFlag(false);
						Package->MarkAsGarbage();
					}
				}
				break;
			}
			case 3:
			{
				// Release the backing directory only after the settlement frame above.
				Mount.Reset();
				return true;
			}
			}
			// Let queued editor and filesystem events settle before the next lifecycle operation.
			++Phase;
			return false;
		}

	private:
		FAutomationTestBase* Test;
		TSharedPtr<FScopedLifecycleTestMount> Mount;
		FString Root;
		EOperation Operation;
		FCortexCommandRouter Router;
		int32 Phase = 0;
		bool bCreated = false;
	};

	// Source and destination for duplicate_level refusal tests. The source is checked-in sandbox
	// content that the refusal paths only read; the destination must never be written.
	const TCHAR* const DuplicateRefusalSource = TEXT("/Game/Maps/TestRoomMap");
	const TCHAR* const DuplicateRefusalDest = TEXT("/Game/Maps/_CortexTest/TestDuplicateRefused");

	TSharedPtr<FJsonObject> MakeDuplicateRefusalParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_path"), DuplicateRefusalSource);
		Params->SetStringField(TEXT("dest_path"), DuplicateRefusalDest);
		return Params;
	}

	/** Removes a destination written by a regressed duplicate_level so later runs start clean. */
	void DeleteDuplicateRefusalDest()
	{
		const FString DestFile = FPackageName::LongPackageNameToFilename(DuplicateRefusalDest, FPackageName::GetMapPackageExtension());
		IFileManager::Get().Delete(*DestFile, false, true);
		if (UPackage* DestPackage = FindPackage(nullptr, DuplicateRefusalDest))
		{
			DestPackage->MarkAsGarbage();
		}
		const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
		IFileManager::Get().DeleteDirectory(*TestDir, false, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelInvalidPathTest,
	"Cortex.Level.Lifecycle.CreateLevel.InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelInvalidPathTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("InvalidPath/NoSlash"));

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestFalse(TEXT("Should fail for invalid path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_PARAMETER"), Result.ErrorCode, TEXT("INVALID_PARAMETER"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelListTemplatesTest,
	"Cortex.Level.Lifecycle.ListTemplates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListTemplatesTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("level.list_templates"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_templates should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Templates = nullptr;
		TestTrue(TEXT("Should have templates array"), Result.Data->TryGetArrayField(TEXT("templates"), Templates));

		if (Templates)
		{
			TestTrue(TEXT("Should have at least one template"), Templates->Num() > 0);

			for (const TSharedPtr<FJsonValue>& Value : *Templates)
			{
				const TSharedPtr<FJsonObject>* TemplateObj = nullptr;
				if (Value->TryGetObject(TemplateObj) && TemplateObj && TemplateObj->IsValid())
				{
					TestTrue(TEXT("Template should have name"), (*TemplateObj)->HasField(TEXT("name")));
					TestTrue(TEXT("Template should have path"), (*TemplateObj)->HasField(TEXT("path")));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelTest,
	"Cortex.Level.Lifecycle.CreateLevel.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	const FString TestPath = TEXT("/Game/Maps/_CortexTest/TestCreateEmpty");
	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Record current level to verify no world transition
	UWorld* WorldBefore = GEditor->GetEditorWorldContext().World();
	const FString LevelBefore = WorldBefore ? WorldBefore->GetOutermost()->GetName() : TEXT("");

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TestPath);
	// open defaults to false

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestTrue(TEXT("create_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("Path should match"), Result.Data->GetStringField(TEXT("path")), TestPath);
		TestTrue(TEXT("Should have world_partition field"), Result.Data->HasField(TEXT("world_partition")));

		// Verify no world transition happened
		UWorld* WorldAfter = GEditor->GetEditorWorldContext().World();
		const FString LevelAfter = WorldAfter ? WorldAfter->GetOutermost()->GetName() : TEXT("");
		TestEqual(TEXT("Active level should not change"), LevelBefore, LevelAfter);
	}

	// Cleanup: delete the created asset
	const FString FilePath = FPackageName::LongPackageNameToFilename(TestPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*FilePath, false, true);
	// Clean in-memory package
	UPackage* Pkg = FindPackage(nullptr, *TestPath);
	if (Pkg)
	{
		Pkg->MarkAsGarbage();
	}
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelExistsTest,
	"Cortex.Level.Lifecycle.CreateLevel.AlreadyExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelExistsTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Use the current level's path — it definitely exists
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddInfo(TEXT("No editor world - skipping"));
		return true;
	}

	const FString ExistingPath = World->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), ExistingPath);

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestFalse(TEXT("Should fail for existing path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_ALREADY_EXISTS"), Result.ErrorCode, TEXT("ASSET_ALREADY_EXISTS"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelTest,
	"Cortex.Level.Lifecycle.OpenLevel.Success",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelOpenLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Save original level for restore after test
	UWorld* WorldBefore = GEditor->GetEditorWorldContext().World();
	const FString OriginalLevelPath = WorldBefore ? WorldBefore->GetOutermost()->GetName() : TEXT("");
	const FString OriginalLevelFile = OriginalLevelPath.IsEmpty() ? TEXT("") :
		FPackageName::LongPackageNameToFilename(OriginalLevelPath, FPackageName::GetMapPackageExtension());

	// First, create a test level to open
	const FString TestPath = TEXT("/Game/Maps/_CortexTest/TestOpenLevel");

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("path"), TestPath);
	FCortexCommandResult CreateResult = Router.Execute(TEXT("level.create_level"), CreateParams);
	if (!CreateResult.bSuccess)
	{
		AddInfo(TEXT("Could not create test level - skipping"));
		return true;
	}

	// Now open it
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("path"), TestPath);
	OpenParams->SetBoolField(TEXT("force"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), OpenParams);
	TestTrue(TEXT("open_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have name field"), Result.Data->HasField(TEXT("name")));
		TestTrue(TEXT("Should have path field"), Result.Data->HasField(TEXT("path")));
		TestTrue(TEXT("Should have actor_count field"), Result.Data->HasField(TEXT("actor_count")));
		TestTrue(TEXT("Should have world_partition field"), Result.Data->HasField(TEXT("world_partition")));
	}

	// Cleanup: delete the test level file (quiet=true to suppress locked-file errors in NullRHI mode)
	const FString FilePath = FPackageName::LongPackageNameToFilename(TestPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*FilePath, false, true, true);
	// Clean in-memory package
	UPackage* Pkg = FindPackage(nullptr, *TestPath);
	if (Pkg)
	{
		Pkg->MarkAsGarbage();
	}
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

	// Restore original level so subsequent tests run in the correct world context
	if (!OriginalLevelFile.IsEmpty() && IFileManager::Get().FileExists(*OriginalLevelFile))
	{
		UEditorLoadingAndSavingUtils::LoadMap(OriginalLevelFile);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelNotFoundTest,
	"Cortex.Level.Lifecycle.OpenLevel.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelOpenLevelNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/Game/Maps/NonExistentLevel_XYZ"));

	FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Should fail for non-existent level"), Result.bSuccess);
	TestEqual(TEXT("Error should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelWritableMountedRootTest,
	"Cortex.Level.Lifecycle.OpenLevel.WritableMountedRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelOpenLevelWritableMountedRootTest::RunTest(const FString& Parameters)
{
	const FString Root = TEXT("/CortexLifecycleWritable/");
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexLifecycleWritable");
	FScopedLifecycleTestMount Mount(Root, Dir, true);
	if (!TestTrue(TEXT("Writable test mount fixture must be created"), Mount.IsValid()))
	{
		return false;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), Root + TEXT("Maps/NoSuchLevel_XYZ"));

	const FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Missing level should fail"), Result.bSuccess);
	TestEqual(TEXT("Writable mounted root must pass validation and reach lookup"),
		Result.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelRegisteredNonWritableRootTest,
	"Cortex.Level.Lifecycle.OpenLevel.RegisteredNonWritableRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelOpenLevelRegisteredNonWritableRootTest::RunTest(const FString& Parameters)
{
	const FString Root = TEXT("/CortexLifecycleReadOnly/");
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexLifecycleReadOnly");
	FScopedLifecycleTestMount Mount(Root, Dir, false);
	if (!TestTrue(TEXT("Registered non-writable test mount fixture must be created"), Mount.IsValid()))
	{
		return false;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), Root + TEXT("Maps/NoSuchLevel_XYZ"));

	const FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Registered non-writable root must fail"), Result.bSuccess);
	TestEqual(TEXT("Registered non-writable root must be rejected before lookup"),
		Result.ErrorCode, FString(TEXT("INVALID_PARAMETER")));
	TestTrue(TEXT("Rejection message must name the mounted root"),
		Result.ErrorMessage.Contains(TEXT("/CortexLifecycleReadOnly")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelUnknownRootTest,
	"Cortex.Level.Lifecycle.OpenLevel.UnknownRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelOpenLevelUnknownRootTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/CortexLifecycleUnknown/Maps/NoSuchLevel_XYZ"));

	const FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Unknown root must fail"), Result.bSuccess);
	TestEqual(TEXT("Unknown root must be rejected before lookup"),
		Result.ErrorCode, FString(TEXT("INVALID_PARAMETER")));
	TestTrue(TEXT("Rejection message must name the unknown root"),
		Result.ErrorMessage.Contains(TEXT("/CortexLifecycleUnknown")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelEngineRootTest,
	"Cortex.Level.Lifecycle.OpenLevel.EngineRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelOpenLevelEngineRootTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/Engine/Maps/NoSuchLevel_XYZ"));

	const FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Engine root must fail"), Result.bSuccess);
	TestEqual(TEXT("Engine root is never writable"), Result.ErrorCode, FString(TEXT("INVALID_PARAMETER")));
	TestTrue(TEXT("Rejection message must name the engine root"),
		Result.ErrorMessage.Contains(TEXT("/Engine")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelTraversalPathTest,
	"Cortex.Level.Lifecycle.OpenLevel.TraversalPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexLevelOpenLevelTraversalPathTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("/Game/../Engine/Foo"));

	const FCortexCommandResult Result = Router.Execute(TEXT("level.open_level"), Params);
	TestFalse(TEXT("Traversal path must fail"), Result.bSuccess);
	TestEqual(TEXT("Traversal path must not be normalized into writable content"),
		Result.ErrorCode, FString(TEXT("INVALID_PARAMETER")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDuplicateLevelTest,
	"Cortex.Level.Lifecycle.DuplicateLevel.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDuplicateLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Create a source level
	const FString SourcePath = TEXT("/Game/Maps/_CortexTest/TestDuplicateSource");
	const FString DestPath = TEXT("/Game/Maps/_CortexTest/TestDuplicateDest");

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("path"), SourcePath);
	FCortexCommandResult CreateResult = Router.Execute(TEXT("level.create_level"), CreateParams);
	if (!CreateResult.bSuccess)
	{
		AddInfo(TEXT("Could not create source level - skipping"));
		return true;
	}

	// Duplicate it. Earlier tests may leave the shared editor world dirty; force=true
	// explicitly accepts discarding that state instead of tripping the unsaved-changes guard.
	TSharedPtr<FJsonObject> DupParams = MakeShared<FJsonObject>();
	DupParams->SetStringField(TEXT("source_path"), SourcePath);
	DupParams->SetStringField(TEXT("dest_path"), DestPath);
	DupParams->SetBoolField(TEXT("force"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("level.duplicate_level"), DupParams);
	TestTrue(TEXT("duplicate_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("Path should match dest"), Result.Data->GetStringField(TEXT("path")), DestPath);
	}

	// Cleanup
	const FString SourceFile = FPackageName::LongPackageNameToFilename(SourcePath, FPackageName::GetMapPackageExtension());
	const FString DestFile = FPackageName::LongPackageNameToFilename(DestPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*SourceFile, false, true);
	IFileManager::Get().Delete(*DestFile, false, true);
	// Clean in-memory packages
	UPackage* SrcPkg = FindPackage(nullptr, *SourcePath);
	if (SrcPkg)
	{
		SrcPkg->MarkAsGarbage();
	}
	UPackage* DstPkg = FindPackage(nullptr, *DestPath);
	if (DstPkg)
	{
		DstPkg->MarkAsGarbage();
	}
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

	return true;
}

// duplicate_level swaps the editor world and reloads the current map from disk, so a dirty
// current map must be refused rather than silently discarded (issue #217).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDuplicateLevelUnsavedChangesTest,
	"Cortex.Level.Lifecycle.DuplicateLevel.UnsavedChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDuplicateLevelUnsavedChangesTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("Editor world is required"), World))
	{
		return false;
	}

	UPackage* WorldPackage = World->GetOutermost();
	if (!FPackageName::DoesPackageExist(WorldPackage->GetName()))
	{
		AddInfo(TEXT("Current editor level has no map file - skipping"));
		return true;
	}

	const bool bWasDirty = WorldPackage->IsDirty();
	WorldPackage->MarkPackageDirty();

	FCortexCommandRouter Router = CreateLifecycleRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("level.duplicate_level"), MakeDuplicateRefusalParams());

	TestFalse(TEXT("Dirty current level must refuse duplication"), Result.bSuccess);
	TestEqual(TEXT("Error should be UNSAVED_CHANGES"), Result.ErrorCode, FString(TEXT("UNSAVED_CHANGES")));
	TestTrue(TEXT("Error details report is_dirty"),
		Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("is_dirty")));
	const bool bWorldKept = GEditor->GetEditorWorldContext().World() == World;
	TestTrue(TEXT("Editor world must not be replaced"), bWorldKept);
	TestTrue(TEXT("Unsaved edits must be kept"), bWorldKept && WorldPackage->IsDirty());
	TestFalse(TEXT("Destination must not be written"),
		FPackageName::DoesPackageExist(DuplicateRefusalDest));

	if (bWorldKept)
	{
		WorldPackage->SetDirtyFlag(bWasDirty);
	}
	DeleteDuplicateRefusalDest();
	return true;
}

// One-file-per-actor edits dirty only the actor's external package, not the map package;
// they are discarded by the map reload just the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDuplicateLevelUnsavedExternalActorTest,
	"Cortex.Level.Lifecycle.DuplicateLevel.UnsavedExternalActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDuplicateLevelUnsavedExternalActorTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("Editor world is required"), World))
	{
		return false;
	}

	UPackage* WorldPackage = World->GetOutermost();
	if (!FPackageName::DoesPackageExist(WorldPackage->GetName()))
	{
		AddInfo(TEXT("Current editor level has no map file - skipping"));
		return true;
	}

	const bool bWasDirty = WorldPackage->IsDirty();
	// Weak handles: a regressed duplicate_level reloads the map and garbage-collects the fixture.
	TWeakObjectPtr<AActor> Actor = World->SpawnActor<AActor>();
	if (!TestTrue(TEXT("Fixture actor spawned"), Actor.IsValid()))
	{
		return false;
	}
	Actor->SetPackageExternal(true, true);
	TWeakObjectPtr<UPackage> ExternalPackage = Actor->GetExternalPackage();
	TestTrue(TEXT("Fixture actor has a dirty external package"), ExternalPackage.IsValid() && ExternalPackage->IsDirty());
	WorldPackage->SetDirtyFlag(false);

	FCortexCommandRouter Router = CreateLifecycleRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("level.duplicate_level"), MakeDuplicateRefusalParams());

	TestFalse(TEXT("Dirty external actor package must refuse duplication"), Result.bSuccess);
	TestEqual(TEXT("Error should be UNSAVED_CHANGES"), Result.ErrorCode, FString(TEXT("UNSAVED_CHANGES")));
	const bool bWorldKept = GEditor->GetEditorWorldContext().World() == World;
	TestTrue(TEXT("Editor world must not be replaced"), bWorldKept);

	if (bWorldKept && Actor.IsValid())
	{
		Actor->SetPackageExternal(false, false);
		World->DestroyActor(Actor.Get());
		WorldPackage->SetDirtyFlag(bWasDirty);
	}
	if (ExternalPackage.IsValid())
	{
		ExternalPackage->SetDirtyFlag(false);
		ExternalPackage->MarkAsGarbage();
	}
	DeleteDuplicateRefusalDest();
	return true;
}

// An Untitled current level has no file to reload after duplication, so the editor would be
// left on the duplicate with the Untitled level lost; refuse regardless of save_current/force.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDuplicateLevelUntitledCurrentTest,
	"Cortex.Level.Lifecycle.DuplicateLevel.UntitledCurrentLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDuplicateLevelUntitledCurrentTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	UWorld* OriginalWorld = GEditor->GetEditorWorldContext().World();
	const FString OriginalPath = OriginalWorld ? OriginalWorld->GetOutermost()->GetName() : FString();
	if (!FPackageName::DoesPackageExist(OriginalPath))
	{
		AddInfo(TEXT("Current editor level has no map file to restore - skipping"));
		return true;
	}

	UWorld* UntitledWorld = UEditorLoadingAndSavingUtils::NewBlankMap(false);
	if (!TestNotNull(TEXT("Untitled fixture world created"), UntitledWorld))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeDuplicateRefusalParams();
	Params->SetBoolField(TEXT("force"), true);
	FCortexCommandRouter Router = CreateLifecycleRouter();
	const FCortexCommandResult Result = Router.Execute(TEXT("level.duplicate_level"), Params);

	TestFalse(TEXT("Untitled current level must refuse duplication"), Result.bSuccess);
	TestEqual(TEXT("Error should be LEVEL_NOT_SAVED"), Result.ErrorCode, FString(TEXT("LEVEL_NOT_SAVED")));
	TestTrue(TEXT("Untitled editor world must not be replaced"), GEditor->GetEditorWorldContext().World() == UntitledWorld);
	TestFalse(TEXT("Destination must not be written"),
		FPackageName::DoesPackageExist(DuplicateRefusalDest));

	const FString OriginalFile = FPackageName::LongPackageNameToFilename(OriginalPath, FPackageName::GetMapPackageExtension());
	TestNotNull(TEXT("Original level restored"), UEditorLoadingAndSavingUtils::LoadMap(OriginalFile));
	DeleteDuplicateRefusalDest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelRenameLevelTest,
	"Cortex.Level.Lifecycle.RenameLevel.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRenameLevelTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	// Frame-separated fixture backed by project Saved: unique per run so filesystem and
	// registry notifications never collide with neighboring lifecycle tests.
	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TestRoot = TEXT("/CortexLevelRename_") + FixtureId + TEXT("/");
	TSharedPtr<FScopedLifecycleTestMount> Mount = MakeShared<FScopedLifecycleTestMount>(
		TestRoot, FPaths::ProjectSavedDir() / TEXT("QA/PR142") / FixtureId + TEXT("/"), true);
	if (!TestTrue(TEXT("Test-owned writable mount is available"), Mount->IsValid()))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FLevelMutationFixtureCommand(this, Mount, TestRoot, FLevelMutationFixtureCommand::EOperation::Rename));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelRenameLevelInUseTest,
	"Cortex.Level.Lifecycle.RenameLevel.InUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRenameLevelInUseTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Try to rename the currently open level
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddInfo(TEXT("No editor world - skipping"));
		return true;
	}

	const FString CurrentPath = World->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), CurrentPath);
	Params->SetStringField(TEXT("new_path"), CurrentPath + TEXT("_Renamed"));

	FCortexCommandResult Result = Router.Execute(TEXT("level.rename_level"), Params);
	TestFalse(TEXT("Should fail for currently open level"), Result.bSuccess);
	TestEqual(TEXT("Error should be LEVEL_IN_USE"), Result.ErrorCode, TEXT("LEVEL_IN_USE"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDeleteLevelTest,
	"Cortex.Level.Lifecycle.DeleteLevel.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDeleteLevelTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	// Frame-separated fixture backed by project Saved: unique per run so filesystem and
	// registry notifications never collide with neighboring lifecycle tests.
	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TestRoot = TEXT("/CortexLevelDelete_") + FixtureId + TEXT("/");
	TSharedPtr<FScopedLifecycleTestMount> Mount = MakeShared<FScopedLifecycleTestMount>(
		TestRoot, FPaths::ProjectSavedDir() / TEXT("QA/PR142") / FixtureId + TEXT("/"), true);
	if (!TestTrue(TEXT("Test-owned writable mount is available"), Mount->IsValid()))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FLevelMutationFixtureCommand(this, Mount, TestRoot, FLevelMutationFixtureCommand::EOperation::Delete));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelDeleteLevelInUseTest,
	"Cortex.Level.Lifecycle.DeleteLevel.InUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDeleteLevelInUseTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Try to delete the currently open level
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddInfo(TEXT("No editor world - skipping"));
		return true;
	}

	const FString CurrentPath = World->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), CurrentPath);

	FCortexCommandResult Result = Router.Execute(TEXT("level.delete_level"), Params);
	TestFalse(TEXT("Should fail for currently open level"), Result.bSuccess);
	TestEqual(TEXT("Error should be LEVEL_IN_USE"), Result.ErrorCode, TEXT("LEVEL_IN_USE"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelOpenLevelDirtyTest,
	"Cortex.Level.Lifecycle.OpenLevel.UnsavedChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelOpenLevelDirtyTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddInfo(TEXT("No editor world - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Create a target level to try to open
	const FString TargetPath = TEXT("/Game/Maps/_CortexTest/TestDirtyTarget");
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("path"), TargetPath);
	FCortexCommandResult CreateResult = Router.Execute(TEXT("level.create_level"), CreateParams);
	if (!CreateResult.bSuccess)
	{
		AddInfo(TEXT("Could not create target level - skipping"));
		return true;
	}

	// Mark current world as dirty
	World->GetOutermost()->MarkPackageDirty();

	// Try to open without save_current or force — should fail with UnsavedChanges
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("path"), TargetPath);

	FCortexCommandResult OpenResult = Router.Execute(TEXT("level.open_level"), OpenParams);
	TestFalse(TEXT("Should fail when current level is dirty"), OpenResult.bSuccess);
	TestEqual(TEXT("Error should be UNSAVED_CHANGES"), OpenResult.ErrorCode, TEXT("UNSAVED_CHANGES"));

	// Cleanup
	const FString TargetFile = FPackageName::LongPackageNameToFilename(TargetPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*TargetFile, false, true);
	UPackage* Pkg = FindPackage(nullptr, *TargetPath);
	if (Pkg)
	{
		Pkg->MarkAsGarbage();
	}
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

	return true;
}

// create_level (open=false) builds its world in memory and saves it. That in-memory world must be an
// asset (UObject::IsAsset needs RF_Public), or the asset operations that follow in the same session -
// rename_level, for one - reject it as "not a valid asset" even though the file on disk is fine.
// Covers both branches that construct the world: a blank world and a duplicated template.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelWorldIsAssetTest,
	"Cortex.Level.Lifecycle.CreateLevel.WorldIsAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelWorldIsAssetTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEditor is required"), GEditor))
	{
		return false;
	}

	// Frame-separated operations keep filesystem notifications ordered with editor deletion.
	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TestRoot = TEXT("/CortexWorldAsset_") + FixtureId + TEXT("/");
	TSharedPtr<FScopedLifecycleTestMount> Mount = MakeShared<FScopedLifecycleTestMount>(
		TestRoot, FPaths::ProjectSavedDir() / TEXT("QA/PR145") / FixtureId + TEXT("/"), true);
	if (!TestTrue(TEXT("Test-owned writable mount is available"), Mount->IsValid()))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FWorldAssetLifecycleCommand(this, Mount, TestRoot));
	return true;
}
