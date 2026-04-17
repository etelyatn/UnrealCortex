#include "Session/CortexCliSession.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"
#include "Providers/CortexClaudeCliProvider.h"
#include "Providers/CortexCodexCliProvider.h"
#include "Providers/CortexProviderRegistry.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Session/CortexCliWorker.h"

namespace
{
	constexpr uint8 MaxRespawnAttempts = 2;
	constexpr float GracePeriodSeconds = 2.0f;

	const ICortexCliProvider& GetPinnedProvider(const FName& ProviderId)
	{
		static FCortexClaudeCliProvider ClaudeProvider;
		static FCortexCodexCliProvider CodexProvider;

		if (ProviderId == FName(TEXT("codex")))
		{
			return CodexProvider;
		}

		return ClaudeProvider;
	}

	FCortexResolvedLaunchOptions SnapshotLaunchOptions()
	{
		FCortexResolvedLaunchOptions LaunchOptions;
		LaunchOptions.AccessMode = FCortexFrontendSettings::Get().GetAccessMode();
		LaunchOptions.bSkipPermissions = FCortexFrontendSettings::Get().GetSkipPermissions();
		LaunchOptions.WorkflowMode = FCortexFrontendSettings::Get().GetWorkflowMode();
		LaunchOptions.bProjectContext = FCortexFrontendSettings::Get().GetProjectContext();
		LaunchOptions.bAutoContext = FCortexFrontendSettings::Get().GetAutoContext();
		LaunchOptions.CustomDirective = FCortexFrontendSettings::Get().GetCustomDirective();
		return LaunchOptions;
	}

	FCortexResolvedSessionOptions SnapshotResolvedOptions(const FName& ProviderId, const FCortexSessionConfig& Config)
	{
		const FCortexProviderDefinition& ProviderDefinition = FCortexProviderRegistry::ResolveDefinition(ProviderId.ToString());
		const FString EffectiveModelId = !Config.ModelId.IsEmpty() ? Config.ModelId : FCortexFrontendSettings::Get().GetSelectedModel();
		const FCortexProviderModelDefinition& ModelDefinition = FCortexProviderRegistry::ValidateOrGetDefaultModel(
			ProviderDefinition,
			EffectiveModelId);

		FCortexResolvedSessionOptions ResolvedOptions;
		ResolvedOptions.ProviderId = ProviderDefinition.ProviderId;
		ResolvedOptions.ProviderDisplayName = ProviderDefinition.DisplayName;
		ResolvedOptions.ModelId = ModelDefinition.ModelId;
		ResolvedOptions.EffortLevel = FCortexProviderRegistry::ValidateOrGetDefaultEffort(
			ProviderDefinition,
			ModelDefinition,
			Config.EffortLevel);
		ResolvedOptions.ContextLimitTokens = FCortexProviderRegistry::GetContextLimit(ProviderDefinition, ResolvedOptions.ModelId);
		return ResolvedOptions;
	}
}

FCortexCliSession::FCortexCliSession(const FCortexSessionConfig& InConfig)
	: Config(InConfig)
	, State(ECortexSessionState::Inactive)
{
	if (Config.ProviderId.IsNone())
	{
		const UCortexFrontendProviderSettings* ProviderSettings = UCortexFrontendProviderSettings::Get();
		const FString ProviderIdString = ProviderSettings != nullptr
			? ProviderSettings->GetEffectiveProviderId()
			: FCortexProviderRegistry::GetDefaultProviderId();
		Config.ProviderId = FName(*ProviderIdString);
		if (!Config.bHasLaunchOptions)
		{
			Config.LaunchOptions = SnapshotLaunchOptions();
		}
		Config.ResolvedOptions = SnapshotResolvedOptions(Config.ProviderId, Config);
	}
	else
	{
		Config.ProviderId = FCortexProviderRegistry::ResolveDefinition(Config.ProviderId.ToString()).ProviderId;
		if (Config.ResolvedOptions.ProviderId.IsNone())
		{
			Config.ResolvedOptions = SnapshotResolvedOptions(Config.ProviderId, Config);
		}
	}

	if (Config.ResolvedOptions.ProviderId.IsNone())
	{
		Config.ResolvedOptions = SnapshotResolvedOptions(Config.ProviderId, Config);
	}

	if (!Config.bHasLaunchOptions)
	{
		Config.LaunchOptions = SnapshotLaunchOptions();
		if (Config.bSkipPermissions)
		{
			Config.LaunchOptions.bSkipPermissions = true;
		}
	}

	PinnedProvider = &GetPinnedProvider(Config.ProviderId);
	ModelId = Config.ResolvedOptions.ModelId;
	Provider = Config.ResolvedOptions.ProviderDisplayName;
}

