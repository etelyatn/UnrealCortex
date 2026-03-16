// Source/CortexFrontend/Private/QA/CortexQASessionManager.cpp
#include "QA/CortexQASessionManager.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FCortexQASessionManager::FCortexQASessionManager(const FString& InRecordingsDir)
    : RecordingsDir(InRecordingsDir)
{
}

void FCortexQASessionManager::RefreshSessionList()
{
    Sessions.Empty();

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(RecordingsDir / TEXT("*.json")), true, false);

    for (const FString& File : Files)
    {
        const FString FullPath = RecordingsDir / File;
        FString JsonStr;
        if (!FFileHelper::LoadFileToString(JsonStr, *FullPath))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Root;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            continue;
        }

        FCortexQASessionListItem Item;
        Item.FilePath = FullPath;
        Item.Name = Root->GetStringField(TEXT("name"));
        Item.Source = Root->GetStringField(TEXT("source"));
        Item.MapPath = Root->GetStringField(TEXT("map"));
        FDateTime::ParseIso8601(*Root->GetStringField(TEXT("recorded_at")), Item.RecordedAt);
        Item.bComplete = Root->GetBoolField(TEXT("complete"));

        const TArray<TSharedPtr<FJsonValue>>& StepsArr = Root->GetArrayField(TEXT("steps"));
        Item.StepCount = StepsArr.Num();

        const TSharedPtr<FJsonObject>* LastRunObj = nullptr;
        if (Root->TryGetObjectField(TEXT("last_run"), LastRunObj))
        {
            Item.bHasBeenRun = true;
            Item.bLastRunPassed = (*LastRunObj)->GetBoolField(TEXT("passed"));
        }

        Sessions.Add(MoveTemp(Item));
    }

    // Sort by date descending (newest first)
    Sessions.Sort([](const FCortexQASessionListItem& A, const FCortexQASessionListItem& B)
    {
        return A.RecordedAt > B.RecordedAt;
    });
}

bool FCortexQASessionManager::DeleteSession(int32 Index)
{
    if (!Sessions.IsValidIndex(Index))
    {
        return false;
    }

    const FString FilePath = Sessions[Index].FilePath;
    if (IFileManager::Get().Delete(*FilePath))
    {
        Sessions.RemoveAt(Index);
        return true;
    }

    return false;
}
