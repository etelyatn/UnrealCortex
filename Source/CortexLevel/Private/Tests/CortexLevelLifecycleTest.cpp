#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	FCortexCommandRouter CreateLifecycleRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
			MakeShared<FCortexLevelCommandHandler>());
		return Router;
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
	FCortexLevelDuplicateLevelTest,
	"Cortex.Level.Lifecycle.DuplicateLevel",
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

	// Duplicate it
	TSharedPtr<FJsonObject> DupParams = MakeShared<FJsonObject>();
	DupParams->SetStringField(TEXT("source_path"), SourcePath);
	DupParams->SetStringField(TEXT("dest_path"), DestPath);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelRenameLevelTest,
	"Cortex.Level.Lifecycle.RenameLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRenameLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	const FString OriginalPath = TEXT("/Game/Maps/_CortexTest/TestRenameOriginal");
	const FString NewPath = TEXT("/Game/Maps/_CortexTest/TestRenameNew");

	// Create level to rename
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("path"), OriginalPath);
	FCortexCommandResult CreateResult = Router.Execute(TEXT("level.create_level"), CreateParams);
	if (!CreateResult.bSuccess)
	{
		AddInfo(TEXT("Could not create test level - skipping"));
		return true;
	}

	// Rename it
	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("path"), OriginalPath);
	RenameParams->SetStringField(TEXT("new_path"), NewPath);

	FCortexCommandResult Result = Router.Execute(TEXT("level.rename_level"), RenameParams);
	TestTrue(TEXT("rename_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("old_path should match"), Result.Data->GetStringField(TEXT("old_path")), OriginalPath);
		TestEqual(TEXT("new_path should match"), Result.Data->GetStringField(TEXT("new_path")), NewPath);
	}

	// Cleanup
	const FString NewFile = FPackageName::LongPackageNameToFilename(NewPath, FPackageName::GetMapPackageExtension());
	const FString OrigFile = FPackageName::LongPackageNameToFilename(OriginalPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*NewFile, false, true);
	IFileManager::Get().Delete(*OrigFile, false, true);
	// Clean in-memory packages (rename moves from OriginalPath to NewPath)
	UPackage* NewPkg = FindPackage(nullptr, *NewPath);
	if (NewPkg)
	{
		NewPkg->MarkAsGarbage();
	}
	UPackage* OrigPkg = FindPackage(nullptr, *OriginalPath);
	if (OrigPkg)
	{
		OrigPkg->MarkAsGarbage();
	}
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

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
	"Cortex.Level.Lifecycle.DeleteLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDeleteLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Create a level to delete
	const FString TestPath = TEXT("/Game/Maps/_CortexTest/TestDeleteLevel");

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("path"), TestPath);
	FCortexCommandResult CreateResult = Router.Execute(TEXT("level.create_level"), CreateParams);
	if (!CreateResult.bSuccess)
	{
		AddInfo(TEXT("Could not create test level - skipping"));
		return true;
	}

	// Delete it
	TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
	DeleteParams->SetStringField(TEXT("path"), TestPath);

	FCortexCommandResult Result = Router.Execute(TEXT("level.delete_level"), DeleteParams);
	TestTrue(TEXT("delete_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("deleted_path should match"), Result.Data->GetStringField(TEXT("deleted_path")), TestPath);
	}

	// Verify it's actually gone
	const FString FilePath = FPackageName::LongPackageNameToFilename(TestPath, FPackageName::GetMapPackageExtension());
	TestFalse(TEXT("File should be deleted"), IFileManager::Get().FileExists(*FilePath));

	// Clean in-memory package if still present (ForceDeleteObjects may have already removed it)
	UPackage* Pkg = FindPackage(nullptr, *TestPath);
	if (Pkg)
	{
		Pkg->MarkAsGarbage();
	}

	// Cleanup directory
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

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