FCortexCliSession::~FCortexCliSession() = default;

bool FCortexCliSession::Connect()
{
	UE_LOG(LogCortexFrontend, Log, TEXT("Connect() called, current state: %d"), static_cast<int32>(State.load()));

	if (!TransitionState(ECortexSessionState::Inactive, ECortexSessionState::Spawning, TEXT("Auto-connect")))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Connect() failed: state was not Inactive (was %d)"), static_cast<int32>(State.load()));
		return false;
	}

	if (!SpawnProcess(Config.LaunchOptions.AccessMode, false))
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
		if (!TransitionState(ECortexSessionState::Inactive, ECortexSessionState::Spawning, TEXT("Spawning persistent provider session")))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("SendPrompt: failed to transition Inactive -> Spawning (state race)"));
			return false;
		}
		if (!SpawnProcess(Request.AccessMode, false))
		{
			TransitionState(ECortexSessionState::Spawning, ECortexSessionState::Inactive, TEXT("Failed to spawn provider"));
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

	if (PinnedProvider != nullptr &&
		PinnedProvider->GetTransportMode() == ECortexCliTransportMode::PerTurnExec &&
		(!ProcessHandle.IsValid() || !FPlatformProcess::IsProcRunning(ProcessHandle)))
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Per-turn exec provider idle without a running process, resuming session"));
		if (!TransitionState(ECortexSessionState::Idle, ECortexSessionState::Spawning, TEXT("Resuming per-turn exec conversation")))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("SendPrompt: failed to transition Idle -> Spawning for per-turn exec resume"));
			return false;
		}
		if (!SpawnProcess(Request.AccessMode, true))
		{
			TransitionState(ECortexSessionState::Spawning, ECortexSessionState::Inactive, TEXT("Failed to resume provider"));
			return false;
		}

		return true;
	}

	// Access mode changed since last spawn — reconnect to apply new --allowedTools
	if (LastSpawnedAccessMode.IsSet() && LastSpawnedAccessMode.GetValue() != Request.AccessMode)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Access mode changed (%d -> %d), reconnecting to apply new permissions"),
			static_cast<int32>(LastSpawnedAccessMode.GetValue()), static_cast<int32>(Request.AccessMode));
		Config.LaunchOptions.AccessMode = Request.AccessMode;
		if (!Reconnect())
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("SendPrompt: reconnect for mode change failed"));
			return false;
		}
		// After reconnect, state is Idle again — prompt is still pending, will be drained by worker
		return true;
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
	if (PinnedProvider != nullptr && !PinnedProvider->SupportsResume())
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Reconnect() failed: provider does not support resume"));
		State.store(ECortexSessionState::Inactive);
		BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive, TEXT("Provider does not support resume"));
		return false;
	}

	const ECortexAccessMode AccessMode = Config.LaunchOptions.AccessMode;
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

	// Extract model / session info from init event.
	if (Event.Type == ECortexStreamEventType::SessionInit)
	{
		if (!Event.SessionId.IsEmpty())
		{
			Config.SessionId = Event.SessionId;
		}

		if (!Event.Model.IsEmpty())
		{
			ModelId = Event.Model;
		}

		if (Provider.IsEmpty())
		{
			Provider = Config.ResolvedOptions.ProviderDisplayName;
		}
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

		if (PinnedProvider != nullptr &&
			PinnedProvider->GetTransportMode() == ECortexCliTransportMode::PerTurnExec)
		{
			bHasResumableProviderConversation = true;
		}

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
			UE_LOG(LogCortexFrontend, Log, TEXT("Cannot respawn: provider CLI not found"));
			State.store(ECortexSessionState::Inactive);
			BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive, TEXT("Provider CLI not found"));
			return;
		}

		UE_LOG(LogCortexFrontend, Log, TEXT("Respawning with --resume (attempt %d/%d)"), ConsecutiveSpawnFailures, MaxRespawnAttempts);
		if (!SpawnProcess(GetPendingAccessMode(), true))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("Respawn failed"));
			State.store(ECortexSessionState::Inactive);
			BroadcastStateChange(ECortexSessionState::Respawning, ECortexSessionState::Inactive, TEXT("Failed to respawn provider"));
			return;
		}
		return;
	}

	// Non-cancel process exit (unexpected crash)
	const bool bKeepResumableConversation =
		PinnedProvider != nullptr &&
		PinnedProvider->GetTransportMode() == ECortexCliTransportMode::PerTurnExec &&
		bHasResumableProviderConversation;
	CleanupProcess();
	if (bKeepResumableConversation)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("Provider CLI exited after a resumable turn; preserving idle state"));
		return;
	}
	BroadcastStateChange(CurrentState, ECortexSessionState::Inactive, Reason);
	State.store(ECortexSessionState::Inactive);
}

