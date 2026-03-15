#include "Session/CortexCliSession.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Session/CortexCliWorker.h"

namespace
{
	constexpr uint8 MaxRespawnAttempts = 2;
	constexpr float GracePeriodSeconds = 2.0f;
}

FCortexCliSession::FCortexCliSession(const FCortexSessionConfig& InConfig)
	: Config(InConfig)
	, State(ECortexSessionState::Inactive)
{
}

bool FCortexCliSession::Connect()
{
	UE_LOG(LogCortexFrontend, Log, TEXT("Connect() called, current state: %d"), static_cast<int32>(State.load()));

	if (!TransitionState(ECortexSessionState::Inactive, ECortexSessionState::Spawning, TEXT("Auto-connect")))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Connect() failed: state was not Inactive (was %d)"), static_cast<int32>(State.load()));
		return false;
	}

	if (!SpawnProcess(FCortexFrontendSettings::Get().GetAccessMode(), false))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Connect() failed: SpawnProcess returned false"));
		TransitionState(ECortexSessionState::Spawning, ECortexSessionState::Inactive, TEXT("Failed to spawn on connect"));
		return false;
	}

	FCortexFrontendSettings::Get().ClearPendingChanges();
	UE_LOG(LogCortexFrontend, Log, TEXT("Connect() succeeded, state now: %d"), static_cast<int32>(State.load()));
	return true;
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
		if (!TransitionState(ECortexSessionState::Inactive, ECortexSessionState::Spawning, TEXT("Spawning persistent Claude CLI session")))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("SendPrompt: failed to transition Inactive -> Spawning (state race)"));
			return false;
		}
		if (!SpawnProcess(Request.AccessMode, false))
		{
			TransitionState(ECortexSessionState::Spawning, ECortexSessionState::Inactive, TEXT("Failed to spawn Claude CLI"));
			return false;
		}

		CurrentState = State.load();

		// SpawnProcess may have already drained the pending prompt (Idle → Processing).
		// In that case the prompt is already sent — return success.
		if (CurrentState == ECortexSessionState::Processing)
		{
			UE_LOG(LogCortexFrontend, Log, TEXT("Prompt drained during spawn"));
			return true;
		}
	}

	if (CurrentState == ECortexSessionState::Spawning)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Prompt queued during spawn"));
		return true;
	}

	if (CurrentState != ECortexSessionState::Idle)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("SendPrompt rejected: session in state %d"), static_cast<int32>(CurrentState));
		return false;
	}

	if (!TransitionState(ECortexSessionState::Idle, ECortexSessionState::Processing, TEXT("Prompt dispatched")))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("SendPrompt: failed to transition Idle -> Processing (state race)"));
		return false;
	}

	WakeWorker();
	return true;
}

bool FCortexCliSession::Cancel()
{
	if (!TransitionState(ECortexSessionState::Processing, ECortexSessionState::Cancelling, TEXT("Cancellation requested")))
	{
		return false;
	}

	++CancelGeneration;
	UE_LOG(LogCortexFrontend, Log, TEXT("Cancel initiated (generation %u)"), CancelGeneration);

	if (!ProcessHandle.IsValid() || !FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Cancel: no active process, handling exit immediately"));
		HandleProcessExited(TEXT("Cancellation requested (no active process)"));
		return true;
	}

	// Close stdin to signal EOF — CLI will finish current turn and exit
	CloseStdinPipe();

	// Start grace timer — force-kill if process doesn't exit within grace period
	const uint32 ExpectedGen = CancelGeneration;
	TWeakPtr<FCortexCliSession> WeakSelf = AsWeak();
	GraceTimerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakSelf, ExpectedGen](float) -> bool
		{
			if (TSharedPtr<FCortexCliSession> Self = WeakSelf.Pin())
			{
				if (Self->CancelGeneration == ExpectedGen && Self->State.load() == ECortexSessionState::Cancelling)
				{
					UE_LOG(LogCortexFrontend, Log, TEXT("Cancel grace period expired (generation %u), force-killing"), ExpectedGen);
					Self->ForceKillProcess();
				}
			}
			return false;
		}),
		GracePeriodSeconds);

	return true;
}

