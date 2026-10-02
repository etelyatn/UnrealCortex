#include "Misc/AutomationTest.h"
#include "CortexEngineCompat.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Providers/CortexMcpConfigTranslator.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMcpConfigTranslatorCodexTest, "Cortex.Frontend.McpConfig.CodexTranslation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMcpConfigTranslatorCodexTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString ConfigPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
    if (!TestTrue(TEXT("Real project .mcp.json should exist"), IFileManager::Get().FileExists(*ConfigPath)))
    {
        return false;
    }

    FString JsonText;
    if (!TestTrue(TEXT("Real project .mcp.json should load"), FFileHelper::LoadFileToString(JsonText, *ConfigPath)))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!TestTrue(TEXT("Real project .mcp.json should parse"), FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid()))
    {
        return false;
    }

    const TArray<FString> ClaudeArgs = FCortexMcpConfigTranslator::BuildClaudeArgs(ConfigPath);
    if (!TestEqual(TEXT("Claude args should be split into flag and path"), ClaudeArgs.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("Claude args should include mcp config flag"), ClaudeArgs[0], FString(TEXT("--mcp-config")));
    TestTrue(TEXT("Claude args should include quoted config path"), ClaudeArgs[1].Contains(TEXT(".mcp.json")));

    const TArray<FString> Overrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(ConfigPath);
    TestTrue(TEXT("Should include at least one override"), Overrides.Num() > 0);
    if (!TestEqual(TEXT("Codex overrides should be flag/value pairs"), Overrides.Num() % 2, 0))
    {
        return false;
    }
    for (int32 Index = 0; Index < Overrides.Num(); Index += 2)
    {
        TestEqual(TEXT("Codex override flag should be a separate argv token"), Overrides[Index], FString(TEXT("-c")));
        TestFalse(TEXT("Codex override value should not include the -c flag"), Overrides[Index + 1].StartsWith(TEXT("\"-c ")));
    }

    const TSharedPtr<FJsonObject>* ServersObject = nullptr;
    TestTrue(TEXT("Real project .mcp.json should contain mcpServers"), RootObject->TryGetObjectField(TEXT("mcpServers"), ServersObject) || RootObject->TryGetObjectField(TEXT("mcp_servers"), ServersObject));
    if (!TestTrue(TEXT("MCP servers object should be valid"), ServersObject != nullptr && ServersObject->IsValid()))
    {
        return false;
    }

    TArray<FString> ServerNames;
    ServerNames.Reserve((*ServersObject)->Values.Num());
    for (const auto& Pair : (*ServersObject)->Values)
    {
        ServerNames.Add(CortexEngineCompat::JsonKeyToString(Pair.Key));
    }
    ServerNames.Sort();
    TestTrue(TEXT("Project should define at least one MCP server"), ServerNames.Num() > 0);

    for (const FString& ServerName : ServerNames)
    {
        const TSharedPtr<FJsonObject>* ServerObject = nullptr;
        if (!(*ServersObject)->TryGetObjectField(ServerName, ServerObject) || ServerObject == nullptr || !ServerObject->IsValid())
        {
            continue;
        }

        const FString Prefix = FString::Printf(TEXT("mcp_servers.%s."), *ServerName);

        FString Command;
        if ((*ServerObject)->TryGetStringField(TEXT("command"), Command))
        {
            const FString ExpectedOverride = FString::Printf(
                TEXT("\"%scommand=\\\"%s\\\"\""),
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
                TEXT("\"%sargs=[\\\"%s\\\"]\""),
                *Prefix,
                *FString::Join(Args, TEXT("\\\",\\\"")));
            TestTrue(FString::Printf(TEXT("Should translate args for %s"), *ServerName), Overrides.Contains(ExpectedOverride));
        }

        const TSharedPtr<FJsonObject>* EnvObject = nullptr;
        if ((*ServerObject)->TryGetObjectField(TEXT("env"), EnvObject) && EnvObject != nullptr && EnvObject->IsValid() && (*EnvObject)->Values.Num() > 0)
        {
            for (const auto& EnvPair : (*EnvObject)->Values)
            {
                FString EnvValue;
                if (!EnvPair.Value.IsValid() || !EnvPair.Value->TryGetString(EnvValue))
                {
                    continue;
                }

                const FString EnvKey = CortexEngineCompat::JsonKeyToString(EnvPair.Key);
                const FString ExpectedOverride = FString::Printf(
                    TEXT("\"%senv.%s=\\\"%s\\\"\""),
                    *Prefix,
                    *EnvKey,
                    *EnvValue);
                TestTrue(FString::Printf(TEXT("Should translate env %s for %s"), *EnvKey, *ServerName), Overrides.Contains(ExpectedOverride));
            }
        }
    }

    const FString WindowsFixtureDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("CodexTranslatorTest"));
    TestTrue(TEXT("Windows fixture directory should be created"), IFileManager::Get().MakeDirectory(*WindowsFixtureDir, true));

    const FString WindowsFixturePath = FPaths::Combine(WindowsFixtureDir, TEXT("windows-paths.mcp.json"));
    const FString WindowsFixtureJson = TEXT(R"({
  "mcpServers": {
    "windows_path_server": {
      "command": "C:\\Program Files\\OpenAI\\codex.cmd",
      "args": [
        "run",
        "--directory",
        "D:\\UnrealProjects\\Cortex Sandbox\\Plugins\\UnrealCortex\\MCP"
      ],
      "env": {
        "CORTEX_PROJECT_DIR": "D:\\UnrealProjects\\Cortex Sandbox"
      }
    }
  }
})");

    TestTrue(TEXT("Windows fixture should save"), FFileHelper::SaveStringToFile(WindowsFixtureJson, *WindowsFixturePath));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*WindowsFixturePath, false, true);
        IFileManager::Get().DeleteDirectory(*WindowsFixtureDir, false, true);
    };

    const TArray<FString> WindowsOverrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(WindowsFixturePath);
    TestTrue(TEXT("Windows fixture should produce overrides"), WindowsOverrides.Num() > 0);
    TestTrue(TEXT("Windows command should keep apostrophes and backslashes"), WindowsOverrides.Contains(TEXT("\"mcp_servers.windows_path_server.command=\\\"C:\\\\Program Files\\\\OpenAI\\\\codex.cmd\\\"\"")));
    TestTrue(TEXT("Windows args should keep apostrophes and backslashes"), WindowsOverrides.Contains(TEXT("\"mcp_servers.windows_path_server.args=[\\\"run\\\",\\\"--directory\\\",\\\"D:\\\\UnrealProjects\\\\Cortex Sandbox\\\\Plugins\\\\UnrealCortex\\\\MCP\\\"]\"")));
    TestTrue(TEXT("Windows env should keep apostrophes and backslashes"), WindowsOverrides.Contains(TEXT("\"mcp_servers.windows_path_server.env.CORTEX_PROJECT_DIR=\\\"D:\\\\UnrealProjects\\\\Cortex Sandbox\\\"\"")));

    const FString ApostropheFixtureDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("CodexTranslatorTestApostrophe"));
    TestTrue(TEXT("Apostrophe fixture directory should be created"), IFileManager::Get().MakeDirectory(*ApostropheFixtureDir, true));

    const FString ApostropheFixturePath = FPaths::Combine(ApostropheFixtureDir, TEXT("apostrophe-paths.mcp.json"));
    const FString ApostropheFixtureJson = TEXT(R"({
  "mcpServers": {
    "apostrophe_server": {
      "command": "C:\\Users\\O'Connor\\AppData\\Local\\Programs\\OpenAI\\codex.cmd",
      "args": [
        "run",
        "--directory",
        "C:\\Users\\O'Connor\\Unreal Projects\\Cortex Sandbox"
      ],
      "env": {
        "CORTEX_PROJECT_DIR": "C:\\Users\\O'Connor\\Unreal Projects\\Cortex Sandbox"
      }
    }
  }
})");

    TestTrue(TEXT("Apostrophe fixture should save"), FFileHelper::SaveStringToFile(ApostropheFixtureJson, *ApostropheFixturePath));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ApostropheFixturePath, false, true);
        IFileManager::Get().DeleteDirectory(*ApostropheFixtureDir, false, true);
    };

    const TArray<FString> ApostropheOverrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(ApostropheFixturePath);
    TestTrue(TEXT("Apostrophe fixture should produce overrides"), ApostropheOverrides.Num() > 0);
    TestTrue(TEXT("Apostrophe command should preserve the apostrophe"), ApostropheOverrides.Contains(TEXT("\"mcp_servers.apostrophe_server.command=\\\"C:\\\\Users\\\\O'Connor\\\\AppData\\\\Local\\\\Programs\\\\OpenAI\\\\codex.cmd\\\"\"")));
    TestTrue(TEXT("Apostrophe args should preserve the apostrophe"), ApostropheOverrides.Contains(TEXT("\"mcp_servers.apostrophe_server.args=[\\\"run\\\",\\\"--directory\\\",\\\"C:\\\\Users\\\\O'Connor\\\\Unreal Projects\\\\Cortex Sandbox\\\"]\"")));
    TestTrue(TEXT("Apostrophe env should preserve the apostrophe"), ApostropheOverrides.Contains(TEXT("\"mcp_servers.apostrophe_server.env.CORTEX_PROJECT_DIR=\\\"C:\\\\Users\\\\O'Connor\\\\Unreal Projects\\\\Cortex Sandbox\\\"\"")));

    const FString OrderingFixtureDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexFrontend"), TEXT("CodexTranslatorOrdering"));
    TestTrue(TEXT("Ordering fixture directory should be created"), IFileManager::Get().MakeDirectory(*OrderingFixtureDir, true));

    const FString OrderingFixturePath = FPaths::Combine(OrderingFixtureDir, TEXT("ordering.mcp.json"));
    const FString OrderingFixtureJson = TEXT(R"({
  "mcpServers": {
    "zeta_server": {
      "command": "cmd.exe",
      "env": {
        "ZETA": "1",
        "ALPHA": "2"
      }
    },
    "alpha_server": {
      "command": "cmd.exe",
      "env": {
        "ZETA": "1",
        "ALPHA": "2"
      }
    }
  }
})");

    TestTrue(TEXT("Ordering fixture should save"), FFileHelper::SaveStringToFile(OrderingFixtureJson, *OrderingFixturePath));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*OrderingFixturePath, false, true);
        IFileManager::Get().DeleteDirectory(*OrderingFixtureDir, false, true);
    };

    const TArray<FString> OrderingOverrides = FCortexMcpConfigTranslator::BuildCodexConfigOverrides(OrderingFixturePath);
    TArray<FString> OverrideValuesOnly;
    for (int32 Index = 1; Index < OrderingOverrides.Num(); Index += 2)
    {
        OverrideValuesOnly.Add(OrderingOverrides[Index]);
    }
    TestTrue(TEXT("Ordering overrides produced"), OverrideValuesOnly.Num() == 6);
    if (OverrideValuesOnly.Num() == 6)
    {
        TestEqual(TEXT("Alpha server command first"), OverrideValuesOnly[0], FString(TEXT("\"mcp_servers.alpha_server.command=\\\"cmd.exe\\\"\"")));
        TestEqual(TEXT("Alpha server env ALPHA second"), OverrideValuesOnly[1], FString(TEXT("\"mcp_servers.alpha_server.env.ALPHA=\\\"2\\\"\"")));
        TestEqual(TEXT("Alpha server env ZETA third"), OverrideValuesOnly[2], FString(TEXT("\"mcp_servers.alpha_server.env.ZETA=\\\"1\\\"\"")));
        TestEqual(TEXT("Zeta server command fourth"), OverrideValuesOnly[3], FString(TEXT("\"mcp_servers.zeta_server.command=\\\"cmd.exe\\\"\"")));
        TestEqual(TEXT("Zeta server env ALPHA fifth"), OverrideValuesOnly[4], FString(TEXT("\"mcp_servers.zeta_server.env.ALPHA=\\\"2\\\"\"")));
        TestEqual(TEXT("Zeta server env ZETA sixth"), OverrideValuesOnly[5], FString(TEXT("\"mcp_servers.zeta_server.env.ZETA=\\\"1\\\"\"")));
    }

    return true;
}
