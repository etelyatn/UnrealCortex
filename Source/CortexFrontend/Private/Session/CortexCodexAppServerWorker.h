#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Process/CortexStreamEvent.h"
#include "Providers/CortexCodexAppServerProtocol.h"
#include "Session/CortexSessionTypes.h"
#include <atomic>

DECLARE_DELEGATE_OneParam(FOnCortexCodexAppServerEvent, const FCortexStreamEvent&);
DECLARE_DELEGATE_OneParam(FOnCortexCodexAppServerExited, const FString&);

class FCortexCodexAppServerWorker : public FRunnable
{
public:
    FCortexCodexAppServerWorker(const FCortexSessionConfig& InConfig, ECortexAccessMode InAccessMode);
    virtual ~FCortexCodexAppServerWorker() override;

    bool Start();
    bool SendTurn(const FString& Prompt, ECortexAccessMode AccessMode);
    bool InterruptTurn();
    void Shutdown();

    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;

    FOnCortexCodexAppServerEvent OnEvent;
    FOnCortexCodexAppServerExited OnExited;

#if WITH_DEV_AUTOMATION_TESTS
    static void SetWriteOverrideForTests(TFunction<bool(const FString&)> InOverride);
    static void ClearWriteOverrideForTests();
    void StartForTests();
    void SetThreadIdForTests(const FString& ThreadId);
#endif

private:
    int32 NextRequestId();
    bool QueueWrite(const FString& Line);
    bool FlushQueuedWrites();
    bool LaunchProcess();
    void ClosePipes();
    void DispatchLine(const FString& Line);

    FCortexSessionConfig Config;
    ECortexAccessMode InitialAccessMode = ECortexAccessMode::ReadOnly;
    FCortexCodexAppServerProtocolState ProtocolState;
    TQueue<FString, EQueueMode::Mpsc> PendingWrites;
    FCriticalSection WriteMutex;
    std::atomic<bool> bStopRequested{false};
    std::atomic<int32> RequestCounter{0};
    FRunnableThread* Thread = nullptr;
    FProcHandle ProcessHandle;
    void* StdoutReadPipe = nullptr;
    void* StdoutWritePipe = nullptr;
    void* StdinReadPipe = nullptr;
    void* StdinWritePipe = nullptr;
    FString ReadBuffer;
};
