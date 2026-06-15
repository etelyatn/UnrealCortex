#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Providers/CortexCodexCliProvider.h"

namespace
{
    bool WaitForAppServerInitializeResponse(void* StdoutReadPipe, FString& OutOutput)
    {
        const double StartTime = FPlatformTime::Seconds();
        while ((FPlatformTime::Seconds() - StartTime) < 5.0)
        {
            const FString Chunk = StdoutReadPipe != nullptr
                ? FPlatformProcess::ReadPipe(StdoutReadPipe)
                : FString();
            if (!Chunk.IsEmpty())
            {
                OutOutput += Chunk;
                if (OutOutput.Contains(TEXT("\"id\":1")) && OutOutput.Contains(TEXT("\"userAgent\"")))
                {
                    return true;
                }
            }
            FPlatformProcess::Sleep(0.01f);
        }
        return false;
    }

    bool WaitForAppServerThreadStartResponse(void* StdoutReadPipe, FString& OutOutput)
    {
        const double StartTime = FPlatformTime::Seconds();
        while ((FPlatformTime::Seconds() - StartTime) < 15.0)
        {
            const FString Chunk = StdoutReadPipe != nullptr
                ? FPlatformProcess::ReadPipe(StdoutReadPipe)
                : FString();
            if (!Chunk.IsEmpty())
            {
                OutOutput += Chunk;
                if (OutOutput.Contains(TEXT("\"id\":2")) && OutOutput.Contains(TEXT("\"thread\"")))
                {
                    return true;
                }
                if (OutOutput.Contains(TEXT("\"id\":2")) && OutOutput.Contains(TEXT("\"error\"")))
                {
                    return false;
                }
            }
            FPlatformProcess::Sleep(0.01f);
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodexAppServerHandshakeSmokeTest,
    "Cortex.Frontend.CodexAppServer.HandshakeSmoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodexAppServerHandshakeSmokeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCodexCliProvider Provider;
    const FCortexCliInfo Info = Provider.FindCli();
    if (!Info.bIsValid)
    {
        AddInfo(TEXT("Codex CLI not found; app-server handshake smoke test skipped."));
        return true;
    }

    void* StdoutReadPipe = nullptr;
    void* StdoutWritePipe = nullptr;
    void* StdinReadPipe = nullptr;
    void* StdinWritePipe = nullptr;
    if (!FPlatformProcess::CreatePipe(StdoutReadPipe, StdoutWritePipe, false))
    {
        AddError(TEXT("Failed to create stdout pipe for Codex app-server handshake smoke test."));
        return false;
    }
    if (!FPlatformProcess::CreatePipe(StdinReadPipe, StdinWritePipe, true))
    {
        FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
        AddError(TEXT("Failed to create stdin pipe for Codex app-server handshake smoke test."));
        return false;
    }

    FProcHandle ProcessHandle = FPlatformProcess::CreateProc(
        *Info.Path,
        TEXT("app-server --stdio"),
        false,
        true,
        false,
        nullptr,
        0,
        nullptr,
        StdoutWritePipe,
        StdinReadPipe);

    if (!ProcessHandle.IsValid())
    {
        FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
        FPlatformProcess::ClosePipe(StdinReadPipe, StdinWritePipe);
        AddError(TEXT("Failed to launch Codex app-server for handshake smoke test."));
        return false;
    }

    const FString InitializeLine =
        TEXT("{\"method\":\"initialize\",\"id\":1,\"params\":{\"clientInfo\":{\"name\":\"cortex_frontend_smoke\",\"title\":\"Cortex Frontend Smoke\",\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":true}}}\n");
    TestTrue(TEXT("Initialize request should write to app-server stdin"),
        FPlatformProcess::WritePipe(StdinWritePipe, InitializeLine));

    FString Output;
    const bool bSawInitialize = WaitForAppServerInitializeResponse(StdoutReadPipe, Output);
    TestTrue(TEXT("Codex app-server should respond to initialize without starting a model turn"), bSawInitialize);
    TestTrue(TEXT("Initialize response should include userAgent"), Output.Contains(TEXT("\"userAgent\"")));

    FPlatformProcess::WritePipe(StdinWritePipe, TEXT("{\"method\":\"initialized\",\"params\":{}}\n"));

    const FString ThreadStartLine =
        TEXT("{\"method\":\"thread/start\",\"id\":2,\"params\":{\"sandbox\":\"read-only\",\"approvalPolicy\":\"never\",\"ephemeral\":true}}\n");
    TestTrue(TEXT("Thread start request should write to app-server stdin"),
        FPlatformProcess::WritePipe(StdinWritePipe, ThreadStartLine));

    const bool bSawThreadStart = WaitForAppServerThreadStartResponse(StdoutReadPipe, Output);
    TestTrue(TEXT("Codex app-server should respond to thread/start without starting a model turn"), bSawThreadStart);
    TestTrue(TEXT("Thread start response should include thread object"), Output.Contains(TEXT("\"thread\"")));

    if (FPlatformProcess::IsProcRunning(ProcessHandle))
    {
        FPlatformProcess::TerminateProc(ProcessHandle, true);
    }
    FPlatformProcess::CloseProc(ProcessHandle);
    FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
    FPlatformProcess::ClosePipe(StdinReadPipe, StdinWritePipe);
    return true;
}
