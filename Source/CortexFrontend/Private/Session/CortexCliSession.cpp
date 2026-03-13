#include "Session/CortexCliSession.h"

#include "Async/Async.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Session/CortexCliWorker.h"

FCortexCliSession::FCortexCliSession(const FCortexSessionConfig& InConfig)
    : Config(InConfig)
    , State(ECortexSessionState::Inactive)
{
}

bool FCortexCliSession::SendPrompt(const FCortexPromptRequest& Request)
{
    {
        FScopeLock Lock(&PromptMutex);
        PendingPrompt = Request.Prompt;
        PendingAccessMode = Request.AccessMode;
    }

    ECortexSessionState CurrentState = State.load();
    if (CurrentState == ECortexSessionState::Inactive)
    {
        BroadcastStateChange(CurrentState, ECortexSessionState::Spawning, TEXT("Spawning persistent Claude CLI session"));
        State.store(ECortexSessionState::Spawning);
        if (!SpawnProcess(Request.AccessMode, false))
        {
            const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Inactive);
            BroadcastStateChange(PreviousState, ECortexSessionState::Inactive, TEXT("Failed to spawn Claude CLI"));
            return false;
        }

        CurrentState = State.load();
    }

    if (CurrentState == ECortexSessionState::Respawning)
    {
        if (!SpawnProcess(Request.AccessMode, true))
        {
            const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Inactive);
            BroadcastStateChange(PreviousState, ECortexSessionState::Inactive, TEXT("Failed to resume Claude CLI session"));
            return false;
        }

        CurrentState = State.load();
    }

    if (CurrentState == ECortexSessionState::Spawning)
    {
        return true;
    }

    if (CurrentState != ECortexSessionState::Idle)
    {
        return false;
    }

    const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Processing);
    BroadcastStateChange(PreviousState, ECortexSessionState::Processing, TEXT("Prompt dispatched"));
    WakeWorker();
    return true;
}

bool FCortexCliSession::Cancel()
{
    if (!TransitionState(ECortexSessionState::Processing, ECortexSessionState::Cancelling, TEXT("Cancellation requested")))
    {
        return false;
    }

    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::TerminateProc(ProcessHandle, true);
        HandleProcessExited(TEXT("Cancellation requested"));
    }
    else
    {
        const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Respawning);
        BroadcastStateChange(PreviousState, ECortexSessionState::Respawning, TEXT("Cancellation requested"));
    }
    return true;
}

void FCortexCliSession::NewChat()
{
    CleanupProcess();
    ClearConversation();
    Config.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Inactive);
    BroadcastStateChange(PreviousState, ECortexSessionState::Inactive, TEXT("New chat"));
}

void FCortexCliSession::Shutdown()
{
    CleanupProcess();
    const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Terminated);
    BroadcastStateChange(PreviousState, ECortexSessionState::Terminated, TEXT("Shutdown"));
}

void FCortexCliSession::HandleWorkerEvent(const FCortexStreamEvent& Event)
{
    OnStreamEvent.Broadcast(Event);

    switch (Event.Type)
    {
    case ECortexStreamEventType::ContentBlockDelta:
        UpdateStreamingAssistantText(Event.Text, true);
        return;

    case ECortexStreamEventType::TextContent:
        UpdateStreamingAssistantText(Event.Text, false);
        return;

    case ECortexStreamEventType::ToolUse:
    {
        TSharedPtr<FCortexChatEntry> ToolEntry = MakeShared<FCortexChatEntry>();
        ToolEntry->Type = ECortexChatEntryType::ToolCall;
        ToolEntry->ToolName = Event.ToolName;
        ToolEntry->ToolCallId = Event.ToolCallId;
        ToolEntry->ToolInput = Event.ToolInput;
        ChatEntries.Add(ToolEntry);
        return;
    }

    case ECortexStreamEventType::ToolResult:
        for (int32 Index = ChatEntries.Num() - 1; Index >= 0; --Index)
        {
            if (ChatEntries[Index]->Type == ECortexChatEntryType::ToolCall && ChatEntries[Index]->ToolCallId == Event.ToolCallId)
            {
                ChatEntries[Index]->ToolResult = Event.ToolResultContent;
                ChatEntries[Index]->bIsToolComplete = true;
                break;
            }
        }
        return;

    case ECortexStreamEventType::Result:
    {
        FCortexTurnResult Result;
        Result.ResultText = Event.ResultText;
        Result.bIsError = Event.bIsError;
        Result.DurationMs = Event.DurationMs;
        Result.NumTurns = Event.NumTurns;
        Result.TotalCostUsd = Event.TotalCostUsd;
        Result.SessionId = Event.SessionId;

        const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Idle);
        BroadcastStateChange(PreviousState, ECortexSessionState::Idle, TEXT("Turn complete"));
        OnTurnComplete.Broadcast(Result);
        return;
    }

    default:
        return;
    }
}

