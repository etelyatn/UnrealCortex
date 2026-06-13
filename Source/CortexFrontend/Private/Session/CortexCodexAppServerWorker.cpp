#include "Session/CortexCodexAppServerWorker.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Providers/CortexCodexCliProvider.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    TFunction<bool(const FString&)> GCodexAppServerWriteOverrideForTests;
}
#endif

FCortexCodexAppServerWorker::FCortexCodexAppServerWorker(
    const FCortexSessionConfig& InConfig,
    ECortexAccessMode InAccessMode)
    : Config(InConfig)
    , InitialAccessMode(InAccessMode)
{
}

FCortexCodexAppServerWorker::~FCortexCodexAppServerWorker()
{
    Shutdown();
}

bool FCortexCodexAppServerWorker::Start()
{
    if (Thread != nullptr)
    {
        return true;
    }

    if (!LaunchProcess())
    {
        return false;
    }

    QueueWrite(FCortexCodexAppServerProtocol::BuildInitializeRequest(NextRequestId()));
    QueueWrite(FCortexCodexAppServerProtocol::BuildInitializedNotification());
    QueueWrite(FCortexCodexAppServerProtocol::BuildThreadStartRequest(NextRequestId(), Config, InitialAccessMode));

    Thread = FRunnableThread::Create(this, TEXT("CortexCodexAppServerWorker"), 0, TPri_Normal);
    return Thread != nullptr;
}

bool FCortexCodexAppServerWorker::SendTurn(const FString& Prompt, ECortexAccessMode AccessMode)
{
    if (ProtocolState.ThreadId.IsEmpty())
    {
        return false;
    }

    return QueueWrite(FCortexCodexAppServerProtocol::BuildTurnStartRequest(
        NextRequestId(),
        ProtocolState.ThreadId,
        Prompt,
        Config,
        AccessMode));
}

bool FCortexCodexAppServerWorker::InterruptTurn()
{
    if (ProtocolState.ThreadId.IsEmpty())
    {
        return false;
    }

    return QueueWrite(FCortexCodexAppServerProtocol::BuildTurnInterruptRequest(
        NextRequestId(),
        ProtocolState.ThreadId));
}

void FCortexCodexAppServerWorker::Shutdown()
{
    Stop();

    if (Thread != nullptr)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }

    if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
    {
        const double WaitStart = FPlatformTime::Seconds();
        while (FPlatformProcess::IsProcRunning(ProcessHandle) && (FPlatformTime::Seconds() - WaitStart) < 0.25)
        {
            FPlatformProcess::Sleep(0.01f);
        }

        if (FPlatformProcess::IsProcRunning(ProcessHandle))
        {
            FPlatformProcess::TerminateProc(ProcessHandle, true);
        }
    }

    ClosePipes();

    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::CloseProc(ProcessHandle);
        ProcessHandle.Reset();
    }
}

bool FCortexCodexAppServerWorker::Init()
{
    return true;
}

uint32 FCortexCodexAppServerWorker::Run()
{
    while (!bStopRequested.load(std::memory_order_acquire))
    {
        FlushQueuedWrites();

        const FString Chunk = StdoutReadPipe != nullptr
            ? FPlatformProcess::ReadPipe(StdoutReadPipe)
            : FString();
        if (!Chunk.IsEmpty())
        {
            ReadBuffer += Chunk;

            int32 NewLineIndex = INDEX_NONE;
            while (ReadBuffer.FindChar(TEXT('\n'), NewLineIndex))
            {
                const FString Line = ReadBuffer.Left(NewLineIndex).TrimStartAndEnd();
                ReadBuffer = ReadBuffer.Mid(NewLineIndex + 1);
                if (!Line.IsEmpty())
                {
                    DispatchLine(Line);
                }
            }
        }

        if (ProcessHandle.IsValid() && !FPlatformProcess::IsProcRunning(ProcessHandle))
        {
            AsyncTask(ENamedThreads::GameThread, [Exited = OnExited]()
            {
                Exited.ExecuteIfBound(TEXT("Codex app-server process exited"));
            });
            break;
        }

        FPlatformProcess::Sleep(0.01f);
    }

    return 0;
}

