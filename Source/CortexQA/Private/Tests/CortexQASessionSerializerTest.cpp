// Source/CortexQA/Private/Tests/CortexQASessionSerializerTest.cpp
#include "Misc/AutomationTest.h"
#include "Recording/CortexQASessionTypes.h"
#include "Recording/CortexQASessionSerializer.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

namespace
{
    FCortexQASessionInfo CreateTestSession()
    {
        FCortexQASessionInfo Info;
        Info.Version = 1;
        Info.Name = TEXT("Test Session");
        Info.Source = TEXT("recorded");
        Info.RecordedAt = FDateTime(2026, 3, 16, 14, 30, 0);
        Info.MapPath = TEXT("/Game/Maps/TestMap");
        Info.DurationSeconds = 10.0;
        Info.bComplete = true;

        // Add a move_to step
        FCortexQAStep MoveStep;
        MoveStep.Type = TEXT("move_to");
        MoveStep.TimestampMs = 1000.0;
        MoveStep.Params = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> TargetArr;
        TargetArr.Add(MakeShared<FJsonValueNumber>(100.0));
        TargetArr.Add(MakeShared<FJsonValueNumber>(200.0));
        TargetArr.Add(MakeShared<FJsonValueNumber>(50.0));
        MoveStep.Params->SetArrayField(TEXT("target"), TargetArr);
        MoveStep.Params->SetNumberField(TEXT("tolerance"), 100.0);
        Info.Steps.Add(MoveStep);

        // Add an assert step
        FCortexQAStep AssertStep;
        AssertStep.Type = TEXT("assert");
        AssertStep.TimestampMs = 2000.0;
        AssertStep.Params = MakeShared<FJsonObject>();
        AssertStep.Params->SetStringField(TEXT("type"), TEXT("actor_property"));
        AssertStep.Params->SetStringField(TEXT("actor"), TEXT("BP_Door_01"));
        AssertStep.Params->SetStringField(TEXT("property"), TEXT("bIsOpen"));
        AssertStep.Params->SetBoolField(TEXT("value"), true);
        Info.Steps.Add(AssertStep);

        return Info;
    }