bool FCortexCliSession::Reconnect()
{
	check(IsInGameThread());

	if (!TransitionState(ECortexSessionState::Idle, ECortexSessionState::Respawning, TEXT("Reconnect requested")))
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Reconnect() rejected: state was not Idle (was %d)"),
			static_cast<int32>(State.load()));
		return false;
	}

	CleanupProcess();

	// Uses --resume to fork from existing conversation context.
	// TODO: Verify empirically whether --resume alone re-applies --append-system-prompt
	// and --setting-sources. If not, add --fork-session flag to BuildLaunchCommandLine.
	const ECortexAccessMode AccessMode = FCortexFrontendSettings::Get().GetAccessMode();
	if (!SpawnProcess(AccessMode, true))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Reconnect() failed: SpawnProcess returned false"));
		State.store(ECortexSessionState::Inactive);
		BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive,
			TEXT("Failed to reconnect"));
		return false;
	}

	FCortexFrontendSettings::Get().ClearPendingChanges();
	UE_LOG(LogCortexFrontend, Log, TEXT("Reconnect() succeeded with session %s"), *Config.SessionId);
	return true;
}

void FCortexCliSession::NewChat()
{
	CancelGraceTimer();
	CleanupProcess();
	ClearConversation();
	Config.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	ConsecutiveSpawnFailures = 0;
	CurrentTurnIndex = 0;
	ConversationContextTokens.store(0);
	const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Inactive);
	BroadcastStateChange(PreviousState, ECortexSessionState::Inactive, TEXT("New chat"));
	OnTokenUsageUpdated.Broadcast();
	UE_LOG(LogCortexFrontend, Log, TEXT("New chat: session %s"), *Config.SessionId);
}

void FCortexCliSession::Shutdown()
{
	CancelGraceTimer();
	CleanupProcess();
	const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Terminated);
	BroadcastStateChange(PreviousState, ECortexSessionState::Terminated, TEXT("Shutdown"));
	UE_LOG(LogCortexFrontend, Log, TEXT("Session shutdown"));
}

void FCortexCliSession::HandleWorkerEvent(const FCortexStreamEvent& Event)
{
	// Accumulate token usage (before state guard — applies even during state transitions)
	if (Event.InputTokens > 0 || Event.OutputTokens > 0)
	{
		TotalInputTokens.fetch_add(Event.InputTokens);
		TotalOutputTokens.fetch_add(Event.OutputTokens);
		TotalCacheReadTokens.fetch_add(Event.CacheReadTokens);
		TotalCacheCreationTokens.fetch_add(Event.CacheCreationTokens);
		ConversationContextTokens.store(Event.InputTokens + Event.CacheReadTokens + Event.CacheCreationTokens);
		OnTokenUsageUpdated.Broadcast();
	}

	// Extract model info from init event
	if (Event.Type == ECortexStreamEventType::SessionInit && !Event.Model.IsEmpty())
	{
		ModelId = Event.Model;
		Provider = TEXT("Claude Code");
	}

	const ECortexSessionState CurrentState = State.load();

	// Ignore stale events after cleanup
	if (CurrentState == ECortexSessionState::Inactive || CurrentState == ECortexSessionState::Terminated)
	{
		return;
	}

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
		ToolEntry->TurnIndex = CurrentTurnIndex;
		ToolEntry->ToolStartTime = FPlatformTime::Seconds();
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
				if (ChatEntries[Index]->ToolStartTime > 0.0)
				{
					ChatEntries[Index]->DurationMs = static_cast<int32>((FPlatformTime::Seconds() - ChatEntries[Index]->ToolStartTime) * 1000.0);
				}
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

		ConsecutiveSpawnFailures = 0;

		if (CurrentState == ECortexSessionState::Cancelling)
		{
			// During cancel: capture result but don't transition to Idle.
			// The process will exit after this (stdin was closed), and
			// HandleProcessExited will handle the Cancelling -> Respawning transition.
			CancelGraceTimer();
			UE_LOG(LogCortexFrontend, Log, TEXT("Turn completed during cancel — awaiting process exit for respawn"));
			OnTurnComplete.Broadcast(Result);
		}
		else
		{
			const ECortexSessionState PreviousState = State.exchange(ECortexSessionState::Idle);
			BroadcastStateChange(PreviousState, ECortexSessionState::Idle, TEXT("Turn complete"));
			UE_LOG(LogCortexFrontend, Log, TEXT("Turn complete: %d turns, $%.4f"), Result.NumTurns, Result.TotalCostUsd);
			OnTurnComplete.Broadcast(Result);
		}
		return;
	}

	default:
		return;
	}
}

