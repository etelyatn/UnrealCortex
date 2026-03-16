// Source/CortexQA/Private/Recording/CortexQASessionSerializer.cpp
#include "Recording/CortexQASessionSerializer.h"
#include "CortexQAModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    TSharedPtr<FJsonObject> StepToJson(const FCortexQAStep& Step)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("type"), Step.Type);
        Obj->SetNumberField(TEXT("timestamp_ms"), Step.TimestampMs);
        if (Step.Params.IsValid())
        {
            Obj->SetObjectField(TEXT("params"), Step.Params);
        }
        return Obj;
    }

    FCortexQAStep JsonToStep(const TSharedPtr<FJsonObject>& Obj)
    {
        FCortexQAStep Step;
        Step.Type = Obj->GetStringField(TEXT("type"));
        Step.TimestampMs = Obj->GetNumberField(TEXT("timestamp_ms"));
        Step.Params = Obj->GetObjectField(TEXT("params"));
        return Step;
    }

    TSharedPtr<FJsonObject> RawInputToJson(const FCortexQARawInputEvent& Evt)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("timestamp_ms"), Evt.TimestampMs);
        Obj->SetStringField(TEXT("type"), Evt.Type);
        if (!Evt.Key.IsEmpty())
        {
            Obj->SetStringField(TEXT("key"), Evt.Key);
        }
        if (Evt.Type == TEXT("mouse_move"))
        {
            Obj->SetNumberField(TEXT("dx"), Evt.DeltaX);
            Obj->SetNumberField(TEXT("dy"), Evt.DeltaY);
        }
        Obj->SetNumberField(TEXT("modifiers"), static_cast<double>(Evt.Modifiers));
        return Obj;
    }

    FCortexQARawInputEvent JsonToRawInput(const TSharedPtr<FJsonObject>& Obj)
    {
        FCortexQARawInputEvent Evt;
        Evt.TimestampMs = Obj->GetNumberField(TEXT("timestamp_ms"));
        Evt.Type = Obj->GetStringField(TEXT("type"));
        Obj->TryGetStringField(TEXT("key"), Evt.Key);
        double Dx = 0.0, Dy = 0.0;
        Obj->TryGetNumberField(TEXT("dx"), Dx);
        Obj->TryGetNumberField(TEXT("dy"), Dy);
        Evt.DeltaX = static_cast<float>(Dx);
        Evt.DeltaY = static_cast<float>(Dy);
        Evt.Modifiers = static_cast<uint32>(Obj->GetNumberField(TEXT("modifiers")));
        return Evt;
    }

    TSharedPtr<FJsonObject> LastRunToJson(const FCortexQALastRun& Run)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("timestamp"), Run.Timestamp.ToIso8601());
        Obj->SetBoolField(TEXT("passed"), Run.bPassed);
        Obj->SetNumberField(TEXT("steps_passed"), Run.StepsPassed);
        Obj->SetNumberField(TEXT("steps_failed"), Run.StepsFailed);
        Obj->SetNumberField(TEXT("duration_seconds"), Run.DurationSeconds);
        return Obj;
    }

    FCortexQALastRun JsonToLastRun(const TSharedPtr<FJsonObject>& Obj)
    {
        FCortexQALastRun Run;
        FDateTime::ParseIso8601(*Obj->GetStringField(TEXT("timestamp")), Run.Timestamp);
        Run.bPassed = Obj->GetBoolField(TEXT("passed"));
        Run.StepsPassed = static_cast<int32>(Obj->GetNumberField(TEXT("steps_passed")));
        Run.StepsFailed = static_cast<int32>(Obj->GetNumberField(TEXT("steps_failed")));
        Run.DurationSeconds = Obj->GetNumberField(TEXT("duration_seconds"));
        return Run;
    }
}

bool FCortexQASessionSerializer::SaveSession(
    const FCortexQASessionInfo& Session,
    const FString& Directory,
    FString& OutPath)
{
    // Ensure directory exists
    IFileManager::Get().MakeDirectory(*Directory, true);

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), Session.Version);
    Root->SetStringField(TEXT("name"), Session.Name);
    Root->SetStringField(TEXT("source"), Session.Source);
    Root->SetStringField(TEXT("recorded_at"), Session.RecordedAt.ToIso8601());
    Root->SetStringField(TEXT("map"), Session.MapPath);
    Root->SetNumberField(TEXT("duration_seconds"), Session.DurationSeconds);
    Root->SetBoolField(TEXT("complete"), Session.bComplete);

    // Steps
    TArray<TSharedPtr<FJsonValue>> StepsArr;
    for (const FCortexQAStep& Step : Session.Steps)
    {
        StepsArr.Add(MakeShared<FJsonValueObject>(StepToJson(Step)));
    }
    Root->SetArrayField(TEXT("steps"), StepsArr);

    // Raw input
    TArray<TSharedPtr<FJsonValue>> RawArr;
    for (const FCortexQARawInputEvent& Evt : Session.RawInput)
    {
        RawArr.Add(MakeShared<FJsonValueObject>(RawInputToJson(Evt)));
    }
    Root->SetArrayField(TEXT("raw_input"), RawArr);

    // Last run (optional)
    if (Session.LastRun.IsSet())
    {
        Root->SetObjectField(TEXT("last_run"), LastRunToJson(Session.LastRun.GetValue()));
    }

    // Conversation history
    TArray<TSharedPtr<FJsonValue>> ConvArr;
    for (const TSharedPtr<FJsonObject>& Entry : Session.ConversationHistory)
    {
        ConvArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("conversation_history"), ConvArr);

    // Serialize to string
    FString JsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    // Build filename: {YYYY-MM-DD}_{name}.json
    const FString DateStr = Session.RecordedAt.ToString(TEXT("%Y-%m-%d"));
    FString SafeName = Session.Name;
    // Replace invalid filename chars
    SafeName.ReplaceCharInline(TEXT(' '), TEXT('_'));
    SafeName.ReplaceCharInline(TEXT('/'), TEXT('_'));
    SafeName.ReplaceCharInline(TEXT('\\'), TEXT('_'));
    const FString Filename = FString::Printf(TEXT("%s_%s.json"), *DateStr, *SafeName);
    OutPath = Directory / Filename;

    // Avoid overwriting — append suffix if file exists
    if (FPaths::FileExists(OutPath))
    {
        int32 Suffix = 1;
        FString BasePath = Directory / FString::Printf(TEXT("%s_%s"), *DateStr, *SafeName);
        do
        {
            OutPath = FString::Printf(TEXT("%s_%d.json"), *BasePath, Suffix++);
        }
        while (FPaths::FileExists(OutPath));
    }

    if (!FFileHelper::SaveStringToFile(JsonStr, *OutPath))
    {
        UE_LOG(LogCortexQA, Error, TEXT("Failed to save session to %s"), *OutPath);
        return false;
    }

    return true;
}

