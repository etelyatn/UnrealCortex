// Source/CortexFrontend/Private/Tests/CortexQASessionManagerTest.cpp
#include "Misc/AutomationTest.h"
#include "QA/CortexQASessionManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"

namespace
{
    FString GetTestManagerDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("CortexQA") / TEXT("TestManagerSessions");
    }

    void CleanupManagerTestDir()
    {
        IFileManager::Get().DeleteDirectory(*GetTestManagerDir(), false, true);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerLoadEmptyTest,
    "Cortex.Frontend.QASessionManager.LoadEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerLoadEmptyTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();

    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();

    TestEqual(TEXT("Empty directory should have 0 sessions"), Manager.GetSessions().Num(), 0);

    CleanupManagerTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerDeleteTest,
    "Cortex.Frontend.QASessionManager.Delete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerDeleteTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();

    // Create a test file manually
    IFileManager::Get().MakeDirectory(*GetTestManagerDir(), true);
    const FString TestFile = GetTestManagerDir() / TEXT("2026-03-16_test.json");
    FFileHelper::SaveStringToFile(
        TEXT("{\"version\":1,\"name\":\"test\",\"source\":\"recorded\",\"recorded_at\":\"2026-03-16T00:00:00Z\",\"map\":\"/Game/Maps/Test\",\"duration_seconds\":1,\"complete\":true,\"steps\":[],\"raw_input\":[],\"conversation_history\":[]}"),
        *TestFile);

    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();
    TestEqual(TEXT("Should have 1 session"), Manager.GetSessions().Num(), 1);

    Manager.DeleteSession(0);
    TestEqual(TEXT("Should have 0 sessions after delete"), Manager.GetSessions().Num(), 0);
    TestFalse(TEXT("File should be deleted"), FPaths::FileExists(TestFile));

    CleanupManagerTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerDeleteInvalidIndexTest,
    "Cortex.Frontend.QASessionManager.DeleteInvalidIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerDeleteInvalidIndexTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();
    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();

    TestFalse(TEXT("Delete at -1 should fail"), Manager.DeleteSession(-1));
    TestFalse(TEXT("Delete at 999 should fail"), Manager.DeleteSession(999));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerSkipCorruptTest,
    "Cortex.Frontend.QASessionManager.SkipCorruptFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerSkipCorruptTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();
    IFileManager::Get().MakeDirectory(*GetTestManagerDir(), true);

    // One valid JSON
    FFileHelper::SaveStringToFile(
        TEXT("{\"version\":1,\"name\":\"valid\",\"source\":\"recorded\",\"recorded_at\":\"2026-03-16T00:00:00Z\",\"map\":\"/Game/Maps/Test\",\"duration_seconds\":1,\"complete\":true,\"steps\":[],\"raw_input\":[],\"conversation_history\":[]}"),
        *(GetTestManagerDir() / TEXT("valid.json")));

    // One corrupt JSON
    FFileHelper::SaveStringToFile(
        TEXT("{broken json"),
        *(GetTestManagerDir() / TEXT("corrupt.json")));

    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();

    TestEqual(TEXT("Should load only the valid session"), Manager.GetSessions().Num(), 1);
    TestEqual(TEXT("Valid session name should match"), Manager.GetSessions()[0].Name, TEXT("valid"));

    CleanupManagerTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerLoadPopulatedTest,
    "Cortex.Frontend.QASessionManager.LoadPopulated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerLoadPopulatedTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();
    IFileManager::Get().MakeDirectory(*GetTestManagerDir(), true);

    // Create 3 sessions with different dates for sort verification
    auto MakeSession = [](const FString& Name, const FString& Date) -> FString
    {
        return FString::Printf(
            TEXT("{\"version\":1,\"name\":\"%s\",\"source\":\"recorded\",\"recorded_at\":\"%sT00:00:00Z\",\"map\":\"/Game/Maps/Test\",\"duration_seconds\":1,\"complete\":true,\"steps\":[{\"type\":\"move_to\",\"timestamp_ms\":0,\"params\":{}}],\"raw_input\":[],\"conversation_history\":[]}"),
            *Name, *Date);
    };

    FFileHelper::SaveStringToFile(MakeSession(TEXT("Old"), TEXT("2026-03-14")),
        *(GetTestManagerDir() / TEXT("old.json")));
    FFileHelper::SaveStringToFile(MakeSession(TEXT("Middle"), TEXT("2026-03-15")),
        *(GetTestManagerDir() / TEXT("middle.json")));
    FFileHelper::SaveStringToFile(MakeSession(TEXT("Newest"), TEXT("2026-03-16")),
        *(GetTestManagerDir() / TEXT("newest.json")));

    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();

    TestEqual(TEXT("Should load 3 sessions"), Manager.GetSessions().Num(), 3);
    TestEqual(TEXT("First session should be newest (sorted desc)"), Manager.GetSessions()[0].Name, TEXT("Newest"));
    TestEqual(TEXT("Each session should have 1 step"), Manager.GetSessions()[0].StepCount, 1);

    CleanupManagerTestDir();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASessionManagerFieldsTest,
    "Cortex.Frontend.QASessionManager.SessionListItemFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQASessionManagerFieldsTest::RunTest(const FString& Parameters)
{
    CleanupManagerTestDir();
    IFileManager::Get().MakeDirectory(*GetTestManagerDir(), true);

    FFileHelper::SaveStringToFile(
        TEXT("{\"version\":1,\"name\":\"FieldTest\",\"source\":\"ai_generated\",\"recorded_at\":\"2026-03-16T10:30:00Z\",\"map\":\"/Game/Maps/Clinic\",\"duration_seconds\":42,\"complete\":true,\"steps\":[{\"type\":\"move_to\",\"timestamp_ms\":0,\"params\":{}},{\"type\":\"assert\",\"timestamp_ms\":100,\"params\":{}}],\"raw_input\":[],\"conversation_history\":[],\"last_run\":{\"timestamp\":\"2026-03-16T11:00:00Z\",\"passed\":false,\"steps_passed\":1,\"steps_failed\":1,\"duration_seconds\":5}}"),
        *(GetTestManagerDir() / TEXT("field_test.json")));

    FCortexQASessionManager Manager(GetTestManagerDir());
    Manager.RefreshSessionList();

    TestEqual(TEXT("Should have 1 session"), Manager.GetSessions().Num(), 1);
    const FCortexQASessionListItem& Item = Manager.GetSessions()[0];
    TestEqual(TEXT("Name"), Item.Name, TEXT("FieldTest"));
    TestEqual(TEXT("Source"), Item.Source, TEXT("ai_generated"));
    TestEqual(TEXT("Map"), Item.MapPath, TEXT("/Game/Maps/Clinic"));
    TestEqual(TEXT("StepCount"), Item.StepCount, 2);
    TestTrue(TEXT("bComplete"), Item.bComplete);
    TestTrue(TEXT("bHasBeenRun"), Item.bHasBeenRun);
    TestFalse(TEXT("bLastRunPassed should be false"), Item.bLastRunPassed);

    CleanupManagerTestDir();
    return true;
}