void FCortexCliSession::HandleProcessExited(const FString& Reason)
{
	const ECortexSessionState CurrentState = State.load();

	// Ignore stale process-exited events
	if (CurrentState == ECortexSessionState::Inactive || CurrentState == ECortexSessionState::Terminated)
	{
		return;
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("Process exited (state=%d): %s"), static_cast<int32>(CurrentState), *Reason);

	CancelGraceTimer();

	if (CurrentState == ECortexSessionState::Cancelling)
	{
		CleanupProcess();

		++ConsecutiveSpawnFailures;
		if (ConsecutiveSpawnFailures > MaxRespawnAttempts)
		{
			ConsecutiveSpawnFailures = 0;
			Config.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			UE_LOG(LogCortexFrontend, Warning, TEXT("Max respawn attempts exceeded, starting fresh session %s"), *Config.SessionId);
			State.store(ECortexSessionState::Inactive);
			BroadcastStateChange(CurrentState, ECortexSessionState::Inactive, TEXT("Session could not be resumed. Started fresh."));
			return;
		}

		State.store(ECortexSessionState::Respawning);
		BroadcastStateChange(CurrentState, ECortexSessionState::Respawning, Reason);

		if (!CachedCliInfo.bIsValid)
		{
			UE_LOG(LogCortexFrontend, Log, TEXT("Cannot respawn: Claude CLI not found"));
			State.store(ECortexSessionState::Inactive);
			BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive, TEXT("Claude CLI not found"));
			return;
		}

		UE_LOG(LogCortexFrontend, Log, TEXT("Respawning with --resume (attempt %d/%d)"), ConsecutiveSpawnFailures, MaxRespawnAttempts);
		if (!SpawnProcess(GetPendingAccessMode(), true))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("Respawn failed"));
			State.store(ECortexSessionState::Inactive);
			BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive, TEXT("Failed to respawn Claude CLI"));
			return;
		}
		return;
	}

	// Non-cancel process exit (unexpected crash)
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

	const FString& SelectedModel = FCortexFrontendSettings::Get().GetSelectedModel();
	if (!SelectedModel.IsEmpty() && SelectedModel != TEXT("Default"))
	{
		CommandLine += FString::Printf(TEXT("--model \"%s\" "), *SelectedModel);
	}

	// Effort flag (omitted when Default)
	const ECortexEffortLevel Effort = FCortexFrontendSettings::Get().GetEffortLevel();
	if (Effort != ECortexEffortLevel::Default)
	{
		CommandLine += FString::Printf(TEXT("--effort \"%s\" "),
			*FCortexFrontendSettings::Get().GetEffortLevelString());
	}

	// Workflow: Direct mode disables slash commands
	const ECortexWorkflowMode Workflow = FCortexFrontendSettings::Get().GetWorkflowMode();
	if (Workflow == ECortexWorkflowMode::Direct)
	{
		CommandLine += TEXT("--disable-slash-commands ");
	}

	// Project context off: restrict setting sources
	if (!FCortexFrontendSettings::Get().GetProjectContext())
	{
		CommandLine += TEXT("--setting-sources \"user,local\" ");
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

	FString SystemPrompt = FString::Printf(
		TEXT("You are running inside the Unreal Editor's Cortex AI Chat panel. "
			 "You have access to Cortex MCP tools for querying and manipulating the editor. "
			 "Current access mode: %s."), *ModeString);

	if (Workflow == ECortexWorkflowMode::Direct)
	{
		SystemPrompt += TEXT(" Workflow mode: Direct. Act immediately on requests using MCP tools. "
			"Do not create planning documents, design docs, spec files, or brainstorming files. "
			"Do not follow documentation-first workflows. Be concise. Prefer action over ceremony.");
	}

	const FString Directive = FCortexFrontendSettings::Get().GetCustomDirective();
	if (!Directive.IsEmpty())
	{
		FString Sanitized = Directive;
		Sanitized.ReplaceInline(TEXT("\n"), TEXT(" "));
		Sanitized.ReplaceInline(TEXT("\r"), TEXT(" "));
		Sanitized.ReplaceInline(TEXT("\t"), TEXT(" "));
		Sanitized.ReplaceInline(TEXT("$("), TEXT(""));
		Sanitized.ReplaceInline(TEXT("`"), TEXT(""));
		Sanitized.ReplaceInline(TEXT("%"), TEXT(""));
		Sanitized.ReplaceInline(TEXT("^"), TEXT(""));
		Sanitized.ReplaceInline(TEXT("|"), TEXT(""));
		Sanitized.ReplaceInline(TEXT("&"), TEXT(""));
		Sanitized.ReplaceInline(TEXT(">"), TEXT(""));
		Sanitized.ReplaceInline(TEXT("<"), TEXT(""));
		SystemPrompt += TEXT(" ") + Sanitized;
	}

	CommandLine += FString::Printf(TEXT("--append-system-prompt \"%s\" "),
		*SystemPrompt.Replace(TEXT("\""), TEXT("\\\"")));

	return CommandLine;
}