bool FCortexQASessionSerializer::LoadSession(
    const FString& FilePath,
    FCortexQASessionInfo& OutSession)
{
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogCortexQA, Log, TEXT("Failed to parse session JSON: %s"), *FilePath);
        return false;
    }

    OutSession.FilePath = FilePath;
    OutSession.Version = static_cast<int32>(Root->GetNumberField(TEXT("version")));
    OutSession.Name = Root->GetStringField(TEXT("name"));
    OutSession.Source = Root->GetStringField(TEXT("source"));
    FDateTime::ParseIso8601(*Root->GetStringField(TEXT("recorded_at")), OutSession.RecordedAt);
    OutSession.MapPath = Root->GetStringField(TEXT("map"));
    OutSession.DurationSeconds = Root->GetNumberField(TEXT("duration_seconds"));
    OutSession.bComplete = Root->GetBoolField(TEXT("complete"));

    // Steps
    OutSession.Steps.Empty();
    const TArray<TSharedPtr<FJsonValue>>& StepsArr = Root->GetArrayField(TEXT("steps"));
    for (const TSharedPtr<FJsonValue>& Val : StepsArr)
    {
        OutSession.Steps.Add(JsonToStep(Val->AsObject()));
    }

    // Raw input
    OutSession.RawInput.Empty();
    const TArray<TSharedPtr<FJsonValue>>* RawArr = nullptr;
    if (Root->TryGetArrayField(TEXT("raw_input"), RawArr))
    {
        for (const TSharedPtr<FJsonValue>& Val : *RawArr)
        {
            OutSession.RawInput.Add(JsonToRawInput(Val->AsObject()));
        }
    }

    // Last run
    const TSharedPtr<FJsonObject>* LastRunObj = nullptr;
    if (Root->TryGetObjectField(TEXT("last_run"), LastRunObj))
    {
        OutSession.LastRun = JsonToLastRun(*LastRunObj);
    }

    // Conversation history
    OutSession.ConversationHistory.Empty();
    const TArray<TSharedPtr<FJsonValue>>* ConvArr = nullptr;
    if (Root->TryGetArrayField(TEXT("conversation_history"), ConvArr))
    {
        for (const TSharedPtr<FJsonValue>& Val : *ConvArr)
        {
            OutSession.ConversationHistory.Add(Val->AsObject());
        }
    }

    return true;
}

void FCortexQASessionSerializer::ListSessions(
    const FString& Directory,
    TArray<FCortexQASessionInfo>& OutSessions)
{
    OutSessions.Empty();

    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.json")), true, false);

    for (const FString& File : Files)
    {
        FCortexQASessionInfo Info;
        if (LoadSession(Directory / File, Info))
        {
            OutSessions.Add(MoveTemp(Info));
        }
    }
}

bool FCortexQASessionSerializer::DeleteSession(const FString& FilePath)
{
    return IFileManager::Get().Delete(*FilePath);
}

bool FCortexQASessionSerializer::UpdateLastRun(
    const FString& FilePath,
    const FCortexQALastRun& LastRun)
{
    FCortexQASessionInfo Session;
    if (!LoadSession(FilePath, Session))
    {
        return false;
    }
    Session.LastRun = LastRun;

    // Write to a temp file first — ensures original is not lost if write fails
    FString OutPath;
    const FString Directory = FPaths::GetPath(FilePath);
    if (!SaveSession(Session, Directory, OutPath))
    {
        return false;
    }

    // SaveSession wrote to a potentially new name (suffix collision avoidance).
    // Move that file to the original path (overwriting it atomically).
    // IFileManager::Move overwrites destination by default.
    if (OutPath != FilePath)
    {
        if (!IFileManager::Get().Move(*FilePath, *OutPath))
        {
            // Clean up the orphaned save
            IFileManager::Get().Delete(*OutPath);
            return false;
        }
    }

    return true;
}

FString FCortexQASessionSerializer::GetDefaultRecordingsDir()
{
    return FPaths::ProjectSavedDir() / TEXT("CortexQA") / TEXT("Recordings");
}
