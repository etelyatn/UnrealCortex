#include "Process/CortexCliDiscovery.h"

#include "CortexFrontendModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

FCortexCliInfo FCortexCliDiscovery::CachedInfo;
bool FCortexCliDiscovery::bHasSearched = false;

void FCortexCliDiscovery::ClearCache()
{
    CachedInfo = FCortexCliInfo();
    bHasSearched = false;
}

FCortexCliInfo FCortexCliDiscovery::FindClaude()
{
    if (bHasSearched)
    {
        return CachedInfo;
    }

    bHasSearched = true;

    TArray<FString> PossiblePaths;

#if PLATFORM_WINDOWS
    const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
    if (!UserProfile.IsEmpty())
    {
        PossiblePaths.Add(FPaths::Combine(UserProfile, TEXT(".local"), TEXT("bin"), TEXT("claude.exe")));
    }

    const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
    if (!AppData.IsEmpty())
    {
        PossiblePaths.Add(FPaths::Combine(AppData, TEXT("npm"), TEXT("claude.cmd")));
    }

    const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
    TArray<FString> PathDirs;
    PathEnv.ParseIntoArray(PathDirs, TEXT(";"), true);
    for (const FString& Dir : PathDirs)
    {
        PossiblePaths.Add(FPaths::Combine(Dir, TEXT("claude.exe")));
        PossiblePaths.Add(FPaths::Combine(Dir, TEXT("claude.cmd")));
    }
#else
    const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
    if (!Home.IsEmpty())
    {
        PossiblePaths.Add(FPaths::Combine(Home, TEXT(".local"), TEXT("bin"), TEXT("claude")));
    }
    PossiblePaths.Add(TEXT("/usr/local/bin/claude"));
    PossiblePaths.Add(TEXT("/usr/bin/claude"));
#endif

    for (const FString& Path : PossiblePaths)
    {
        if (IFileManager::Get().FileExists(*Path))
        {
            UE_LOG(LogCortexFrontend, Log, TEXT("Found Claude CLI at: %s"), *Path);
            CachedInfo.Path = Path;
            CachedInfo.bIsCmd = Path.EndsWith(TEXT(".cmd"));
            CachedInfo.bIsValid = true;
            return CachedInfo;
        }
    }

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
            CachedInfo.Path = Lines[0];
            CachedInfo.bIsCmd = Lines[0].EndsWith(TEXT(".cmd"));
            CachedInfo.bIsValid = true;
            UE_LOG(LogCortexFrontend, Log, TEXT("Found Claude CLI via system search: %s"), *CachedInfo.Path);
            return CachedInfo;
        }
    }

    UE_LOG(LogCortexFrontend, Warning, TEXT("Claude CLI not found. Install with: npm install -g @anthropic-ai/claude-code"));
    return CachedInfo;
}