FString FCortexCliSession::BuildAllowedToolsArg(ECortexAccessMode AccessMode) const
{
	// Built-in Claude Code tools allowed per access mode.
	// --allowedTools is a whitelist: only listed tools are available to the LLM.
	// Without listing built-in tools, the LLM can bypass MCP restrictions via Edit/Write/Bash.
	static const TCHAR* ReadOnlyBuiltins = TEXT("Read,Glob,Grep,Agent,WebFetch,WebSearch,AskUserQuestion,TodoRead,TodoWrite");
	static const TCHAR* GuidedBuiltins = TEXT("Read,Edit,Write,Bash,Glob,Grep,Agent,WebFetch,WebSearch,NotebookEdit,AskUserQuestion,TodoRead,TodoWrite");

	switch (AccessMode)
	{
	case ECortexAccessMode::ReadOnly:
		return FString::Printf(TEXT("%s,"
			"mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*"),
			ReadOnlyBuiltins);
	case ECortexAccessMode::Guided:
		return FString::Printf(TEXT("%s,"
			"mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*,"
			"mcp__cortex_mcp__spawn_*,mcp__cortex_mcp__create_*,mcp__cortex_mcp__add_*,mcp__cortex_mcp__set_*,mcp__cortex_mcp__compile_*,mcp__cortex_mcp__connect_*,"
			"mcp__cortex_mcp__graph_add_*,mcp__cortex_mcp__graph_connect,mcp__cortex_mcp__graph_list_*,mcp__cortex_mcp__graph_get_*,mcp__cortex_mcp__graph_set_*,mcp__cortex_mcp__graph_search_*,mcp__cortex_mcp__graph_auto_layout,"
			"mcp__cortex_mcp__open_*,mcp__cortex_mcp__close_*,mcp__cortex_mcp__focus_*,mcp__cortex_mcp__select_*,"
			"mcp__cortex_mcp__rename_*,mcp__cortex_mcp__configure_*,mcp__cortex_mcp__import_*,mcp__cortex_mcp__update_*,mcp__cortex_mcp__duplicate_*,mcp__cortex_mcp__reparent*,mcp__cortex_mcp__attach_*,mcp__cortex_mcp__detach_*,mcp__cortex_mcp__register_*,mcp__cortex_mcp__reload_*"),
			GuidedBuiltins);
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
		UE_LOG(LogCortexFrontend, Error, TEXT("Claude CLI not found"));
		return false;
	}

	if (!FPlatformProcess::CreatePipe(StdoutReadPipe, StdoutWritePipe, false))
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create stdout pipe"));
		return false;
	}

	if (!FPlatformProcess::CreatePipe(StdinReadPipe, StdinWritePipe, true))
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create stdin pipe"));
		FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
		StdoutReadPipe = nullptr;
		StdoutWritePipe = nullptr;
		return false;
	}

	const FString WorkingDirectory = !Config.WorkingDirectory.IsEmpty()
		? Config.WorkingDirectory
		: FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString CommandLine = BuildLaunchCommandLine(bResumeSession, AccessMode);

	UE_LOG(LogCortexFrontend, Log, TEXT("Launching Claude CLI: %s %s"), *CachedCliInfo.Path, *CommandLine);

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
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create Claude CLI process"));
		CleanupProcess();
		return false;
	}

	if (PromptReadyEvent == nullptr)
	{
		PromptReadyEvent = FPlatformProcess::GetSynchEventFromPool(false);
	}

	// Pass handle copies to Worker — Worker uses its own copies, not Session's
	Worker = MakeUnique<FCortexCliWorker>(
		AsShared(),
		StdoutReadPipe,
		StdinWritePipe,
		ProcessHandle,
		PromptReadyEvent);

	// CAS: the compare_exchange_strong IS the atomicity guard against Cancel() races.
	// The pre-check below is an optimization to avoid the log noise of a failing CAS
	// when the state is clearly wrong — it does not close the race (the CAS does).
	ECortexSessionState ExpectedState = State.load();
	if (ExpectedState != ECortexSessionState::Spawning && ExpectedState != ECortexSessionState::Respawning)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("SpawnProcess: unexpected state %d before Idle transition, aborting"), static_cast<int32>(ExpectedState));
		CleanupProcess();
		return false;
	}
	if (!State.compare_exchange_strong(ExpectedState, ECortexSessionState::Idle))
	{
		// Cancel() raced and changed state between the load above and this CAS
		UE_LOG(LogCortexFrontend, Log, TEXT("SpawnProcess: lost CAS race (state now %d), aborting Idle transition"), static_cast<int32>(ExpectedState));
		CleanupProcess();
		return false;
	}
	BroadcastStateChange(ExpectedState, ECortexSessionState::Idle, TEXT("Claude CLI session ready"));
	UE_LOG(LogCortexFrontend, Log, TEXT("Claude CLI session ready (resume=%s)"), bResumeSession ? TEXT("true") : TEXT("false"));

	// Drain any prompt queued during Spawning. Uses TransitionState (CAS) so a concurrent
	// Cancel() between here and the CAS is handled safely.
	bool bHasPendingPrompt = false;
	{
		FScopeLock Lock(&PromptMutex);
		bHasPendingPrompt = PendingPrompt.IsSet();
	}
	if (bHasPendingPrompt)
	{
		if (TransitionState(ECortexSessionState::Idle, ECortexSessionState::Processing, TEXT("Draining queued prompt")))
		{
			WakeWorker();
		}
	}

	return true;
}