void FCortexCliSession::HandleProcessExited(const FString& Reason)
{
    const ECortexSessionState CurrentState = State.load();
    if (CurrentState == ECortexSessionState::Cancelling)
    {
        CleanupProcess();
        BroadcastStateChange(CurrentState, ECortexSessionState::Respawning, Reason);
        State.store(ECortexSessionState::Respawning);
        if (!CachedCliInfo.bIsValid)
        {
            return;
        }
        if (!SpawnProcess(GetPendingAccessMode(), true))
        {
            const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Inactive);
            BroadcastStateChange(PreviousState, ECortexSessionState::Inactive, TEXT("Failed to respawn Claude CLI"));
            return;
        }
        return;
    }

    CleanupProcess();
    BroadcastStateChange(CurrentState, ECortexSessionState::Inactive, Reason);
    State.store(ECortexSessionState::Inactive);
}

FString FCortexCliSession::BuildLaunchCommandLine(bool bResumeSession, ECortexAccessMode AccessMode) const
{
    FString CommandLine = TEXT("-p --input-format stream-json --output-format stream-json --verbose --include-partial-messages ");

    if (Config.bSkipPermissions)
    {
        CommandLine += TEXT("--dangerously-skip-permissions ");
    }

    if (bResumeSession)
    {
        CommandLine += FString::Printf(TEXT("--resume \"%s\" "), *Config.SessionId);
    }
    else
    {
        CommandLine += FString::Printf(TEXT("--session-id \"%s\" "), *Config.SessionId);
    }

    const FString AllowedTools = BuildAllowedToolsArg(AccessMode);
    if (!AllowedTools.IsEmpty())
    {
        CommandLine += FString::Printf(TEXT("--allowedTools \"%s\" "), *AllowedTools);
    }

    if (!Config.McpConfigPath.IsEmpty())
    {
        CommandLine += FString::Printf(TEXT("--mcp-config \"%s\" "), *Config.McpConfigPath.Replace(TEXT("\\"), TEXT("/")));
    }

    FString ModeString;
    switch (AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        ModeString = TEXT("Read-Only");
        break;
    case ECortexAccessMode::Guided:
        ModeString = TEXT("Guided");
        break;
    case ECortexAccessMode::FullAccess:
        ModeString = TEXT("Full Access");
        break;
    }

    const FString SystemPrompt = FString::Printf(TEXT("You are running inside the Unreal Editor's Cortex AI Chat panel. You have access to Cortex MCP tools for querying and manipulating the editor. Current access mode: %s."), *ModeString);
    CommandLine += FString::Printf(TEXT("--append-system-prompt \"%s\" "), *SystemPrompt.Replace(TEXT("\""), TEXT("\\\"")));

    return CommandLine;
}

