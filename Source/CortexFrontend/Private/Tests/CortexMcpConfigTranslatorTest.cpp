#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMcpConfigTranslatorCodexTest, "Cortex.Frontend.McpConfig.CodexTranslation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMcpConfigTranslatorCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString ConfigPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    TestTrue(TEXT("Real project .mcp.json should exist"), IFileManager::Get().FileExists(*ConfigPath));

    FString JsonText;
    TestTrue(TEXT("Real project .mcp.json should load"), FFileHelper::LoadFileToString(JsonText, *ConfigPath));

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    TestTrue(TEXT("Real project .mcp.json should parse"), FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid());

    const TArray<FString> ClaudeArgs = FCortexMcpConfigTranslator::BuildClaudeArgs(ConfigPath);
    TestEqual(TEXT("Claude args should be split into flag and path"), ClaudeArgs.Num(), 2);
    TestEqual(TEXT("Claude args should include mcp config flag"), ClaudeArgs[0], FString(TEXT("--mcp-config")));
    TestTrue(TEXT("Claude args should include quoted config path"), ClaudeArgs[1].Contains(TEXT(".mcp.json")));

    const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(ConfigPath);
    TestTrue(TEXT("Should include at least one override"), Overrides.Num() > 0);

    const TSharedPtr<FJsonObject>* ServersObject = nullptr;
    TestTrue(TEXT("Real project .mcp.json should contain mcpServers"), RootObject->TryGetObjectField(TEXT("mcpServers"), ServersObject) || RootObject->TryGetObjectField(TEXT("mcp_servers"), ServersObject));
    TestTrue(TEXT("MCP servers object should be valid"), ServersObject != nullptr && ServersObject->IsValid());

    TArray<FString> ServerNames;
    (*ServersObject)->Values.GetKeys(ServerNames);
    ServerNames.Sort();
    TestTrue(TEXT("Project should define at least one MCP server"), ServerNames.Num() > 0);

    for (const FString& ServerName : ServerNames)
    {
        const TSharedPtr<FJsonObject>* ServerObject = nullptr;
        if (!(*ServersObject)->TryGetObjectField(ServerName, ServerObject) || ServerObject == nullptr)
        {
            continue;
        }

        const FString Prefix = FString::Printf(TEXT("mcp_servers.%s."), *ServerName);

        FString Command;
        if ((*ServerObject)->TryGetStringField(TEXT("command"), Command))
        {
            const FString ExpectedOverride = FString::Printf(
                TEXT("\"-c %scommand='%s'\""),
                *Prefix,
                *Command);
            TestTrue(FString::Printf(TEXT("Should translate command for %s"), *ServerName), Overrides.Contains(ExpectedOverride));
        }

        const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
        if ((*ServerObject)->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray != nullptr && ArgsArray->Num() > 0)
        {
            TArray<FString> Args;
            for (const TSharedPtr<FJsonValue>& ArgValue : *ArgsArray)
            {
                FString Arg;
                if (ArgValue.IsValid() && ArgValue->TryGetString(Arg))
                {
                    Args.Add(Arg);
                }
            }

            const FString ExpectedOverride = FString::Printf(
                TEXT("\"-c %sargs=['%s']\""),
                *Prefix,
                *FString::Join(Args, TEXT("','")));
            TestTrue(FString::Printf(TEXT("Should translate args for %s"), *ServerName), Overrides.Contains(ExpectedOverride));
        }

        const TSharedPtr<FJsonObject>* EnvObject = nullptr;
        if ((*ServerObject)->TryGetObjectField(TEXT("env"), EnvObject) && EnvObject != nullptr && (*EnvObject)->Values.Num() > 0)
        {
            const FString ExpectedOverride = FString::Printf(
                TEXT("\"-c %senv.CORTEX_PROJECT_DIR='.'\""),
                *Prefix);
            TestTrue(FString::Printf(TEXT("Should translate env for %s"), *ServerName), Overrides.Contains(ExpectedOverride));
        }
    }
    return true;
}