void FCortexCliSession::CleanupProcess()
{
	// 1. Signal Worker to stop (sets bStopRequested, triggers FEvent)
	if (Worker)
	{
		Worker->Stop();
	}

	// 2. Close stdin to signal graceful process exit
	CloseStdinPipe();

	// 3. Wait up to 2s for process to exit, then force-kill
	if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		const double WaitStart = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(ProcessHandle) && (FPlatformTime::Seconds() - WaitStart) < 2.0)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			UE_LOG(LogCortexFrontend, Log, TEXT("Process did not exit within grace period, force-killing"));
			FPlatformProcess::TerminateProc(ProcessHandle, true);
		}
	}

	// 4. Join Worker thread (safe: process is dead, Worker will exit quickly)
	if (Worker)
	{
		Worker.Reset();
	}

	// 5. Close stdout pipe (safe: Worker is joined)
	if (StdoutReadPipe != nullptr || StdoutWritePipe != nullptr)
	{
		FPlatformProcess::ClosePipe(StdoutReadPipe, StdoutWritePipe);
		StdoutReadPipe = nullptr;
		StdoutWritePipe = nullptr;
	}

	// 6. Close process handle
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
		ProcessHandle.Reset();
	}

	// 7. Return event to pool
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
	++CurrentTurnIndex;

	TSharedPtr<FCortexChatEntry> UserEntry = MakeShared<FCortexChatEntry>();
	UserEntry->Type = ECortexChatEntryType::UserMessage;
	UserEntry->Text = Message;
	UserEntry->TurnIndex = CurrentTurnIndex;
	ChatEntries.Add(UserEntry);

	CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
	CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
	CurrentStreamingEntry->TurnIndex = CurrentTurnIndex;
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
		CurrentStreamingEntry->TurnIndex = CurrentTurnIndex;
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

	// Propagate TurnIndex from streaming entry to replacement entries
	const int32 TurnIndex = CurrentStreamingEntry->TurnIndex;
	for (const TSharedPtr<FCortexChatEntry>& Entry : ReplacementEntries)
	{
		Entry->TurnIndex = TurnIndex;
	}

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

	UE_LOG(LogCortexFrontend, Verbose, TEXT("State: %d -> %d (%s)"), static_cast<int32>(ExpectedState), static_cast<int32>(NewState), *Reason);
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