FString FCortexCliSession::BuildLaunchCommandLine(bool bResumeSession, ECortexAccessMode AccessMode) const
{
	const ICortexCliProvider* PinnedProviderToUse = PinnedProvider != nullptr ? PinnedProvider : &GetPinnedProvider(Config.ProviderId);
	return PinnedProviderToUse != nullptr
		? PinnedProviderToUse->BuildLaunchCommandLine(bResumeSession, AccessMode, Config)
		: FString();
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

	if (bResumeSession && PinnedProvider != nullptr && !PinnedProvider->SupportsResume())
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("SpawnProcess: provider does not support resume"));
		return false;
	}

	CachedCliInfo = PinnedProvider != nullptr ? PinnedProvider->FindCli() : FCortexCliDiscovery::FindClaude();
	if (!CachedCliInfo.bIsValid)
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("%s CLI not found"), PinnedProvider != nullptr ? *PinnedProvider->GetDefinition().ExecutableDisplayName : TEXT("Provider"));
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
	Config.LaunchOptions.AccessMode = AccessMode;
	bHasResumableProviderConversation = false;
	const FString CommandLine = BuildLaunchCommandLine(bResumeSession, AccessMode);

	UE_LOG(LogCortexFrontend, Log, TEXT("Launching %s: %s %s"),
		PinnedProvider != nullptr ? *PinnedProvider->GetDefinition().ExecutableDisplayName : TEXT("provider"),
		*CachedCliInfo.Path,
		*CommandLine);

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
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to create provider process"));
		CleanupProcess();
		return false;
	}

	if (PromptReadyEvent == nullptr)
	{
		PromptReadyEvent = FPlatformProcess::GetSynchEventFromPool(false);
	}

	// Pass handle copies and pinned provider to Worker — Worker uses its own copies, not Session's
	Worker = MakeUnique<FCortexCliWorker>(
		AsShared(),
		PinnedProvider,
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
	LastSpawnedAccessMode = AccessMode;
	BroadcastStateChange(ExpectedState, ECortexSessionState::Idle, TEXT("Provider session ready"));
	UE_LOG(LogCortexFrontend, Log, TEXT("Provider session ready (resume=%s)"), bResumeSession ? TEXT("true") : TEXT("false"));

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

	// 2. Force-kill the process FIRST.
	//    This is critical: the worker thread may be blocked in WritePipe/ReadPipe.
	//    Killing the process invalidates all pipe handles, causing blocked pipe
	//    operations to fail immediately so the worker thread can exit.
	if (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		if (Config.bConversionMode)
		{
			// Conversion: kill immediately — no grace period
			FPlatformProcess::TerminateProc(ProcessHandle, true);
		}
		else
		{
			// Chat: close stdin for graceful exit, then wait up to 2s
			CloseStdinPipe();
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
	}

	// 3. Close stdin pipe (may already be closed for chat sessions)
	CloseStdinPipe();

	// 4. Join Worker thread (safe: process is dead, pipe ops fail immediately)
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

FString FCortexCliSession::GetAuthCommandText() const
{
	return PinnedProvider != nullptr ? PinnedProvider->BuildAuthCommand() : FString();
}

FName FCortexCliSession::GetProviderId() const
{
	return Config.ProviderId;
}

const FCortexResolvedSessionOptions& FCortexCliSession::GetResolvedOptions() const
{
	return Config.ResolvedOptions;
}

int64 FCortexCliSession::GetContextLimitTokens() const
{
	return Config.ResolvedOptions.ContextLimitTokens;
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
