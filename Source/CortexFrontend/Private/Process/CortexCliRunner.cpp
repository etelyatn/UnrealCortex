#include "Process/CortexCliRunner.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FCortexCliRunner::FCortexCliRunner() = default;

FCortexCliRunner::~FCortexCliRunner()
{
    StopCounter.store(1);

    if (Thread)
    {
        Thread->Kill(true);
        delete Thread;
        Thread = nullptr;
    }

    CleanupHandles();
}

bool FCortexCliRunner::ExecuteAsync(const FCortexChatRequest& Request, FOnCortexComplete OnComplete, FOnCortexStreamEvent OnStreamEvent)
{
    if (bIsExecuting.load())
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("CLI runner is already executing"));
        return false;
    }

    CachedCliInfo = FCortexCliDiscovery::FindClaude();
    if (!CachedCliInfo.bIsValid)
    {
        OnComplete.ExecuteIfBound(TEXT("Claude CLI not found. Install: npm install -g @anthropic-ai/claude-code"), false);
        return false;
    }

    if (Thread)
    {
        Thread->Kill(true);
        delete Thread;
        Thread = nullptr;
    }

    CurrentRequest = Request;
    OnCompleteDelegate = OnComplete;
    OnStreamEventDelegate = OnStreamEvent;
    bIsExecuting.store(true);

    Thread = FRunnableThread::Create(this, TEXT("CortexCliRunner"), 0, TPri_Normal);
    if (!Thread)
    {
        bIsExecuting.store(false);
        return false;
    }

    return true;
}

void FCortexCliRunner::Cancel()
{
    StopCounter.store(1);
    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::TerminateProc(ProcessHandle, true);
    }
}

bool FCortexCliRunner::Init()
{
    StopCounter.store(0);
    NdjsonLineBuffer.Empty();
    AccumulatedText.Empty();
    return true;
}

uint32 FCortexCliRunner::Run()
{
    if (!CreateProcessPipes())
    {
        const FOnCortexComplete CompleteCopy = OnCompleteDelegate;
        AsyncTask(ENamedThreads::GameThread, [CompleteCopy]()
        {
            CompleteCopy.ExecuteIfBound(TEXT("Failed to create pipes"), false);
        });
        return 1;
    }

    FString Executable = CachedCliInfo.Path;
    FString CommandLine = BuildCommandLine(CurrentRequest);
    FString Params = CommandLine;
    if (CachedCliInfo.bIsCmd)
    {
        Params = FString::Printf(TEXT("/c \"%s\" %s"), *CachedCliInfo.Path, *CommandLine);
        Executable = TEXT("cmd.exe");
    }

    FString WorkingDir = CurrentRequest.WorkingDirectory;
    if (WorkingDir.IsEmpty())
    {
        WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    }

    UE_LOG(LogCortexFrontend, Log, TEXT("Launching: %s %s"), *Executable, *Params);
    ProcessHandle = FPlatformProcess::CreateProc(*Executable, *Params, false, true, false, nullptr, 0, *WorkingDir, WritePipe, StdInReadPipe);
    if (!ProcessHandle.IsValid())
    {
        CleanupHandles();
        const FOnCortexComplete CompleteCopy = OnCompleteDelegate;
        AsyncTask(ENamedThreads::GameThread, [CompleteCopy]()
        {
            CompleteCopy.ExecuteIfBound(TEXT("Failed to start Claude process"), false);
        });
        return 1;
    }

    const FString Payload = BuildStdinPayload(CurrentRequest.Prompt);
    if (StdInWritePipe && !Payload.IsEmpty())
    {
        FPlatformProcess::WritePipe(StdInWritePipe, Payload);
    }

    if (StdInReadPipe || StdInWritePipe)
    {
        FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
        StdInReadPipe = nullptr;
        StdInWritePipe = nullptr;
    }

    ReadProcessOutput();

    int32 ExitCode = 0;
    FPlatformProcess::GetProcReturnCode(ProcessHandle, &ExitCode);
    CleanupHandles();

    const bool bSuccess = ExitCode == 0 && !StopCounter.load();
    const FString FinalText = AccumulatedText;
    const FOnCortexComplete CompleteCopy = OnCompleteDelegate;
    AsyncTask(ENamedThreads::GameThread, [CompleteCopy, FinalText, bSuccess]()
    {
        CompleteCopy.ExecuteIfBound(FinalText, bSuccess);
    });

    return 0;
}

void FCortexCliRunner::Stop()
{
    StopCounter.store(1);
}

void FCortexCliRunner::Exit()
{
    bIsExecuting.store(false);
}

