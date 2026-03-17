// Source/CortexQA/Private/Recording/CortexQASessionSerializer.h
#pragma once

#include "CoreMinimal.h"
#include "Recording/CortexQASessionTypes.h"

/**
 * JSON serializer for QA session files.
 * Handles save/load/list of session recordings in Saved/CortexQA/Recordings/.
 */
class FCortexQASessionSerializer
{
public:
    static bool SaveSession(
        const FCortexQASessionInfo& Session,
        const FString& Directory,
        FString& OutPath);

    static bool LoadSession(
        const FString& FilePath,
        FCortexQASessionInfo& OutSession);

    static void ListSessions(
        const FString& Directory,
        TArray<FCortexQASessionInfo>& OutSessions);

    static bool DeleteSession(const FString& FilePath);

    static bool UpdateLastRun(
        const FString& FilePath,
        const FCortexQALastRun& LastRun);

    static FString GetDefaultRecordingsDir();
};
