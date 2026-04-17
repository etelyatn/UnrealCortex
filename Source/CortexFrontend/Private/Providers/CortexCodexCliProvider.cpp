#include "Providers/CortexCodexCliProvider.h"

#include "CortexFrontendModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Providers/CortexProviderRegistry.h"

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
    return false;
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
    (void)bResumeSession;

    FString CommandLine = TEXT("exec --json ");

    const FString WorkingDirectory = !SessionConfig.WorkingDirectory.IsEmpty()
        ? SessionConfig.WorkingDirectory
        : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    if (!WorkingDirectory.IsEmpty())
    {
        CommandLine += FString::Printf(TEXT("-C \"%s\" "), *WorkingDirectory.Replace(TEXT("\\"), TEXT("/")));
    }

    if (!SessionConfig.McpConfigPath.IsEmpty())
    {
        const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(SessionConfig.McpConfigPath);
        for (const FString& Override : Overrides)
        {
            CommandLine += Override + TEXT(" ");
        }
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
