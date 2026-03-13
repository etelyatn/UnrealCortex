#pragma once

#include "CoreMinimal.h"
#include "Process/CortexCliDiscovery.h"
#include "Process/CortexStreamEvent.h"
#include "Session/CortexSessionTypes.h"

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
    FString GetSessionId() const;
    ECortexSessionState GetState() const;
    void AddUserPromptEntry(const FString& Message);
    void UpdateStreamingAssistantText(const FString& Text, bool bAppend);
    void ReplaceStreamingEntry(const TArray<TSharedPtr<FCortexChatEntry>>& ReplacementEntries);
    void ClearConversation();

    FOnCortexSessionStreamEvent OnStreamEvent;
    FOnCortexSessionTurnComplete OnTurnComplete;
    FOnCortexSessionStateChanged OnStateChanged;

private:
    friend class FCortexCliSessionBuildInitialLaunchArgsTest;
    friend class FCortexCliSessionBuildResumeLaunchArgsTest;
    friend class FCortexCliSessionBuildPromptEnvelopeTest;
    friend class FCortexCliSessionQueuePromptWhileSpawningTest;
    friend class FCortexCliSessionTurnCompleteReturnsIdleTest;
    friend class FCortexCliSessionCancelTransitionsTest;

    FString BuildLaunchCommandLine(bool bResumeSession, ECortexAccessMode AccessMode) const;
    FString BuildAllowedToolsArg(ECortexAccessMode AccessMode) const;
    FString BuildPromptEnvelope(const FString& Prompt) const;
    bool TransitionState(ECortexSessionState ExpectedState, ECortexSessionState NewState, const FString& Reason = FString());
    void BroadcastStateChange(ECortexSessionState PreviousState, ECortexSessionState NewState, const FString& Reason);
    ECortexSessionState GetStateForTest() const;
    void SetStateForTest(ECortexSessionState NewState);
    FString GetPendingPromptForTest() const;
    TSharedPtr<FCortexChatEntry> GetCurrentStreamingEntry() const;

    FCortexSessionConfig Config;
    FCortexCliInfo CachedCliInfo;
    std::atomic<ECortexSessionState> State;
    TOptional<FString> PendingPrompt;
    TOptional<ECortexAccessMode> PendingAccessMode;
    TUniquePtr<FCortexCliWorker> Worker;
    TArray<TSharedPtr<FCortexChatEntry>> ChatEntries;
    TSharedPtr<FCortexChatEntry> CurrentStreamingEntry;
};
