#include "Session/CortexCliSession.h"

#include "Session/CortexCliWorker.h"

FCortexCliSession::FCortexCliSession(const FCortexSessionConfig& InConfig)
    : Config(InConfig)
    , State(ECortexSessionState::Inactive)
{
}

bool FCortexCliSession::SendPrompt(const FCortexPromptRequest& Request)
{
    const ECortexSessionState CurrentState = State.load();
    switch (CurrentState)
    {
    case ECortexSessionState::Inactive:
        PendingPrompt = Request.Prompt;
        PendingAccessMode = Request.AccessMode;
        BroadcastStateChange(CurrentState, ECortexSessionState::Spawning, TEXT("Initial prompt queued"));
        State.store(ECortexSessionState::Spawning);
        return true;

    case ECortexSessionState::Spawning:
        PendingPrompt = Request.Prompt;
        PendingAccessMode = Request.AccessMode;
        return true;

    case ECortexSessionState::Idle:
        PendingPrompt = Request.Prompt;
        PendingAccessMode = Request.AccessMode;
        BroadcastStateChange(CurrentState, ECortexSessionState::Processing, TEXT("Prompt dispatched"));
        State.store(ECortexSessionState::Processing);
        return true;

    default:
        return false;
    }
}

bool FCortexCliSession::Cancel()
{
    return TransitionState(ECortexSessionState::Processing, ECortexSessionState::Cancelling, TEXT("Cancellation requested"));
}

void FCortexCliSession::NewChat()
{
    PendingPrompt.Reset();
    PendingAccessMode.Reset();
    BroadcastStateChange(State.exchange(ECortexSessionState::Inactive), ECortexSessionState::Inactive, TEXT("New chat"));
}

void FCortexCliSession::Shutdown()
{
    const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Terminated);
    BroadcastStateChange(PreviousState, ECortexSessionState::Terminated, TEXT("Shutdown"));
}

void FCortexCliSession::HandleWorkerEvent(const FCortexStreamEvent& Event)
{
    OnStreamEvent.Broadcast(Event);

    if (Event.Type != ECortexStreamEventType::Result)
    {
        return;
    }

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
}

void FCortexCliSession::HandleProcessExited(const FString& Reason)
{
    const ECortexSessionState CurrentState = State.load();
    if (CurrentState == ECortexSessionState::Cancelling)
    {
        BroadcastStateChange(CurrentState, ECortexSessionState::Respawning, Reason);
        State.store(ECortexSessionState::Respawning);
        return;
    }

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
    return PendingPrompt.Get(TEXT(""));
}
