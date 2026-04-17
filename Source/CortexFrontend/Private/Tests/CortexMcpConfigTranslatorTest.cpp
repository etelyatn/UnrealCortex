#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMcpConfigTranslatorCodexTest, "Cortex.Frontend.McpConfig.CodexTranslation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMcpConfigTranslatorCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString ConfigPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    TestTrue(TEXT("Real project .mcp.json should exist"), IFileManager::Get().FileExists(*ConfigPath));

    const TArray<FString> ClaudeArgs = FCortexMcpConfigTranslator::BuildClaudeArgs(ConfigPath);
    TestEqual(TEXT("Claude args should be split into flag and path"), ClaudeArgs.Num(), 2);
    TestEqual(TEXT("Claude args should include mcp config flag"), ClaudeArgs[0], FString(TEXT("--mcp-config")));
    TestTrue(TEXT("Claude args should include quoted config path"), ClaudeArgs[1].Contains(TEXT(".mcp.json")));

    const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(ConfigPath);
    TestTrue(TEXT("Should include at least one override"), Overrides.Num() > 0);
    TestTrue(TEXT("Should include command override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".command="));
    }));
    TestTrue(TEXT("Should include args override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".args="));
    }));
    TestTrue(TEXT("Should include env override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".env.CORTEX_PROJECT_DIR="));
    }));
    const int32 CommandIndex = Overrides.IndexOfByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".command="));
    });
    const int32 ArgsIndex = Overrides.IndexOfByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".args="));
    });
    const int32 EnvIndex = Overrides.IndexOfByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT(".env.CORTEX_PROJECT_DIR="));
    });
    TestTrue(TEXT("Overrides should be emitted in a deterministic order"), CommandIndex != INDEX_NONE && ArgsIndex != INDEX_NONE && EnvIndex != INDEX_NONE && CommandIndex < ArgsIndex && ArgsIndex < EnvIndex);
    return true;
}
