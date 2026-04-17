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
        TSharedPtr<FJsonObject> JsonObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
        if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        {
            return {};
        }

        TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseCodexJsonObject(JsonObj, InOutPendingAssistantText);
        for (FCortexStreamEvent& Event : Events)
        {
            Event.RawJson = JsonLine;
        }
        return Events;
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
    const FCortexResolvedLaunchOptions& LaunchOptions = SessionConfig.LaunchOptions;
    const FCortexResolvedSessionOptions& ResolvedOptions = SessionConfig.ResolvedOptions;

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

    if (!ResolvedOptions.ModelId.IsEmpty() && ResolvedOptions.ModelId != TEXT("Default"))
    {
        CommandLine += FString::Printf(TEXT("-m \"%s\" "), *ResolvedOptions.ModelId);
    }

    if (ResolvedOptions.EffortLevel != ECortexEffortLevel::Default)
    {
        CommandLine += FString::Printf(TEXT("-c model_reasoning_effort=%s "), *GetCodexEffortString(ResolvedOptions.EffortLevel));
    }

    if (LaunchOptions.bSkipPermissions)
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
        FString& InOutAssistantText,
        TArray<FCortexStreamEvent>& OutEvents) const
{
    InOutChunkBuffer += RawChunk;

    int32 NewLineIndex = INDEX_NONE;
    while (InOutChunkBuffer.FindChar(TEXT('\n'), NewLineIndex))
    {
        const FString Line = InOutChunkBuffer.Left(NewLineIndex).TrimStartAndEnd();
        InOutChunkBuffer = InOutChunkBuffer.Mid(NewLineIndex + 1);
        if (Line.IsEmpty())
        {
            continue;
        }

        OutEvents.Append(ParseCodexJsonLine(Line, InOutAssistantText));
    }
}