FString FCortexCliSession::BuildAllowedToolsArg(ECortexAccessMode AccessMode) const
{
    switch (AccessMode)
    {
    case ECortexAccessMode::ReadOnly:
        return TEXT("mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*");
    case ECortexAccessMode::Guided:
        return TEXT("mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*,"
            "mcp__cortex_mcp__spawn_*,mcp__cortex_mcp__create_*,mcp__cortex_mcp__add_*,mcp__cortex_mcp__set_*,mcp__cortex_mcp__compile_*,mcp__cortex_mcp__connect_*,"
            "mcp__cortex_mcp__graph_add_*,mcp__cortex_mcp__graph_connect,mcp__cortex_mcp__graph_list_*,mcp__cortex_mcp__graph_get_*,mcp__cortex_mcp__graph_set_*,mcp__cortex_mcp__graph_search_*,mcp__cortex_mcp__graph_auto_layout,"
            "mcp__cortex_mcp__open_*,mcp__cortex_mcp__close_*,mcp__cortex_mcp__focus_*,mcp__cortex_mcp__select_*,"
            "mcp__cortex_mcp__rename_*,mcp__cortex_mcp__configure_*,mcp__cortex_mcp__import_*,mcp__cortex_mcp__update_*,mcp__cortex_mcp__duplicate_*,mcp__cortex_mcp__reparent*,mcp__cortex_mcp__attach_*,mcp__cortex_mcp__detach_*,mcp__cortex_mcp__register_*,mcp__cortex_mcp__reload_*");
    case ECortexAccessMode::FullAccess:
        return FString();
    }

    return FString();
}

FString FCortexCliSession::BuildPromptEnvelope(const FString& Prompt) const
{
    FString EscapedPrompt = Prompt;
    EscapedPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    EscapedPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));
    EscapedPrompt.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    EscapedPrompt.ReplaceInline(TEXT("\r"), TEXT("\\r"));
    EscapedPrompt.ReplaceInline(TEXT("\t"), TEXT("\\t"));
    return FString::Printf(TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"%s\"}}\n"), *EscapedPrompt);
}

bool FCortexCliSession::SpawnProcess(ECortexAccessMode AccessMode, bool bResumeSession)
{
    CleanupProcess();

    CachedCliInfo = FCortexCliDiscovery::FindClaude();
    if (!CachedCliInfo.bIsValid)
    {
        return false;
    }

    if (!FPlatformProcess::CreatePipe(StdoutReadPipe, StdoutWritePipe, false))
    {
        return false;
    }

    if (!FPlatformProcess::CreatePipe(StdinReadPipe, StdinWritePipe, true))
    {
        FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
        StdoutReadPipe = nullptr;
        StdoutWritePipe = nullptr;
        return false;
    }

    const FString WorkingDirectory = !Config.WorkingDirectory.IsEmpty()
        ? Config.WorkingDirectory
        : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const FString CommandLine = BuildLaunchCommandLine(bResumeSession, AccessMode);

    ProcessHandle = FPlatformProcess::CreateProc(
        *CachedCliInfo.Path,
        *CommandLine,
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
        CleanupProcess();
        return false;
    }

    if (PromptReadyEvent == nullptr)
    {
        PromptReadyEvent = FPlatformProcess::GetSynchEventFromPool(false);
    }

    Worker = MakeUnique<FCortexCliWorker>(AsShared());

    const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Idle);
    BroadcastStateChange(PreviousState, ECortexSessionState::Idle, TEXT("Claude CLI session ready"));
    return true;
}

void FCortexCliSession::CleanupProcess()
{
    if (Worker)
    {
        Worker->Stop();
        Worker.Reset();
    }

    if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
    {
        FPlatformProcess::TerminateProc(ProcessHandle, true);
        while (FPlatformProcess::IsProcRunning(ProcessHandle))
        {
            FPlatformProcess::Sleep(0.01f);
        }
    }

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

    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::CloseProc(ProcessHandle);
        ProcessHandle.Reset();
    }

    if (PromptReadyEvent != nullptr)
    {
        FPlatformProcess::ReturnSynchEventToPool(PromptReadyEvent);
        PromptReadyEvent = nullptr;
    }
}

void FCortexCliSession::WakeWorker()
{
    if (PromptReadyEvent != nullptr)
    {
        PromptReadyEvent->Trigger();
    }
}

FString FCortexCliSession::ConsumePendingPromptEnvelope()
{
    FScopeLock Lock(&PromptMutex);

    if (!PendingPrompt.IsSet())
    {
        return FString();
    }

    const FString Envelope = BuildPromptEnvelope(PendingPrompt.GetValue());
    PendingPrompt.Reset();
    return Envelope;
}

