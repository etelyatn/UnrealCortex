#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Providers/CortexCliProvider.h"
#include "Process/CortexCliDiscovery.h"

namespace
{
class FFakeCliProvider final : public ICortexCliProvider
{
public:
    FFakeCliProvider(FName InProviderId, FString InPath)
        : ProviderId(InProviderId)
        , Path(MoveTemp(InPath))
    {
    }

    virtual FName GetProviderId() const override
    {
        return ProviderId;
    }

    virtual const FCortexProviderDefinition& GetDefinition() const override
    {
        static const FCortexProviderDefinition DummyDefinition;
        return DummyDefinition;
    }

    virtual ECortexCliTransportMode GetTransportMode() const override
    {
        return ECortexCliTransportMode::PersistentSession;
    }

    virtual bool SupportsResume() const override
    {
        return true;
    }

    virtual FCortexCliInfo FindCli() const override
    {
        FCortexCliInfo Info;
        Info.ProviderId = ProviderId;
        Info.Path = Path;
        Info.bIsCmd = Path.EndsWith(TEXT(".cmd"));
        Info.bIsValid = true;
        return Info;
    }

    virtual FString BuildLaunchCommandLine(
        const FString& WorkingDirectory,
        const FString& SessionId,
        const FString& McpConfigPath,
        const FString& ModelId,
        ECortexEffortLevel EffortLevel,
        bool bBypassApprovals,
        bool bSkipPermissions,
        bool bResumeSession) const override
    {
        return FString::Printf(
            TEXT("%s|%s|%s|%s|%d|%d|%d|%d"),
            *WorkingDirectory,
            *SessionId,
            *McpConfigPath,
            *ModelId,
            static_cast<int32>(EffortLevel),
            bBypassApprovals ? 1 : 0,
            bSkipPermissions ? 1 : 0,
            bResumeSession ? 1 : 0);
    }

    virtual FString BuildAuthCommand() const override
    {
        return FString::Printf(TEXT("%s login"), *ProviderId.ToString());
    }

    virtual bool TryConsumeStreamChunk(const FString& RawLine, FCortexStreamEvent& OutEvent) const override
    {
        OutEvent.RawJson = RawLine;
        OutEvent.SessionId = ProviderId.ToString();
        return true;
    }

private:
    FName ProviderId;
    FString Path;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryFindClaudeTest, "Cortex.Frontend.CliDiscovery.FindClaude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryFindClaudeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    const FCortexCliInfo Info = FCortexCliDiscovery::FindClaude();
    if (Info.bIsValid)
    {
        TestFalse(TEXT("Path should not be empty when valid"), Info.Path.IsEmpty());
        AddInfo(FString::Printf(TEXT("Found Claude at: %s (isCmd=%d)"), *Info.Path, Info.bIsCmd));
    }
    else
    {
        AddInfo(TEXT("Claude CLI not found - search completed without crash"));
    }
    const FCortexCliInfo Info2 = FCortexCliDiscovery::FindClaude();
    TestEqual(TEXT("Second call should return same path"), Info.Path, Info2.Path);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryFindCodexTest, "Cortex.Frontend.CliDiscovery.FindCodex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryFindCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("codex")), MakeShared<FFakeCliProvider>(FName(TEXT("codex")), TEXT("C:/Temp/codex.exe")));

    const FCortexCliInfo Info = FCortexCliDiscovery::Find(FName(TEXT("codex")));
    TestTrue(TEXT("Codex discovery should be valid"), Info.bIsValid);
    TestEqual(TEXT("Provider id should be codex"), Info.ProviderId, FName(TEXT("codex")));
    TestEqual(TEXT("Path should come from the injected provider"), Info.Path, FString(TEXT("C:/Temp/codex.exe")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryProviderScopedCacheTest, "Cortex.Frontend.CliDiscovery.ProviderScopedCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryProviderScopedCacheTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
    };

    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("claude_code")), MakeShared<FFakeCliProvider>(FName(TEXT("claude_code")), TEXT("C:/Temp/claude-a.exe")));
    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("codex")), MakeShared<FFakeCliProvider>(FName(TEXT("codex")), TEXT("C:/Temp/codex-a.exe")));

    const FCortexCliInfo ClaudeInfo = FCortexCliDiscovery::Find(FName(TEXT("claude_code")));
    const FCortexCliInfo CodexInfo = FCortexCliDiscovery::Find(FName(TEXT("codex")));

    TestEqual(TEXT("Claude cache entry should keep Claude path"), ClaudeInfo.Path, FString(TEXT("C:/Temp/claude-a.exe")));
    TestEqual(TEXT("Codex cache entry should keep Codex path"), CodexInfo.Path, FString(TEXT("C:/Temp/codex-a.exe")));

    FCortexCliDiscovery::SetProviderOverrideForTest(FName(TEXT("codex")), MakeShared<FFakeCliProvider>(FName(TEXT("codex")), TEXT("C:/Temp/codex-b.exe")));
    const FCortexCliInfo CachedCodexInfo = FCortexCliDiscovery::Find(FName(TEXT("codex")));
    TestEqual(TEXT("Codex cache should be provider-scoped"), CachedCodexInfo.Path, FString(TEXT("C:/Temp/codex-a.exe")));
    TestTrue(TEXT("Claude and Codex cached results should differ"), ClaudeInfo.Path != CachedCodexInfo.Path);
    return true;
}
