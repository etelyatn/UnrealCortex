#include "Providers/CortexClaudeCliProvider.h"

#include "CortexFrontendModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Providers/CortexProviderRegistry.h"

namespace
{
    FString GetClaudeEffortString(ECortexEffortLevel EffortLevel)
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

    FString FindClaudeBinaryFromEnvironment()
    {
#if PLATFORM_WINDOWS
        const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
        if (!UserProfile.IsEmpty())
        {
            const FString LocalBin = FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), TEXT("claude.exe"));
            if (IFileManager::Get().FileExists(*LocalBin))
            {
                return LocalBin;
            }
        }

        const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
        if (!AppData.IsEmpty())
        {
            const FString NpmCmd = FPaths::Combine(AppData, TEXT("npm"), TEXT("claude.cmd"));
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
            const FString ExePath = FPaths::Combine(Dir, TEXT("claude.exe"));
            if (IFileManager::Get().FileExists(*ExePath))
            {
                return ExePath;
            }

            const FString CmdPath = FPaths::Combine(Dir, TEXT("claude.cmd"));
            if (IFileManager::Get().FileExists(*CmdPath))
            {
                return CmdPath;
            }
        }
#else
        const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
        if (!Home.IsEmpty())
        {
            const FString LocalBin = FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("claude"));
            if (IFileManager::Get().FileExists(*LocalBin))
            {
                return LocalBin;
            }
        }

        if (IFileManager::Get().FileExists(TEXT("/usr/local/bin/claude")))
        {
            return TEXT("/usr/local/bin/claude");
        }

        if (IFileManager::Get().FileExists(TEXT("/usr/bin/claude")))
        {
            return TEXT("/usr/bin/claude");
        }
#endif

        return FString();
    }
}

FName FCortexClaudeCliProvider::GetProviderId() const
{
    return FName(TEXT("claude_code"));
}

const FCortexProviderDefinition& FCortexClaudeCliProvider::GetDefinition() const
{
    return FCortexProviderRegistry::ResolveDefinition(TEXT("claude_code"));
}

ECortexCliTransportMode FCortexClaudeCliProvider::GetTransportMode() const
{
    return ECortexCliTransportMode::PersistentSession;
}

bool FCortexClaudeCliProvider::SupportsResume() const
{
    return true;
}

FCortexCliInfo FCortexClaudeCliProvider::FindCli() const
{
    FCortexCliInfo Info;
    Info.ProviderId = GetProviderId();
    Info.ProviderDisplayName = GetDefinition().DisplayName;
    Info.Path = FindClaudeBinaryFromEnvironment();

    if (Info.Path.IsEmpty())
    {
        FString WhereOutput;
        FString WhereErrors;
        int32 ReturnCode = 0;
#if PLATFORM_WINDOWS
        const bool bSearchOk = FPlatformProcess::ExecProcess(TEXT("where"), TEXT("claude"), &ReturnCode, &WhereOutput, &WhereErrors);
#else
        const bool bSearchOk = FPlatformProcess::ExecProcess(TEXT("/bin/sh"), TEXT("-c 'which claude 2>/dev/null'"), &ReturnCode, &WhereOutput, &WhereErrors);
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
        UE_LOG(LogCortexFrontend, Log, TEXT("Found Claude CLI at: %s"), *Info.Path);
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("Claude CLI not found. Install with: %s"), *GetDefinition().InstallationHintText);
    }
    return Info;
}

FString FCortexClaudeCliProvider::BuildLaunchCommandLine(
    bool bResumeSession,
    ECortexAccessMode AccessMode,
    const FCortexSessionConfig& SessionConfig) const
{
    FString CommandLine = TEXT("-p --input-format stream-json --output-format stream-json --verbose --include-partial-messages ");
    (void)AccessMode;

    if (SessionConfig.bSkipPermissions)
    {
        CommandLine += TEXT("--dangerously-skip-permissions ");
    }

    if (bResumeSession)
    {
        CommandLine += FString::Printf(TEXT("--resume \"%s\" "), *SessionConfig.SessionId);
    }
    else
    {
        CommandLine += FString::Printf(TEXT("--session-id \"%s\" "), *SessionConfig.SessionId);
    }

    if (!SessionConfig.McpConfigPath.IsEmpty())
    {
        const TArray<FString> Args = FCortexMcpConfigTranslator::BuildClaudeArgs(SessionConfig.McpConfigPath);
        for (const FString& Arg : Args)
        {
            CommandLine += Arg + TEXT(" ");
        }
    }

    if (!SessionConfig.ModelId.IsEmpty() && SessionConfig.ModelId != TEXT("Default"))
    {
        CommandLine += FString::Printf(TEXT("--model \"%s\" "), *SessionConfig.ModelId);
    }

    if (SessionConfig.EffortLevel != ECortexEffortLevel::Default)
    {
        CommandLine += FString::Printf(TEXT("--effort \"%s\" "), *GetClaudeEffortString(SessionConfig.EffortLevel));
    }

    return CommandLine.TrimStartAndEnd();
}

FString FCortexClaudeCliProvider::BuildAuthCommand() const
{
    return TEXT("claude login");
}

void FCortexClaudeCliProvider::ConsumeStreamChunk(
    const FString& RawChunk,
    FString& InOutChunkBuffer,
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

        OutEvents.Append(CortexStreamEventParser::ParseNdjsonLine(Line));
    }
}