ECortexAccessMode FCortexCliSession::GetPendingAccessMode() const
{
    FScopeLock Lock(&PromptMutex);
    return PendingAccessMode.Get(ECortexAccessMode::ReadOnly);
}

const TArray<TSharedPtr<FCortexChatEntry>>& FCortexCliSession::GetChatEntries() const
{
    return ChatEntries;
}

FString FCortexCliSession::GetSessionId() const
{
    return Config.SessionId;
}

ECortexSessionState FCortexCliSession::GetState() const
{
    return State.load();
}

void FCortexCliSession::AddUserPromptEntry(const FString& Message)
{
    TSharedPtr<FCortexChatEntry> UserEntry = MakeShared<FCortexChatEntry>();
    UserEntry->Type = ECortexChatEntryType::UserMessage;
    UserEntry->Text = Message;
    ChatEntries.Add(UserEntry);

    CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
    CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
    ChatEntries.Add(CurrentStreamingEntry);
}

void FCortexCliSession::RollbackLastPromptEntries()
{
    if (CurrentStreamingEntry.IsValid())
    {
        ChatEntries.RemoveSingle(CurrentStreamingEntry);
        CurrentStreamingEntry.Reset();
    }

    if (ChatEntries.Num() > 0 && ChatEntries.Last()->Type == ECortexChatEntryType::UserMessage)
    {
        ChatEntries.RemoveAt(ChatEntries.Num() - 1);
    }
}

void FCortexCliSession::UpdateStreamingAssistantText(const FString& Text, bool bAppend)
{
    if (!CurrentStreamingEntry.IsValid())
    {
        CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
        CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
        ChatEntries.Add(CurrentStreamingEntry);
    }

    if (bAppend)
    {
        CurrentStreamingEntry->Text += Text;
    }
    else
    {
        CurrentStreamingEntry->Text = Text;
    }
}

void FCortexCliSession::ReplaceStreamingEntry(const TArray<TSharedPtr<FCortexChatEntry>>& ReplacementEntries)
{
    if (!CurrentStreamingEntry.IsValid())
    {
        return;
    }

    const int32 CurrentIndex = ChatEntries.IndexOfByKey(CurrentStreamingEntry);
    if (CurrentIndex == INDEX_NONE)
    {
        return;
    }

    ChatEntries.RemoveAt(CurrentIndex);
    ChatEntries.Insert(ReplacementEntries, CurrentIndex);
    CurrentStreamingEntry.Reset();
}

void FCortexCliSession::ClearConversation()
{
    FScopeLock Lock(&PromptMutex);
    PendingPrompt.Reset();
    PendingAccessMode.Reset();
    CurrentStreamingEntry.Reset();
    ChatEntries.Reset();
}

bool FCortexCliSession::TransitionState(ECortexSessionState ExpectedState, ECortexSessionState NewState, const FString& Reason)
{
    ECortexSessionState LocalExpected = ExpectedState;
    if (!State.compare_exchange_strong(LocalExpected, NewState))
    {
        return false;
    }

    BroadcastStateChange(ExpectedState, NewState, Reason);
    return true;
}

void FCortexCliSession::BroadcastStateChange(ECortexSessionState PreviousState, ECortexSessionState NewState, const FString& Reason)
{
    FCortexSessionStateChange Change;
    Change.PreviousState = PreviousState;
    Change.NewState = NewState;
    Change.Reason = Reason;
    OnStateChanged.Broadcast(Change);
}

ECortexSessionState FCortexCliSession::GetStateForTest() const
{
    return State.load();
}

void FCortexCliSession::SetStateForTest(ECortexSessionState NewState)
{
    State.store(NewState);
}

FString FCortexCliSession::GetPendingPromptForTest() const
{
    FScopeLock Lock(&PromptMutex);
    return PendingPrompt.Get(TEXT(""));
}

TSharedPtr<FCortexChatEntry> FCortexCliSession::GetCurrentStreamingEntry() const
{
    return CurrentStreamingEntry;
}
