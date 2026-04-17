#include "Providers/CortexCodexCliProvider.h"

#include "CortexFrontendModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Providers/CortexProviderRegistry.h"
#include "Process/CortexStreamEvent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    FString FindCodexBinaryFromEnvironment()
    {
#if PLATFORM_WINDOWS
        const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
        if (!UserProfile.IsEmpty())
        {
            const FString LocalBin = FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), TEXT("codex.exe"));
            if (IFileManager::Get().FileExists(*LocalBin))
            {
                return LocalBin;
            }
        }

        const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
        if (!AppData.IsEmpty())
        {
            const FString NpmCmd = FPaths::Combine(AppData, TEXT("npm"), TEXT("codex.cmd"));
            if (IFileManager::Get().FileExists(*NpmCmd))
            {
                return NpmCmd;
            }
        }

        const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
        TArray<FString> PathDirs;
        PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
        for (const FString& Dir : PathDirs)
        {
            const FString ExePath = FPaths::Combine(Dir, TEXT("codex.exe"));
            if (IFileManager::Get().FileExists(*ExePath))
            {
                return ExePath;
            }

            const FString CmdPath = FPaths::Combine(Dir, TEXT("codex.cmd"));
            if (IFileManager::Get().FileExists(*CmdPath))
            {
                return CmdPath;
            }
        }
#else
        const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
        if (!Home.IsEmpty())
        {
            const FString LocalBin = FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("codex"));
            if (IFileManager::Get().FileExists(*LocalBin))
            {
                return LocalBin;
            }
        }

        if (IFileManager::Get().FileExists(TEXT("/usr/local/bin/codex")))
        {
            return TEXT("/usr/local/bin/codex");
        }

        if (IFileManager::Get().FileExists(TEXT("/usr/bin/codex")))
        {
            return TEXT("/usr/bin/codex");
        }
