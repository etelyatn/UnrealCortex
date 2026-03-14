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

class FCortexCliSession : public TSharedFromThis<FCortexCliSession>
{
public:
	explicit FCortexCliSession(const FCortexSessionConfig& InConfig);

	bool SendPrompt(const FCortexPromptRequest& Request);
	bool Cancel();
	void NewChat();
	void Shutdown();

	void HandleWorkerEvent(const FCortexStreamEvent& Event);
	void HandleProcessExited(const FString& Reason);

	const TArray<TSharedPtr<FCortexChatEntry>>& GetChatEntries() const;
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

	int64 GetTotalInputTokens() const { return TotalInputTokens; }
	int64 GetTotalOutputTokens() const { return TotalOutputTokens; }
	int64 GetTotalCacheReadTokens() const { return TotalCacheReadTokens; }
	int64 GetTotalCacheCreationTokens() const { return TotalCacheCreationTokens; }
	int64 GetConversationContextTokens() const { return ConversationContextTokens; }
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
	friend class FCortexCliSessionQueuePromptWhileSpawningTest;
	friend class FCortexCliSessionTurnCompleteReturnsIdleTest;
	friend class FCortexCliSessionCancelTransitionsTest;
	friend class FCortexCliSessionNewChatGeneratesFreshSessionIdTest;
	friend class FCortexChatPanelRejectedSendDoesNotAppendEntriesTest;
	friend class FCortexCliSessionToolCallTurnIndexTest;

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

	// Session-scoped token accumulators (survive conversation resets)
	int64 TotalInputTokens = 0;
	int64 TotalOutputTokens = 0;
	int64 TotalCacheReadTokens = 0;
	int64 TotalCacheCreationTokens = 0;

	// Per-conversation context tracking (reset on NewChat)
	int64 ConversationContextTokens = 0;

	// Model info (set once from system.init)
	FString ModelId;
	FString Provider;

	// Turn counter (incremented per user prompt, reset on NewChat)
	int32 CurrentTurnIndex = 0;

	FCortexSessionConfig Config;
	FCortexCliInfo CachedCliInfo;
	std::atomic<ECortexSessionState> State;
	TOptional<FString> PendingPrompt;
	TOptional<ECortexAccessMode> PendingAccessMode;
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
