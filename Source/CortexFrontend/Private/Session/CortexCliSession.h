#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "Process/CortexCliDiscovery.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexSessionTypes.h"
#include <atomic>

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexSessionStreamEvent, const FCortexStreamEvent&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexSessionTurnComplete, const FCortexTurnResult&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexSessionStateChanged, const FCortexSessionStateChange&);

class FCortexCliWorker;
class ICortexCliProvider;

class FCortexCliSession : public TSharedFromThis<FCortexCliSession>
{
public:
	explicit FCortexCliSession(const FCortexSessionConfig& InConfig);
	~FCortexCliSession();

	bool Connect();
	bool SendPrompt(const FCortexPromptRequest& Request);
	bool Cancel();
	bool Reconnect();
	void NewChat();
	void Shutdown();

	void HandleWorkerEvent(const FCortexStreamEvent& Event);
	void HandleProcessExited(const FString& Reason);

	const TArray<TSharedPtr<FCortexChatEntry>>& GetChatEntries() const;
#if WITH_DEV_AUTOMATION_TESTS
	TArray<TSharedPtr<FCortexChatEntry>>& GetChatEntriesMutable() { return ChatEntries; }
#endif
	TSharedPtr<FCortexChatEntry> GetCurrentStreamingEntry() const;
	FString GetSessionId() const;
	ECortexSessionState GetState() const;
	void AddUserPromptEntry(const FString& Message);
	void RollbackLastPromptEntries();
	void UpdateStreamingAssistantText(const FString& Text, bool bAppend);
	void ReplaceStreamingEntry(const TArray<TSharedPtr<FCortexChatEntry>>& ReplacementEntries);
	void ClearConversation();

	FOnCortexSessionStreamEvent OnStreamEvent;
	FOnCortexSessionTurnComplete OnTurnComplete;
	FOnCortexSessionStateChanged OnStateChanged;

	DECLARE_MULTICAST_DELEGATE(FOnTokenUsageUpdated);
	FOnTokenUsageUpdated OnTokenUsageUpdated;

	int64 GetTotalInputTokens() const { return TotalInputTokens.load(); }
	int64 GetTotalOutputTokens() const { return TotalOutputTokens.load(); }
	int64 GetTotalCacheReadTokens() const { return TotalCacheReadTokens.load(); }
	int64 GetTotalCacheCreationTokens() const { return TotalCacheCreationTokens.load(); }
	int64 GetConversationContextTokens() const { return ConversationContextTokens.load(); }
	FName GetProviderId() const;
	const FCortexResolvedSessionOptions& GetResolvedOptions() const;
	FString GetAuthCommandText() const;
	int64 GetContextLimitTokens() const;
	FString GetModelId() const { return ModelId; }
	FString GetProvider() const { return Provider; }

	static float CalculateCacheHitRate(int64 CacheRead, int64 Input)
	{
		const int64 Total = CacheRead + Input;
		return (Total > 0) ? static_cast<float>(CacheRead) / static_cast<float>(Total) * 100.0f : 0.0f;
	}

private:
	friend class FCortexCliWorker;
	friend class FCortexCliSessionBuildInitialLaunchArgsTest;
	friend class FCortexCliSessionBuildResumeLaunchArgsTest;
	friend class FCortexCliSessionBuildPromptEnvelopeTest;
	friend class FCortexCliSessionBuildCodexExecArgsTest;
	friend class FCortexCliSessionLaunchOptionsPinnedAcrossSettingChangeTest;
	friend class FCortexCliSessionDefaultLaunchPinsLiveSkipPermissionsTest;
	friend class FCortexCliSessionCodexTurnExitPreservesResumableIdleStateTest;
	friend class FCortexCliSessionCodexOverridePathRecomputesResolvedOptionsTest;
	friend class FCortexCliSessionQueuePromptWhileSpawningTest;
	friend class FCortexCliSessionTurnCompleteReturnsIdleTest;
	friend class FCortexCliSessionCancelTransitionsTest;
	friend class FCortexCliSessionNewChatGeneratesFreshSessionIdTest;
	friend class FCortexChatPanelRejectedSendDoesNotAppendEntriesTest;
	friend class FCortexCliSessionToolCallTurnIndexTest;
	friend class FCortexCliSessionConnectTest;
	friend class FCortexCliSessionPendingPromptDrainedAfterSpawnTest;
	friend class FCortexCliSessionModelFlagTest;
	friend class FCortexCmdLineEffortDefaultTest;
	friend class FCortexCmdLineEffortMediumTest;
	friend class FCortexCmdLineWorkflowDirectTest;
	friend class FCortexCmdLineWorkflowThoroughTest;
	friend class FCortexCmdLineProjectContextOffTest;
	friend class FCortexCmdLineProjectContextOnTest;
	friend class FCortexCmdLineDirectiveTest;
	friend class FCortexCmdLineDirectiveEmptyTest;
	friend class FCortexCmdLineDirectiveSanitizationTest;
	friend class FCortexReconnectRejectsNonIdleTest;
	friend class FCortexReconnectFromIdleTransitionsTest;
	friend class FCortexReconnectDirtyStatePreservedOnFailureTest;
	friend class FCortexReconnectPinsLaunchMetadataAcrossSettingsChangeTest;