#endif

        return FString();
    }

    FString GetCodexEffortString(ECortexEffortLevel EffortLevel)
    {
        switch (EffortLevel)
        {
        case ECortexEffortLevel::Default:
            return TEXT("default");
        case ECortexEffortLevel::Low:
            return TEXT("low");
        case ECortexEffortLevel::Medium:
            return TEXT("medium");
        case ECortexEffortLevel::High:
            return TEXT("high");
        case ECortexEffortLevel::Maximum:
            return TEXT("maximum");
        }

        return TEXT("default");
    }

    FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Object)
    {
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
        return Json;
    }

    void CopyUsageFields(const TSharedPtr<FJsonObject>& UsageObject, FCortexStreamEvent& OutEvent)
    {
        if (!UsageObject.IsValid())
        {
            return;
        }

        double Value = 0.0;
        if (UsageObject->TryGetNumberField(TEXT("input_tokens"), Value))
        {
            OutEvent.InputTokens = static_cast<int64>(Value);
        }
        if (UsageObject->TryGetNumberField(TEXT("output_tokens"), Value))
        {
            OutEvent.OutputTokens = static_cast<int64>(Value);
        }
        if (UsageObject->TryGetNumberField(TEXT("cache_read_input_tokens"), Value))
        {
            OutEvent.CacheReadTokens = static_cast<int64>(Value);
        }
        if (UsageObject->TryGetNumberField(TEXT("cache_creation_input_tokens"), Value))
        {
            OutEvent.CacheCreationTokens = static_cast<int64>(Value);
        }
    }

    TArray<FCortexStreamEvent> ParseCodexJsonLine(const FString& JsonLine, FString& InOutPendingAssistantText)
    {
        TArray<FCortexStreamEvent> Events;

        TSharedPtr<FJsonObject> JsonObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
        if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        {
            return Events;
        }

        FString Type;
        if (!JsonObj->TryGetStringField(TEXT("type"), Type))
        {
            return Events;
        }

        if (Type == TEXT("thread.started"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::SessionInit;
            Event.RawJson = JsonLine;

            if (!JsonObj->TryGetStringField(TEXT("session_id"), Event.SessionId))
            {
                JsonObj->TryGetStringField(TEXT("thread_id"), Event.SessionId);
            }

            if (Event.SessionId.IsEmpty())
            {
                const TSharedPtr<FJsonObject>* ThreadObject = nullptr;
                if (JsonObj->TryGetObjectField(TEXT("thread"), ThreadObject) && ThreadObject != nullptr)
                {
                    (*ThreadObject)->TryGetStringField(TEXT("id"), Event.SessionId);
                }
            }

            Events.Add(MoveTemp(Event));
            return Events;
        }

        if (Type == TEXT("turn.completed"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::Result;
            Event.RawJson = JsonLine;
            Event.ResultText = InOutPendingAssistantText;

            const TSharedPtr<FJsonObject>* UsageObject = nullptr;
            if (JsonObj->TryGetObjectField(TEXT("usage"), UsageObject) && UsageObject != nullptr)
            {
                CopyUsageFields(*UsageObject, Event);
            }

            JsonObj->TryGetStringField(TEXT("session_id"), Event.SessionId);
            if (Event.ResultText.IsEmpty())
            {
                JsonObj->TryGetStringField(TEXT("result"), Event.ResultText);
            }
            Event.Text = Event.ResultText;
            Events.Add(MoveTemp(Event));
            return Events;
        }

        if (Type == TEXT("turn.failed") || Type == TEXT("error"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::Result;
            Event.bIsError = true;
            Event.RawJson = JsonLine;
            Event.ResultText = InOutPendingAssistantText;

            const TSharedPtr<FJsonObject>* UsageObject = nullptr;
            if (Type == TEXT("turn.failed") && JsonObj->TryGetObjectField(TEXT("usage"), UsageObject) && UsageObject != nullptr)
            {
                CopyUsageFields(*UsageObject, Event);
            }

            if (!JsonObj->TryGetStringField(TEXT("message"), Event.Text))
            {
                JsonObj->TryGetStringField(TEXT("error"), Event.Text);
            }

            const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
            if (JsonObj->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject != nullptr)
            {
                (*ErrorObject)->TryGetStringField(TEXT("message"), Event.Text);
                if (Event.Text.IsEmpty())
                {
                    (*ErrorObject)->TryGetStringField(TEXT("type"), Event.Text);
                }
            }

            if (Event.Text.IsEmpty())
            {
                Event.Text = JsonLine;
            }

            if (Event.ResultText.IsEmpty())
            {
                Event.ResultText = Event.Text;
            }
            Event.Text = Event.ResultText;

            Events.Add(MoveTemp(Event));
            return Events;
        }

        const TSharedPtr<FJsonObject>* ItemObject = nullptr;
        if ((Type == TEXT("item.started") || Type == TEXT("item.completed")) &&
            JsonObj->TryGetObjectField(TEXT("item"), ItemObject) && ItemObject != nullptr)
        {
            FString ItemType;
            if ((*ItemObject)->TryGetStringField(TEXT("type"), ItemType))
            {
                if (ItemType == TEXT("command_execution"))
                {
                    FCortexStreamEvent Event;
                    Event.RawJson = JsonLine;
                    (*ItemObject)->TryGetStringField(TEXT("id"), Event.ToolCallId);
                    Event.ToolName = TEXT("command_execution");

                    if (Type == TEXT("item.started"))
                    {
                        Event.Type = ECortexStreamEventType::ToolUse;
                        Event.ToolInput = SerializeJsonObject(*ItemObject);
                    }
                    else
                    {
                        Event.Type = ECortexStreamEventType::ToolResult;
                        if (!(*ItemObject)->TryGetStringField(TEXT("output"), Event.ToolResultContent))
                        {
                            (*ItemObject)->TryGetStringField(TEXT("result"), Event.ToolResultContent);
                        }
                    }

                    Events.Add(MoveTemp(Event));
                    return Events;
                }

                if (ItemType == TEXT("agent_message") && Type == TEXT("item.completed"))
                {
                    FCortexStreamEvent Event;
                    Event.Type = ECortexStreamEventType::TextContent;
                    Event.RawJson = JsonLine;
                    (*ItemObject)->TryGetStringField(TEXT("text"), Event.Text);
                    InOutPendingAssistantText = Event.Text;
                    Events.Add(MoveTemp(Event));
                    return Events;
                }

                return Events;
            }
        }

        return CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    }
}

FName FCortexCodexCliProvider::GetProviderId() const
{
    return FName(TEXT("codex"));
}

const FCortexProviderDefinition& FCortexCodexCliProvider::GetDefinition() const
{
    return FCortexProviderRegistry::ResolveDefinition(TEXT("codex"));
}

ECortexCliTransportMode FCortexCodexCliProvider::GetTransportMode() const
{
    return ECortexCliTransportMode::PerTurnExec;
}

bool FCortexCodexCliProvider::SupportsResume() const
{
    return true;
}

FCortexCliInfo FCortexCodexCliProvider::FindCli() const
{
    FCortexCliInfo Info;
    Info.ProviderId = GetProviderId();
    Info.ProviderDisplayName = GetDefinition().DisplayName;
    Info.Path = FindCodexBinaryFromEnvironment();

    if (Info.Path.IsEmpty())
    {
        FString WhereOutput;
        FString WhereErrors;
        int32 ReturnCode = 0;
#if PLATFORM_WINDOWS
        const bool bSearchOk = FPlatformProcess::ExecProcess(TEXT("where"), TEXT("codex"), &ReturnCode, &WhereOutput, &WhereErrors);
#else
        const bool bSearchOk = FPlatformProcess::ExecProcess(TEXT("/bin/sh"), TEXT("-c 'which codex 2>/dev/null'"), &ReturnCode, &WhereOutput, &WhereErrors);
#endif
        if (bSearchOk && ReturnCode == 0)
        {
            WhereOutput.TrimStartAndEndInline();
            TArray<FString> Lines;
            WhereOutput.ParseIntoArrayLines(Lines);
            if (Lines.Num() > 0)
            {
                Info.Path = Lines[0];
            }
        }
    }

    Info.bIsCmd = Info.Path.EndsWith(TEXT(".cmd"));
    Info.bIsValid = !Info.Path.IsEmpty();
    if (Info.bIsValid)
    {
        UE_LOG(LogCortexFrontend, Log, TEXT("Found Codex CLI at: %s"), *Info.Path);
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("Codex CLI not found. Install with: %s"), *GetDefinition().InstallationHintText);
    }
    return Info;
}

FString FCortexCodexCliProvider::BuildLaunchCommandLine(
    bool bResumeSession,
    ECortexAccessMode AccessMode,
    const FCortexSessionConfig& SessionConfig) const
{
    (void)AccessMode;

    FString CommandLine = bResumeSession ? TEXT("exec resume --json ") : TEXT("exec --json ");

    if (bResumeSession)
    {
        if (!SessionConfig.SessionId.IsEmpty())
        {
            CommandLine += FString::Printf(TEXT("\"%s\" "), *SessionConfig.SessionId);
        }
        else
        {
            CommandLine += TEXT("--last ");
        }
    }
    else
    {
        const FString WorkingDirectory = !SessionConfig.WorkingDirectory.IsEmpty()
            ? SessionConfig.WorkingDirectory
            : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        if (!WorkingDirectory.IsEmpty())
        {
            CommandLine += FString::Printf(TEXT("-C \"%s\" "), *WorkingDirectory.Replace(TEXT("\\"), TEXT("/")));
        }
    }

    if (!SessionConfig.McpConfigPath.IsEmpty())
    {
        const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(SessionConfig.McpConfigPath);
        for (const FString& Override : Overrides)
        {
            CommandLine += Override + TEXT(" ");
        }
    }

    if (!SessionConfig.ModelId.IsEmpty() && SessionConfig.ModelId != TEXT("Default"))
    {
        CommandLine += FString::Printf(TEXT("-m \"%s\" "), *SessionConfig.ModelId);
    }

    if (SessionConfig.EffortLevel != ECortexEffortLevel::Default)
    {
        CommandLine += FString::Printf(TEXT("-c model_reasoning_effort=%s "), *GetCodexEffortString(SessionConfig.EffortLevel));
    }

    if (SessionConfig.bSkipPermissions)
    {
        CommandLine += TEXT("--dangerously-bypass-approvals-and-sandbox ");
    }

    return CommandLine.TrimStartAndEnd();
}

FString FCortexCodexCliProvider::BuildAuthCommand() const
{
    return TEXT("codex login");
}

void FCortexCodexCliProvider::ConsumeStreamChunk(
    const FString& RawChunk,
    FString& InOutChunkBuffer,
    TArray<FCortexStreamEvent>& OutEvents) const
{
    InOutChunkBuffer += RawChunk;
    FString PendingAssistantText;

    int32 NewLineIndex = INDEX_NONE;
    while (InOutChunkBuffer.FindChar(TEXT('\n'), NewLineIndex))
    {
        const FString Line = InOutChunkBuffer.Left(NewLineIndex).TrimStartAndEnd();
        InOutChunkBuffer = InOutChunkBuffer.Mid(NewLineIndex + 1);
        if (Line.IsEmpty())
        {
            continue;
        }

        OutEvents.Append(ParseCodexJsonLine(Line, PendingAssistantText));
    }
}