void FCortexCodexAppServerWorker::Stop()
{
    bStopRequested.store(true, std::memory_order_release);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexCodexAppServerWorker::SetWriteOverrideForTests(TFunction<bool(const FString&)> InOverride)
{
    GCodexAppServerWriteOverrideForTests = MoveTemp(InOverride);
}

void FCortexCodexAppServerWorker::ClearWriteOverrideForTests()
{
    GCodexAppServerWriteOverrideForTests.Reset();
}

void FCortexCodexAppServerWorker::StartForTests()
{
    QueueWrite(FCortexCodexAppServerProtocol::BuildInitializeRequest(NextRequestId()));
    QueueWrite(FCortexCodexAppServerProtocol::BuildInitializedNotification());
    QueueWrite(FCortexCodexAppServerProtocol::BuildThreadStartRequest(NextRequestId(), Config, InitialAccessMode));
    FlushQueuedWrites();
}

void FCortexCodexAppServerWorker::SetThreadIdForTests(const FString& ThreadId)
{
    ProtocolState.ThreadId = ThreadId;
}
#endif

int32 FCortexCodexAppServerWorker::NextRequestId()
{
    return ++RequestCounter;
}

bool FCortexCodexAppServerWorker::QueueWrite(const FString& Line)
{
    {
        FScopeLock Lock(&WriteMutex);
        PendingWrites.Enqueue(Line);
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (GCodexAppServerWriteOverrideForTests)
    {
        return FlushQueuedWrites();
    }
#endif

    return true;
}

bool FCortexCodexAppServerWorker::FlushQueuedWrites()
{
    TArray<FString> Writes;
    {
        FScopeLock Lock(&WriteMutex);

        FString Line;
        while (PendingWrites.Dequeue(Line))
        {
            Writes.Add(MoveTemp(Line));
        }
    }

    for (const FString& Line : Writes)
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (GCodexAppServerWriteOverrideForTests)
        {
            if (!GCodexAppServerWriteOverrideForTests(Line))
            {
                return false;
            }
            continue;
        }
#endif

        if (StdinWritePipe == nullptr || !FPlatformProcess::WritePipe(StdinWritePipe, Line))
        {
            UE_LOG(LogCortexFrontend, Warning, TEXT("Failed to write to Codex app-server stdin"));
            return false;
        }
    }

    return true;
}

bool FCortexCodexAppServerWorker::LaunchProcess()
{
    FCortexCodexCliProvider Provider;
    const FCortexCliInfo CliInfo = Provider.FindCli();
    if (!CliInfo.bIsValid)
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("Codex CLI not available for app-server worker"));
        return false;
    }

    if (!FPlatformProcess::CreatePipe(StdoutReadPipe, StdoutWritePipe, false))
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create Codex app-server stdout pipe"));
        return false;
    }

    if (!FPlatformProcess::CreatePipe(StdinReadPipe, StdinWritePipe, true))
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create Codex app-server stdin pipe"));
        ClosePipes();
        return false;
    }

    const FString WorkingDirectory = !Config.WorkingDirectory.IsEmpty()
        ? Config.WorkingDirectory
        : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

    ProcessHandle = FPlatformProcess::CreateProc(
        *CliInfo.Path,
        TEXT("app-server --stdio"),
        false,
        true,
        false,
        nullptr,
        0,
        *WorkingDirectory,
        StdoutWritePipe,
        StdinReadPipe);

    if (!ProcessHandle.IsValid())
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Failed to launch Codex app-server process"));
        ClosePipes();
        return false;
    }

    return true;
}

void FCortexCodexAppServerWorker::ClosePipes()
{
    if (StdoutReadPipe != nullptr || StdoutWritePipe != nullptr)
    {
        FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
        StdoutReadPipe = nullptr;
        StdoutWritePipe = nullptr;
    }

    if (StdinReadPipe != nullptr || StdinWritePipe != nullptr)
    {
        FPlatformProcess::ClosePipe(StdinReadPipe, StdinWritePipe);
        StdinReadPipe = nullptr;
        StdinWritePipe = nullptr;
    }
}

void FCortexCodexAppServerWorker::DispatchLine(const FString& Line)
{
    TArray<FCortexStreamEvent> Events;
    FCortexCodexAppServerProtocol::ParseLine(Line, ProtocolState, Events);
    for (const FCortexStreamEvent& Event : Events)
    {
        AsyncTask(ENamedThreads::GameThread, [Delegate = OnEvent, Event]()
        {
            Delegate.ExecuteIfBound(Event);
        });
    }
}
