#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Process/CortexCliDiscovery.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexSessionTypes.h"
#include <atomic>

DECLARE_DELEGATE_OneParam(FOnCortexStreamEvent, const FCortexStreamEvent&);
DECLARE_DELEGATE_TwoParams(FOnCortexComplete, const FString&, bool);

struct FCortexChatRequest
{
    FString Prompt;
    FString SessionId;
    bool bIsFirstMessage = true;
    ECortexAccessMode AccessMode = ECortexAccessMode::ReadOnly;
    bool bSkipPermissions = true;
    FString McpConfigPath;
    FString WorkingDirectory;
};

class FCortexCliRunner : public FRunnable
{
public:
    FCortexCliRunner();
    virtual ~FCortexCliRunner();

    bool ExecuteAsync(const FCortexChatRequest& Request, FOnCortexComplete OnComplete, FOnCortexStreamEvent OnStreamEvent);

    void Cancel();
    bool IsExecuting() const { return bIsExecuting.load(); }

    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

    friend class FCortexCliRunnerBuildCommandLineTest;
    friend class FCortexCliRunnerConcurrencyGuardTest;
    friend class FCortexCliRunnerAllowedToolsTest;

private:
    FString BuildCommandLine(const FCortexChatRequest& Request);
    FString BuildAllowedToolsArg(ECortexAccessMode Mode);
    FString BuildStdinPayload(const FString& Prompt);

    bool CreateProcessPipes();
    void ReadProcessOutput();
    void ParseAndEmitLines(const FString& Chunk);
    void CleanupHandles();

    FCortexChatRequest CurrentRequest;
    FCortexCliInfo CachedCliInfo;
    FOnCortexComplete OnCompleteDelegate;
    FOnCortexStreamEvent OnStreamEventDelegate;

    FRunnableThread* Thread = nullptr;
    std::atomic<int32> StopCounter{0};
    std::atomic<bool> bIsExecuting{false};

    FProcHandle ProcessHandle;
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    void* StdInReadPipe = nullptr;
    void* StdInWritePipe = nullptr;

    FString NdjsonLineBuffer;
    FString AccumulatedText;
    FString RawOutputBuffer;  // Captures non-JSON lines for error diagnostics
};
