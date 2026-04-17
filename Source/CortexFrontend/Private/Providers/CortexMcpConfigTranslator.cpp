#include "Providers/CortexMcpConfigTranslator.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString QuoteTomlString(const FString& Value)
    {
        FString Escaped = Value;
        Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    FString BuildTomlArray(const TArray<FString>& Values)
    {
        FString Result = TEXT("[");
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            if (Index > 0)
            {
                Result += TEXT(", ");
            }
            Result += QuoteTomlString(Values[Index]);
        }
        Result += TEXT("]");
        return Result;
    }

    void AppendCodexEnvOverrides(
        const TSharedPtr<FJsonObject>& CortexServerObject,
        TArray<FString>& OutOverrides)
    {
        const TSharedPtr<FJsonObject>* EnvObject = nullptr;
        if (!CortexServerObject->TryGetObjectField(TEXT("env"), EnvObject) || EnvObject == nullptr)
        {
            return;
        }

        TArray<FString> EnvKeys;
        (*EnvObject)->Values.GetKeys(EnvKeys);
        EnvKeys.Sort();

        for (const FString& EnvKey : EnvKeys)
        {
            const TSharedPtr<FJsonValue>* EnvValue = (*EnvObject)->Values.Find(EnvKey);
            if (EnvValue == nullptr || !EnvValue->IsValid())
            {
                continue;
            }

            FString EnvText;
            const TSharedPtr<FJsonValue>& ValueRef = *EnvValue;
            if (ValueRef->TryGetString(EnvText))
            {
            }
            else if (double NumberValue = 0.0; ValueRef->TryGetNumber(NumberValue))
            {
                EnvText = FString::SanitizeFloat(NumberValue);
            }
            else if (bool BoolValue = false; ValueRef->TryGetBool(BoolValue))
            {
                EnvText = BoolValue ? TEXT("true") : TEXT("false");
            }
            else
            {
                continue;
            }

            OutOverrides.Add(FString::Printf(
                TEXT("-c mcp_servers.cortex_mcp.env.%s=%s"),
                *EnvKey,
                *QuoteTomlString(EnvText)));
        }
    }

    bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJsonObject)
    {
        FString JsonText;
        if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
        {
            return false;
        }

        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
        return FJsonSerializer::Deserialize(Reader, OutJsonObject) && OutJsonObject.IsValid();
    }
}

TArray<FString> FCortexMcpConfigTranslator::BuildClaudeArgs(const FString& McpConfigPath)
{
    TArray<FString> Args;
    if (McpConfigPath.IsEmpty())
    {
        return Args;
    }

    Args.Add(TEXT("--mcp-config"));
    Args.Add(FString::Printf(TEXT("\"%s\""), *McpConfigPath.Replace(TEXT("\\"), TEXT("/"))));
    return Args;
}

TArray<FString> FCortexMcpConfigTranslator::BuildCodexConfigOverrides(const FString& McpConfigPath)
{
    TArray<FString> Overrides;

    TSharedPtr<FJsonObject> RootObject;
    if (!LoadJsonFile(McpConfigPath, RootObject))
    {
        return Overrides;
    }

    const TSharedPtr<FJsonObject>* McpServersObject = nullptr;
    if (!RootObject->TryGetObjectField(TEXT("mcpServers"), McpServersObject) || McpServersObject == nullptr)
    {
        RootObject->TryGetObjectField(TEXT("mcp_servers"), McpServersObject);
    }

    if (McpServersObject == nullptr)
    {
        return Overrides;
    }

    const TSharedPtr<FJsonObject>* CortexServerObject = nullptr;
    if (!(*McpServersObject)->TryGetObjectField(TEXT("cortex_mcp"), CortexServerObject) || CortexServerObject == nullptr)
    {
        return Overrides;
    }

    FString Command;
    if ((*CortexServerObject)->TryGetStringField(TEXT("command"), Command))
    {
        Overrides.Add(FString::Printf(TEXT("-c mcp_servers.cortex_mcp.command=%s"), *QuoteTomlString(Command)));
    }

    const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
    if ((*CortexServerObject)->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray != nullptr)
    {
        TArray<FString> Args;
        Args.Reserve(ArgsArray->Num());
        for (const TSharedPtr<FJsonValue>& ArgValue : *ArgsArray)
        {
            FString Arg;
            if (ArgValue.IsValid() && ArgValue->TryGetString(Arg))
            {
                Args.Add(Arg);
            }
        }

        if (Args.Num() > 0)
        {
            Overrides.Add(FString::Printf(TEXT("-c mcp_servers.cortex_mcp.args=%s"), *BuildTomlArray(Args)));
        }
    }

    AppendCodexEnvOverrides(*CortexServerObject, Overrides);

    return Overrides;
}