FString FCortexCliRunner::BuildCommandLine(const FCortexChatRequest& Request)
{
    FString Cmd = TEXT("-p --verbose --output-format stream-json --input-format stream-json --include-partial-messages ");

    if (Request.bSkipPermissions)
    {
        Cmd += TEXT("--dangerously-skip-permissions ");
    }

    Cmd += FString::Printf(TEXT("--session-id \"%s\" "), *Request.SessionId);

    if (!Request.bIsFirstMessage)
    {
        Cmd += TEXT("--resume ");
    }

    const FString AllowedTools = BuildAllowedToolsArg(Request.AccessMode);
    if (!AllowedTools.IsEmpty())
    {
        Cmd += FString::Printf(TEXT("--allowedTools \"%s\" "), *AllowedTools);
    }

    if (!Request.McpConfigPath.IsEmpty())
    {
        Cmd += FString::Printf(TEXT("--mcp-config \"%s\" "), *Request.McpConfigPath.Replace(TEXT("\\"), TEXT("/")));
    }

    FString ModeStr;
    switch (Request.AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        ModeStr = TEXT("Read-Only");
        break;
    case ECortexAccessMode::Guided:
        ModeStr = TEXT("Guided");
        break;
    case ECortexAccessMode::FullAccess:
        ModeStr = TEXT("Full Access");
        break;
    }

    const FString SystemPrompt = FString::Printf(TEXT("You are running inside the Unreal Editor's Cortex AI Chat panel. You have access to Cortex MCP tools for querying and manipulating the editor. Current access mode: %s."), *ModeStr);
    Cmd += FString::Printf(TEXT("--append-system-prompt \"%s\" "), *SystemPrompt.Replace(TEXT("\""), TEXT("\\\"")));

    return Cmd;
}

FString FCortexCliRunner::BuildAllowedToolsArg(ECortexAccessMode Mode)
{
    switch (Mode)
    {
    case ECortexAccessMode::ReadOnly:
        return TEXT("mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*");
    case ECortexAccessMode::Guided:
        return TEXT("mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*,mcp__cortex_mcp__spawn_*,mcp__cortex_mcp__create_*,mcp__cortex_mcp__add_*,mcp__cortex_mcp__set_*,mcp__cortex_mcp__compile_*,mcp__cortex_mcp__connect_*,mcp__cortex_mcp__graph_*,mcp__cortex_mcp__open_*,mcp__cortex_mcp__close_*,mcp__cortex_mcp__focus_*,mcp__cortex_mcp__select_*,mcp__cortex_mcp__rename_*");
    case ECortexAccessMode::FullAccess:
        return FString();
    }

    return FString();
}

FString FCortexCliRunner::BuildStdinPayload(const FString& Prompt)
{
    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), Prompt);

    TArray<TSharedPtr<FJsonValue>> ContentArray;
    ContentArray.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("role"), TEXT("user"));
    Message->SetArrayField(TEXT("content"), ContentArray);

    TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("type"), TEXT("user"));
    Envelope->SetObjectField(TEXT("message"), Message);

    FString JsonLine;
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonLine);
    FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
    Writer->Close();
    JsonLine += TEXT("\n");
    return JsonLine;
}

bool FCortexCliRunner::CreateProcessPipes()
{
    if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe, false))
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create stdout pipe"));
        return false;
    }

    if (!FPlatformProcess::CreatePipe(StdInReadPipe, StdInWritePipe, true))
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create stdin pipe"));
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        ReadPipe = nullptr;
        WritePipe = nullptr;
        return false;
    }

    return true;
}

void FCortexCliRunner::ReadProcessOutput()
{
    while (!StopCounter.load())
    {
        const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
        if (!Chunk.IsEmpty())
        {
            ParseAndEmitLines(Chunk);
        }

        if (!FPlatformProcess::IsProcRunning(ProcessHandle))
        {
            FString Remaining = FPlatformProcess::ReadPipe(ReadPipe);
            while (!Remaining.IsEmpty())
            {
                ParseAndEmitLines(Remaining);
                Remaining = FPlatformProcess::ReadPipe(ReadPipe);
            }

            NdjsonLineBuffer.TrimEndInline();
            if (!NdjsonLineBuffer.IsEmpty())
            {
                ParseAndEmitLines(NdjsonLineBuffer + TEXT("\n"));
                NdjsonLineBuffer.Empty();
            }
            break;
        }

        FPlatformProcess::Sleep(0.01f);
    }
}

void FCortexCliRunner::ParseAndEmitLines(const FString& Chunk)
{
    NdjsonLineBuffer += Chunk;

    int32 NewlineIndex = INDEX_NONE;
    while (NdjsonLineBuffer.FindChar(TEXT('\n'), NewlineIndex))
    {
        FString Line = NdjsonLineBuffer.Left(NewlineIndex);
        Line.TrimEndInline();
        NdjsonLineBuffer.RightChopInline(NewlineIndex + 1, EAllowShrinking::No);

        if (Line.IsEmpty())
        {
            continue;
        }

        TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(Line);
        for (FCortexStreamEvent& Event : Events)
        {
            if (Event.Type == ECortexStreamEventType::TextContent)
            {
                AccumulatedText += Event.Text;
            }

            if (OnStreamEventDelegate.IsBound())
            {
                const FOnCortexStreamEvent EventCopy = OnStreamEventDelegate;
                const FCortexStreamEvent EventData = MoveTemp(Event);
                AsyncTask(ENamedThreads::GameThread, [EventCopy, EventData]()
                {
                    EventCopy.ExecuteIfBound(EventData);
                });
            }
        }
    }
}

void FCortexCliRunner::CleanupHandles()
{
    if (ReadPipe || WritePipe)
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        ReadPipe = nullptr;
        WritePipe = nullptr;
    }

    if (StdInReadPipe || StdInWritePipe)
    {
        FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
        StdInReadPipe = nullptr;
        StdInWritePipe = nullptr;
    }

    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::CloseProc(ProcessHandle);
        ProcessHandle.Reset();
    }
}
