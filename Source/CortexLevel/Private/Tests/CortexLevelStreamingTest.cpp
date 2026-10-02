#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
    bool TryGetCurrentLevelFilename(FCortexCommandRouter& Router, FString& OutFilename)
    {
        const FCortexCommandResult InfoResult = Router.Execute(TEXT("level.get_info"), MakeShared<FJsonObject>());
        if (!InfoResult.bSuccess || !InfoResult.Data.IsValid())
        {
            return false;
        }

        FString LevelPath;
        if (!InfoResult.Data->TryGetStringField(TEXT("level_path"), LevelPath) || LevelPath.IsEmpty())
        {
            return false;
        }

        OutFilename = FPackageName::LongPackageNameToFilename(LevelPath, FPackageName::GetMapPackageExtension());
        return !OutFilename.IsEmpty();
    }

    bool CanOpenForExclusiveWrite(const FString& Filename)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Filename, true, false));
        return Handle.IsValid();
    }

    FCortexCommandRouter CreateLevelRouterStreaming()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetInfoTest,
    "Cortex.Level.Streaming.GetInfo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetInfoTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.get_info"), MakeShared<FJsonObject>());
    TestTrue(TEXT("get_info should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("level_name should exist"), Result.Data->HasField(TEXT("level_name")));
        TestTrue(TEXT("level_path should exist"), Result.Data->HasField(TEXT("level_path")));
        TestTrue(TEXT("world_type should exist"), Result.Data->HasField(TEXT("world_type")));
        TestTrue(TEXT("actor_count should exist"), Result.Data->HasField(TEXT("actor_count")));
        TestTrue(TEXT("is_world_partition should exist"), Result.Data->HasField(TEXT("is_world_partition")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListSublevelsTest,
    "Cortex.Level.Streaming.ListSublevels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListSublevelsTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.list_sublevels"), MakeShared<FJsonObject>());
    TestTrue(TEXT("list_sublevels should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("sublevels array should exist"), Result.Data->HasField(TEXT("sublevels")));
        TestTrue(TEXT("count should exist"), Result.Data->HasField(TEXT("count")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveLevelTest,
    "Cortex.Level.Streaming.SaveLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveLevelTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Editor is available"), GEditor))
    {
        return false;
    }

    const FString PackagePath = TEXT("/Game/Temp/CortexLevelSave_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Map");
    const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetMapPackageExtension());
    UPackage* Package = CreatePackage(*PackagePath);
    UWorld* TestWorld = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("Map"), Package);
    if (!TestNotNull(TEXT("Saved-map fixture created"), TestWorld))
    {
        Package->MarkAsGarbage();
        return false;
    }

    UWorld* OriginalWorld = GEditor->GetEditorWorldContext().World();
    ON_SCOPE_EXIT
    {
        GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalWorld);
        Package->SetDirtyFlag(false);
        TestWorld->DestroyWorld(false);
        TestWorld->MarkAsGarbage();
        Package->MarkAsGarbage();
        IFileManager::Get().Delete(*Filename, false, true);
        IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Filename), false, false);
    };

    TestWorld->SetFlags(RF_Public | RF_Standalone);
    if (!TestFalse(TEXT("Fixture package contains a saveable map asset"), UPackage::IsEmptyPackage(Package)))
    {
        return false;
    }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Standalone;
    if (!TestTrue(TEXT("Fixture has an existing map file"), UPackage::SavePackage(Package, TestWorld, *Filename, SaveArgs)))
    {
        return false;
    }

    GEditor->GetEditorWorldContext().SetCurrentWorld(TestWorld);
    Package->SetDirtyFlag(true);
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    FCortexCommandResult Result = Router.Execute(TEXT("level.save_level"), MakeShared<FJsonObject>());
    TestTrue(TEXT("save_level should succeed"), Result.bSuccess);
    TestFalse(TEXT("Saved package is no longer dirty"), Package->IsDirty());
    TestTrue(TEXT("Map file remains nonempty"), IFileManager::Get().FileSize(*Filename) > 0);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        bool bSaved = false;
        Result.Data->TryGetBoolField(TEXT("saved"), bSaved);
        TestTrue(TEXT("saved should be true"), bSaved);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveAllTest,
    "Cortex.Level.Streaming.SaveAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveAllTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterStreaming();

    FString LevelFilename;
    if (TryGetCurrentLevelFilename(Router, LevelFilename) && !CanOpenForExclusiveWrite(LevelFilename))
    {
        AddInfo(FString::Printf(TEXT("Skipping save_all: map file is locked by another process (%s)"), *LevelFilename));
        return true;
    }

    FCortexCommandResult Result = Router.Execute(TEXT("level.save_all"), MakeShared<FJsonObject>());
    TestTrue(TEXT("save_all should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("saved field should exist"), Result.Data->HasField(TEXT("saved")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveNeverSavedTest,
    "Cortex.Level.Streaming.SaveNeverSavedLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveNeverSavedTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("Editor is available"), GEditor))
    {
        return false;
    }

    UWorld* OriginalWorld = GEditor->GetEditorWorldContext().World();
    UWorld* TestWorld = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!TestNotNull(TEXT("Unsaved world created"), TestWorld))
    {
        return false;
    }

    GEditor->GetEditorWorldContext().SetCurrentWorld(TestWorld);
    TestWorld->GetOutermost()->SetDirtyFlag(true);
    TestTrue(TEXT("Fixture has no saved filename"), FEditorFileUtils::GetFilename(TestWorld).IsEmpty());

    // Keep a regression from blocking automation on the engine's Save As modal.
    // The unfixed implementation returns INVALID_OPERATION in this mode.
    TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
    FCortexCommandRouter Router = CreateLevelRouterStreaming();
    const FCortexCommandResult Result = Router.Execute(TEXT("level.save_level"), MakeShared<FJsonObject>());
    TestFalse(TEXT("Unsaved level is rejected"), Result.bSuccess);
    TestEqual(TEXT("Actionable error identifies a never-saved level"), Result.ErrorCode, TEXT("LEVEL_NOT_SAVED"));
    TestTrue(TEXT("Error explains how to save first"), Result.ErrorMessage.Contains(TEXT("create_level")));
    TestTrue(TEXT("Unsaved edits remain dirty"), TestWorld->GetOutermost()->IsDirty());
    TestTrue(TEXT("Follow-up command still runs"), Router.Execute(TEXT("level.get_info"), MakeShared<FJsonObject>()).bSuccess);

    GEditor->GetEditorWorldContext().SetCurrentWorld(OriginalWorld);
    TestWorld->GetOutermost()->SetDirtyFlag(false);
    TestWorld->DestroyWorld(false);
    TestWorld->MarkAsGarbage();
    return true;
}
