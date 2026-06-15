#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Session/CortexCodexAppServerWorker.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerWorkerQueuesInitializeAndThreadStartTest,
    "Cortex.Frontend.CodexAppServer.Worker.QueuesInitializeAndThreadStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerWorkerQueuesTurnsOnExistingThreadTest,
    "Cortex.Frontend.CodexAppServer.Worker.QueuesTurnsOnExistingThread",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerWorkerInterruptRequiresTurnIdTest,
    "Cortex.Frontend.CodexAppServer.Worker.InterruptRequiresTurnId",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerWorkerInterruptWritesThreadAndTurnTest,
    "Cortex.Frontend.CodexAppServer.Worker.InterruptWritesThreadAndTurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodexAppServerWorkerQueuesInitializeAndThreadStartTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("codex-app-server-test");
    Config.ProviderId = FName(TEXT("codex"));
    Config.WorkingDirectory = TEXT("D:/UnrealProjects/CortexSandboxMirror");
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderDisplayName = TEXT("Codex");
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.LifetimePolicy = ECortexSessionLifetimePolicy::Persistent;

    TArray<FString> CapturedWrites;
    FCortexCodexAppServerWorker::SetWriteOverrideForTests(
        [&CapturedWrites](const FString& Line)
        {
            CapturedWrites.Add(Line);
            return true;
        });
    ON_SCOPE_EXIT
    {
        FCortexCodexAppServerWorker::ClearWriteOverrideForTests();
    };

    FCortexCodexAppServerWorker Worker(Config, ECortexAccessMode::Guided);
    Worker.StartForTests();

    TestTrue(TEXT("Worker should send initialize request"),
        CapturedWrites.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("\"method\":\"initialize\"")); }));
    TestTrue(TEXT("Worker should send initialized notification"),
        CapturedWrites.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("\"method\":\"initialized\"")); }));
    TestTrue(TEXT("Worker should send thread/start request"),
        CapturedWrites.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("\"method\":\"thread/start\"")); }));

    return true;
}

bool FCortexCodexAppServerWorkerQueuesTurnsOnExistingThreadTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("codex-app-server-test");
    Config.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderDisplayName = TEXT("Codex");
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.LifetimePolicy = ECortexSessionLifetimePolicy::Persistent;

    TArray<FString> CapturedWrites;
    FCortexCodexAppServerWorker::SetWriteOverrideForTests(
        [&CapturedWrites](const FString& Line)
        {
            CapturedWrites.Add(Line);
            return true;
        });
    ON_SCOPE_EXIT
    {
        FCortexCodexAppServerWorker::ClearWriteOverrideForTests();
    };

    FCortexCodexAppServerWorker Worker(Config, ECortexAccessMode::Guided);
    Worker.SetThreadIdForTests(TEXT("thread-live"));
    TestTrue(TEXT("Turn should queue when thread is ready"),
        Worker.SendTurn(TEXT("Second chat prompt"), ECortexAccessMode::Guided));

    TestEqual(TEXT("Only one write should be needed for an existing thread turn"),
        CapturedWrites.Num(),
        1);
    TestTrue(TEXT("Queued write should be turn/start"),
        CapturedWrites[0].Contains(TEXT("\"method\":\"turn/start\"")));
    TestTrue(TEXT("Queued write should target the existing thread id"),
        CapturedWrites[0].Contains(TEXT("\"threadId\":\"thread-live\"")));

    return true;
}

bool FCortexCodexAppServerWorkerInterruptRequiresTurnIdTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.LifetimePolicy = ECortexSessionLifetimePolicy::Persistent;

    FCortexCodexAppServerWorker Worker(Config, ECortexAccessMode::Guided);
    Worker.SetThreadIdForTests(TEXT("thread-ready"));

    TestFalse(TEXT("Interrupt should fail without an active turn id"),
        Worker.InterruptTurn());
    return true;
}

bool FCortexCodexAppServerWorkerInterruptWritesThreadAndTurnTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.LifetimePolicy = ECortexSessionLifetimePolicy::Persistent;

    TArray<FString> CapturedWrites;
    FCortexCodexAppServerWorker::SetWriteOverrideForTests(
        [&CapturedWrites](const FString& Line)
        {
            CapturedWrites.Add(Line);
            return true;
        });
    ON_SCOPE_EXIT
    {
        FCortexCodexAppServerWorker::ClearWriteOverrideForTests();
    };

    FCortexCodexAppServerWorker Worker(Config, ECortexAccessMode::Guided);
    Worker.SetThreadIdForTests(TEXT("thread-ready"));
    Worker.SetActiveTurnIdForTests(TEXT("turn-live"));

    TestTrue(TEXT("Interrupt should queue when thread and turn are ready"),
        Worker.InterruptTurn());
    TestEqual(TEXT("Interrupt should emit exactly one write"),
        CapturedWrites.Num(),
        1);
    TestTrue(TEXT("Interrupt write should include thread id"),
        CapturedWrites[0].Contains(TEXT("\"threadId\":\"thread-ready\"")));
    TestTrue(TEXT("Interrupt write should include turn id"),
        CapturedWrites[0].Contains(TEXT("\"turnId\":\"turn-live\"")));
    return true;
}