    FString GetTestDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("CortexQA") / TEXT("TestRecordings");
    }

    void CleanupTestDir()
    {
        IFileManager::Get().DeleteDirectory(*GetTestDir(), false, true);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerSaveTest,
    "Cortex.QA.SessionSerializer.SaveSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerSaveTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Session = CreateTestSession();

    FString OutPath;
    const bool bSaved = FCortexQASessionSerializer::SaveSession(Session, GetTestDir(), OutPath);

    TestTrue(TEXT("Save should succeed"), bSaved);
    TestTrue(TEXT("Output path should be non-empty"), !OutPath.IsEmpty());
    TestTrue(TEXT("File should exist on disk"), FPaths::FileExists(OutPath));

    // Verify JSON is valid
    FString JsonStr;
    FFileHelper::LoadFileToString(JsonStr, *OutPath);
    TestTrue(TEXT("File should have content"), !JsonStr.IsEmpty());

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerRoundTripTest,
    "Cortex.QA.SessionSerializer.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerRoundTripTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Original = CreateTestSession();

    FString SavedPath;
    FCortexQASessionSerializer::SaveSession(Original, GetTestDir(), SavedPath);

    FCortexQASessionInfo Loaded;
    const bool bLoaded = FCortexQASessionSerializer::LoadSession(SavedPath, Loaded);

    TestTrue(TEXT("Load should succeed"), bLoaded);
    TestEqual(TEXT("Version round-trips"), Loaded.Version, Original.Version);
    TestEqual(TEXT("Name round-trips"), Loaded.Name, Original.Name);
    TestEqual(TEXT("Source round-trips"), Loaded.Source, Original.Source);
    TestEqual(TEXT("Map round-trips"), Loaded.MapPath, Original.MapPath);
    TestTrue(TEXT("Duration round-trips"), FMath::IsNearlyEqual(Loaded.DurationSeconds, Original.DurationSeconds, 0.1));
    TestEqual(TEXT("Complete flag round-trips"), Loaded.bComplete, Original.bComplete);
    TestEqual(TEXT("Step count round-trips"), Loaded.Steps.Num(), Original.Steps.Num());
    TestEqual(TEXT("Step 0 type round-trips"), Loaded.Steps[0].Type, TEXT("move_to"));
    TestEqual(TEXT("Step 1 type round-trips"), Loaded.Steps[1].Type, TEXT("assert"));

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerLoadMissingTest,
    "Cortex.QA.SessionSerializer.LoadMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerLoadMissingTest::RunTest(const FString& Parameters)
{
    FCortexQASessionInfo Info;
    const bool bLoaded = FCortexQASessionSerializer::LoadSession(
        TEXT("C:/nonexistent/path.json"), Info);
    TestFalse(TEXT("Loading missing file should fail"), bLoaded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerListSessionsTest,
    "Cortex.QA.SessionSerializer.ListSessions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerListSessionsTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();

    // Save 2 sessions
    FCortexQASessionInfo S1 = CreateTestSession();
    S1.Name = TEXT("Session A");
    FString P1;
    FCortexQASessionSerializer::SaveSession(S1, GetTestDir(), P1);

    FCortexQASessionInfo S2 = CreateTestSession();
    S2.Name = TEXT("Session B");
    FString P2;
    FCortexQASessionSerializer::SaveSession(S2, GetTestDir(), P2);

    TArray<FCortexQASessionInfo> Sessions;
    FCortexQASessionSerializer::ListSessions(GetTestDir(), Sessions);

    TestEqual(TEXT("Should find 2 sessions"), Sessions.Num(), 2);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerLastRunTest,
    "Cortex.QA.SessionSerializer.LastRunRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerLastRunTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Session = CreateTestSession();
    Session.LastRun = FCortexQALastRun();
    Session.LastRun->bPassed = true;
    Session.LastRun->StepsPassed = 2;
    Session.LastRun->StepsFailed = 0;
    Session.LastRun->DurationSeconds = 5.0;

    FString Path;
    FCortexQASessionSerializer::SaveSession(Session, GetTestDir(), Path);

    FCortexQASessionInfo Loaded;
    FCortexQASessionSerializer::LoadSession(Path, Loaded);

    TestTrue(TEXT("LastRun should be set"), Loaded.LastRun.IsSet());
    TestTrue(TEXT("LastRun passed should round-trip"), Loaded.LastRun->bPassed);
    TestEqual(TEXT("LastRun steps_passed round-trips"), Loaded.LastRun->StepsPassed, 2);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerRecordedAtRoundTripTest,
    "Cortex.QA.SessionSerializer.RecordedAtRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerRecordedAtRoundTripTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Original = CreateTestSession();
    Original.RecordedAt = FDateTime(2026, 3, 16, 14, 30, 45);

    FString Path;
    FCortexQASessionSerializer::SaveSession(Original, GetTestDir(), Path);

    FCortexQASessionInfo Loaded;
    FCortexQASessionSerializer::LoadSession(Path, Loaded);

    // ISO 8601 round-trip should preserve at least minute-level precision
    TestEqual(TEXT("Year round-trips"), Loaded.RecordedAt.GetYear(), 2026);
    TestEqual(TEXT("Month round-trips"), Loaded.RecordedAt.GetMonth(), 3);
    TestEqual(TEXT("Day round-trips"), Loaded.RecordedAt.GetDay(), 16);
    TestEqual(TEXT("Hour round-trips"), Loaded.RecordedAt.GetHour(), 14);
    TestEqual(TEXT("Minute round-trips"), Loaded.RecordedAt.GetMinute(), 30);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerLoadMalformedJsonTest,
    "Cortex.QA.SessionSerializer.LoadMalformedJson",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerLoadMalformedJsonTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    IFileManager::Get().MakeDirectory(*GetTestDir(), true);
    const FString BadFile = GetTestDir() / TEXT("bad.json");
    FFileHelper::SaveStringToFile(TEXT("{broken json not valid"), *BadFile);

    FCortexQASessionInfo Info;
    const bool bLoaded = FCortexQASessionSerializer::LoadSession(BadFile, Info);
    TestFalse(TEXT("Malformed JSON should fail to load"), bLoaded);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerFilenameSuffixTest,
    "Cortex.QA.SessionSerializer.FilenameSuffixCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerFilenameSuffixTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo S1 = CreateTestSession();
    FCortexQASessionInfo S2 = CreateTestSession();

    FString P1, P2;
    FCortexQASessionSerializer::SaveSession(S1, GetTestDir(), P1);
    FCortexQASessionSerializer::SaveSession(S2, GetTestDir(), P2);

    TestTrue(TEXT("Both files should exist"), FPaths::FileExists(P1) && FPaths::FileExists(P2));
    TestFalse(TEXT("Paths should be different"), P1 == P2);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerUpdateLastRunTest,
    "Cortex.QA.SessionSerializer.UpdateLastRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerUpdateLastRunTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Session = CreateTestSession();

    FString Path;
    FCortexQASessionSerializer::SaveSession(Session, GetTestDir(), Path);

    FCortexQALastRun Run;
    Run.Timestamp = FDateTime::UtcNow();
    Run.bPassed = false;
    Run.StepsPassed = 1;
    Run.StepsFailed = 1;
    Run.DurationSeconds = 3.5;

    const bool bUpdated = FCortexQASessionSerializer::UpdateLastRun(Path, Run);
    TestTrue(TEXT("UpdateLastRun should succeed"), bUpdated);

    // Reload and verify (UpdateLastRun deletes + re-saves, path may change)
    TArray<FCortexQASessionInfo> All;
    FCortexQASessionSerializer::ListSessions(GetTestDir(), All);
    TestEqual(TEXT("Should still have 1 session"), All.Num(), 1);
    TestTrue(TEXT("LastRun should be set after update"), All[0].LastRun.IsSet());
    TestFalse(TEXT("LastRun passed should be false"), All[0].LastRun->bPassed);
    TestEqual(TEXT("LastRun steps_failed should be 1"), All[0].LastRun->StepsFailed, 1);

    CleanupTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASerializerDeleteSessionTest,
    "Cortex.QA.SessionSerializer.DeleteSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASerializerDeleteSessionTest::RunTest(const FString& Parameters)
{
    CleanupTestDir();
    FCortexQASessionInfo Session = CreateTestSession();
    FString Path;
    FCortexQASessionSerializer::SaveSession(Session, GetTestDir(), Path);
    TestTrue(TEXT("File should exist after save"), FPaths::FileExists(Path));

    const bool bDeleted = FCortexQASessionSerializer::DeleteSession(Path);
    TestTrue(TEXT("Delete should succeed"), bDeleted);
    TestFalse(TEXT("File should not exist after delete"), FPaths::FileExists(Path));

    CleanupTestDir();
    return true;
}