void FCortexCliSession::CancelGraceTimer()
{
	if (GraceTimerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GraceTimerHandle);
		GraceTimerHandle.Reset();
	}
}

void FCortexCliSession::ForceKillProcess()
{
	if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
	}

	HandleProcessExited(TEXT("Cancel grace period expired, process force-killed"));
}

void FCortexCliSession::CloseStdinPipe()
{
	if (StdinReadPipe != nullptr || StdinWritePipe != nullptr)
	{
		FPlatformProcess::ClosePipe(StdinReadPipe, StdinWritePipe);
		StdinReadPipe = nullptr;
		StdinWritePipe = nullptr;
	}
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

void FCortexCliSession::SetPendingPromptForTest(const FString& Prompt)
{
	FScopeLock Lock(&PromptMutex);
	PendingPrompt = Prompt;
}

void FCortexCliSession::DrainPendingPromptForTest()
{
	// Tests the Idle -> Processing drain transition in isolation.
	// Does not test SpawnProcess directly (WakeWorker is not mockable).
	bool bHasPendingPrompt = false;
	{
		FScopeLock Lock(&PromptMutex);
		bHasPendingPrompt = PendingPrompt.IsSet();
	}
	if (bHasPendingPrompt)
	{
		State.store(ECortexSessionState::Idle);
		TransitionState(ECortexSessionState::Idle, ECortexSessionState::Processing, TEXT("Draining queued prompt (test)"));
	}
}

TSharedPtr<FCortexChatEntry> FCortexCliSession::GetCurrentStreamingEntry() const
{
	return CurrentStreamingEntry;
}
