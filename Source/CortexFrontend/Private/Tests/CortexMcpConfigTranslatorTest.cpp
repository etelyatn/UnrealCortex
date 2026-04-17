#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Providers/CortexMcpConfigTranslator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMcpConfigTranslatorCodexTest, "Cortex.Frontend.McpConfig.CodexTranslation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMcpConfigTranslatorCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("Task3"));
    const FString ConfigPath = FPaths::Combine(TempDir, TEXT("mcp.json"));
    IFileManager::Get().MakeDirectory(*TempDir, true);

    const FString Json = TEXT("{\"mcpServers\":{\"cortex_mcp\":{\"command\":\"uv\",\"args\":[\"run\",\"--directory\",\"Plugins/UnrealCortex/MCP\",\"cortex-mcp\"],\"env\":{\"CORTEX_PROJECT_DIR\":\".\"}}}}");
    TestTrue(TEXT("Should write test config"), FFileHelper::SaveStringToFile(Json, *ConfigPath));

    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ConfigPath);
    };

    const TArray<FString> ClaudeArgs = FCortexMcpConfigTranslator::BuildClaudeArgs(ConfigPath);
    TestEqual(TEXT("Claude args should be split into flag and path"), ClaudeArgs.Num(), 2);
    TestEqual(TEXT("Claude args should include mcp config flag"), ClaudeArgs[0], FString(TEXT("--mcp-config")));
    TestTrue(TEXT("Claude args should include quoted config path"), ClaudeArgs[1].Contains(TEXT("mcp.json")));

    const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(ConfigPath);
    TestTrue(TEXT("Should include command override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT("mcp_servers.cortex_mcp.command"));
    }));
    TestTrue(TEXT("Should include args override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT("mcp_servers.cortex_mcp.args"));
    }));
    TestTrue(TEXT("Should include env override"), Overrides.ContainsByPredicate([](const FString& Override)
    {
        return Override.Contains(TEXT("mcp_servers.cortex_mcp.env.CORTEX_PROJECT_DIR"));
    }));
    return true;
}
