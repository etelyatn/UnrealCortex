// Source/CortexFrontend/Private/QA/CortexQASessionManager.h
#pragma once

#include "CoreMinimal.h"
#include "QA/CortexQATabTypes.h"

/**
 * Manages QA session files for the frontend tab.
 * Handles listing, loading, deleting, and refreshing sessions from disk.
 */
class FCortexQASessionManager
{
public:
    explicit FCortexQASessionManager(const FString& InRecordingsDir);

    /** Reload session list from disk. */
    void RefreshSessionList();

    /** Get current session list. */
    const TArray<FCortexQASessionListItem>& GetSessions() const { return Sessions; }

    /** Delete session at index. */
    bool DeleteSession(int32 Index);

    /** Load the step list for a session by index. Returns empty array on failure. */
    TArray<struct FCortexQADetailStep> LoadSteps(int32 Index) const;

    /** Get the recordings directory. */
    const FString& GetRecordingsDir() const { return RecordingsDir; }

private:
    FString RecordingsDir;
    TArray<FCortexQASessionListItem> Sessions;
};
