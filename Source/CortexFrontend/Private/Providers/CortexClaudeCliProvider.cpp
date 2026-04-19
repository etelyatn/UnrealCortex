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
    FString SanitizeClaudeDirective(const FString& Directive)
    {
        FString Sanitized = Directive;
        Sanitized.ReplaceInline(TEXT("\n"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("\r"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("\t"), TEXT(" "));
        Sanitized.ReplaceInline(TEXT("$("), TEXT(""));
        Sanitized.ReplaceInline(TEXT("`"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("%"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("^"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("|"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("&"), TEXT(""));
        Sanitized.ReplaceInline(TEXT(">"), TEXT(""));
        Sanitized.ReplaceInline(TEXT("<"), TEXT(""));
        return Sanitized;
    }

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

    FString BuildAllowedToolsArg(ECortexAccessMode AccessMode)
    {
        static const TCHAR* ReadOnlyBuiltins = TEXT("Read,Glob,Grep,Agent,WebFetch,WebSearch,AskUserQuestion,TodoRead,TodoWrite");
        static const TCHAR* GuidedBuiltins = TEXT("Read,Edit,Write,Bash,Glob,Grep,Agent,WebFetch,WebSearch,NotebookEdit,AskUserQuestion,TodoRead,TodoWrite");

        switch (AccessMode)
        {
        case ECortexAccessMode::ReadOnly:
            return FString::Printf(TEXT("%s,"
                "mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*"),
                ReadOnlyBuiltins);
        case ECortexAccessMode::Guided:
            return FString::Printf(TEXT("%s,"
                "mcp__cortex_mcp__get_*,mcp__cortex_mcp__list_*,mcp__cortex_mcp__search_*,mcp__cortex_mcp__query_*,mcp__cortex_mcp__describe_*,mcp__cortex_mcp__find_*,mcp__cortex_mcp__schema_*,mcp__cortex_mcp__reflect_*,"
                "mcp__cortex_mcp__spawn_*,mcp__cortex_mcp__create_*,mcp__cortex_mcp__add_*,mcp__cortex_mcp__set_*,mcp__cortex_mcp__compile_*,mcp__cortex_mcp__connect_*,"
                "mcp__cortex_mcp__graph_add_*,mcp__cortex_mcp__graph_connect,mcp__cortex_mcp__graph_list_*,mcp__cortex_mcp__graph_get_*,mcp__cortex_mcp__graph_set_*,mcp__cortex_mcp__graph_search_*,mcp__cortex_mcp__graph_auto_layout,"
                "mcp__cortex_mcp__open_*,mcp__cortex_mcp__close_*,mcp__cortex_mcp__focus_*,mcp__cortex_mcp__select_*,"
                "mcp__cortex_mcp__rename_*,mcp__cortex_mcp__configure_*,mcp__cortex_mcp__import_*,mcp__cortex_mcp__update_*,mcp__cortex_mcp__duplicate_*,mcp__cortex_mcp__reparent*,mcp__cortex_mcp__attach_*,mcp__cortex_mcp__detach_*,mcp__cortex_mcp__register_*,mcp__cortex_mcp__reload_*"),
                GuidedBuiltins);
        case ECortexAccessMode::FullAccess:
            return FString();
        }

        return FString();
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
    const FCortexResolvedLaunchOptions& LaunchOptions = SessionConfig.LaunchOptions;
    const FCortexResolvedSessionOptions& ResolvedOptions = SessionConfig.ResolvedOptions;

    FString CommandLine = TEXT("-p --input-format stream-json --output-format stream-json --verbose --include-partial-messages ");

    if (LaunchOptions.bSkipPermissions)
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

    if (!ResolvedOptions.ModelId.IsEmpty() && ResolvedOptions.ModelId != TEXT("Default"))
    {
        CommandLine += FString::Printf(TEXT("--model \"%s\" "), *ResolvedOptions.ModelId);
    }

    if (SessionConfig.bConversionMode)
    {
        CommandLine += TEXT("--strict-mcp-config ");
        CommandLine += TEXT("--setting-sources \"user,local\" ");
        CommandLine += TEXT("--effort \"low\" ");
    }
    else
    {
        if (ResolvedOptions.EffortLevel != ECortexEffortLevel::Default)
        {
            CommandLine += FString::Printf(TEXT("--effort \"%s\" "), *GetClaudeEffortString(ResolvedOptions.EffortLevel));
        }

        if (LaunchOptions.WorkflowMode == ECortexWorkflowMode::Direct)
        {
            CommandLine += TEXT("--disable-slash-commands ");
        }

        if (!LaunchOptions.bProjectContext)
        {
            CommandLine += TEXT("--setting-sources \"user,local\" ");
        }

        const FString AllowedTools = BuildAllowedToolsArg(AccessMode);
        if (!AllowedTools.IsEmpty())
        {
            CommandLine += FString::Printf(TEXT("--allowedTools \"%s\" "), *AllowedTools);
        }

    }

    FString SystemPrompt;
    if (!SessionConfig.SystemPrompt.IsEmpty())
    {
        SystemPrompt = SessionConfig.SystemPrompt;
    }
    else
    {
        FString ModeString;
        switch (AccessMode)
        {
        case ECortexAccessMode::ReadOnly:
            ModeString = TEXT("Read-Only");
            break;
        case ECortexAccessMode::Guided:
            ModeString = TEXT("Guided");
            break;
        case ECortexAccessMode::FullAccess:
            ModeString = TEXT("Full Access");
            break;
        }

        SystemPrompt = FString::Printf(
            TEXT("You are running inside the Unreal Editor's Cortex AI Chat panel. "
                 "You have access to Cortex MCP tools for querying and manipulating the editor. "
                 "Current access mode: %s."), *ModeString);

        if (LaunchOptions.WorkflowMode == ECortexWorkflowMode::Direct)
        {
            SystemPrompt += TEXT(" Workflow mode: Direct. Act immediately on requests using MCP tools. "
                "Do not create planning documents, design docs, spec files, or brainstorming files. "
                "Do not follow documentation-first workflows. Be concise. Prefer action over ceremony.");
        }

        if (!LaunchOptions.CustomDirective.IsEmpty())
        {
            const FString Sanitized = SanitizeClaudeDirective(LaunchOptions.CustomDirective);
            SystemPrompt += TEXT(" ") + Sanitized;
        }
    }

    CommandLine += FString::Printf(TEXT("--append-system-prompt \"%s\" "),
        *SystemPrompt.Replace(TEXT("\""), TEXT("\\\"")));

    return CommandLine.TrimStartAndEnd();
}

FString FCortexClaudeCliProvider::BuildPromptEnvelope(
    const FString& Prompt,
    ECortexAccessMode AccessMode,
    const FCortexSessionConfig& SessionConfig) const
{
    (void)AccessMode;
    (void)SessionConfig;

    FString EscapedPrompt = Prompt;
    EscapedPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    EscapedPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));
    EscapedPrompt.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    EscapedPrompt.ReplaceInline(TEXT("\r"), TEXT("\\r"));
    EscapedPrompt.ReplaceInline(TEXT("\t"), TEXT("\\t"));
    return FString::Printf(TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"%s\"}}\n"), *EscapedPrompt);
}

FString FCortexClaudeCliProvider::BuildAuthCommand() const
{
    return TEXT("claude login");
}

void FCortexClaudeCliProvider::ConsumeStreamChunk(
    const FString& RawChunk,
    FString& InOutChunkBuffer,
    FString& InOutAssistantText,
    TArray<FCortexStreamEvent>& OutEvents) const
{
    (void)InOutAssistantText;
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