	FString BuildLaunchCommandLine(bool bResumeSession, ECortexAccessMode AccessMode) const;
	FString BuildAllowedToolsArg(ECortexAccessMode AccessMode) const;
	FString BuildPromptEnvelope(const FString& Prompt) const;
	bool SpawnProcess(ECortexAccessMode AccessMode, bool bResumeSession);
	void CleanupProcess();
	void WakeWorker();
	FString ConsumePendingPromptEnvelope();
	ECortexAccessMode GetPendingAccessMode() const;
	bool TransitionState(ECortexSessionState ExpectedState, ECortexSessionState NewState, const FString& Reason = FString());
	void BroadcastStateChange(ECortexSessionState PreviousState, ECortexSessionState NewState, const FString& Reason);
	void CancelGraceTimer();
	void ForceKillProcess();
	void CloseStdinPipe();
	ECortexSessionState GetStateForTest() const;
	void SetStateForTest(ECortexSessionState NewState);
	FString GetPendingPromptForTest() const;
	void SetPendingPromptForTest(const FString& Prompt);  // mutex-safe
	void DrainPendingPromptForTest();

	// Session-scoped token accumulators (survive conversation resets)
	// Atomic: written on worker thread, read on Game Thread for display
	std::atomic<int64> TotalInputTokens{0};
	std::atomic<int64> TotalOutputTokens{0};
	std::atomic<int64> TotalCacheReadTokens{0};
	std::atomic<int64> TotalCacheCreationTokens{0};

	// Per-conversation context tracking (reset on NewChat)
	// Atomic: written on worker thread, read on Game Thread for display
	std::atomic<int64> ConversationContextTokens{0};

	// Model info (set once from system.init)
	FString ModelId;
	FString Provider;

	// Turn counter (incremented per user prompt, reset on NewChat)
	int32 CurrentTurnIndex = 0;

	FCortexSessionConfig Config;
	const ICortexCliProvider* PinnedProvider = nullptr;
	FCortexCliInfo CachedCliInfo;
	bool bHasResumableProviderConversation = false;
	std::atomic<ECortexSessionState> State;
	TOptional<FString> PendingPrompt;
	TOptional<ECortexAccessMode> PendingAccessMode;
	TOptional<ECortexAccessMode> LastSpawnedAccessMode;
	TUniquePtr<FCortexCliWorker> Worker;
	FProcHandle ProcessHandle;
	void* StdoutReadPipe = nullptr;
	void* StdoutWritePipe = nullptr;
	void* StdinReadPipe = nullptr;
	void* StdinWritePipe = nullptr;
	FEvent* PromptReadyEvent = nullptr;
	mutable FCriticalSection PromptMutex;
	TArray<TSharedPtr<FCortexChatEntry>> ChatEntries;
	TSharedPtr<FCortexChatEntry> CurrentStreamingEntry;
	uint32 CancelGeneration = 0;
	uint8 ConsecutiveSpawnFailures = 0;
	FTSTicker::FDelegateHandle GraceTimerHandle;
};
