#include "Providers/CortexMcpConfigTranslator.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString JsonValueToTomlLiteral(const TSharedPtr<FJsonValue>& Value);

    FString QuoteTomlString(const FString& Value)
    {
        FString Escaped = Value;
        Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    FString QuoteCodexConfigValue(const FString& Value)
    {
        FString Escaped = Value;
        Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    FString JsonValueToTomlLiteral(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("\"\"");
        }

        switch (Value->Type)
        {
        case EJson::String:
        {
            FString StringValue;
            Value->TryGetString(StringValue);
            return QuoteTomlString(StringValue);
        }

        case EJson::Boolean:
        {
            bool bBoolValue = false;
            Value->TryGetBool(bBoolValue);
            return bBoolValue ? TEXT("true") : TEXT("false");
        }

        case EJson::Number:
        {
            double NumberValue = 0.0;
            Value->TryGetNumber(NumberValue);
            return FString::SanitizeFloat(NumberValue);
        }

        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& JsonArray = Value->AsArray();
            TArray<FString> Values;
            Values.Reserve(JsonArray.Num());
            for (const TSharedPtr<FJsonValue>& Element : JsonArray)
            {
                Values.Add(JsonValueToTomlLiteral(Element));
            }
            return FString::Printf(TEXT("[%s]"), *FString::Join(Values, TEXT(",")));
        }

        default:
            return TEXT("\"\"");
        }
    }

    void AddCodexConfigOverride(TArray<FString>& OutOverrides, const FString& Value)
    {
        OutOverrides.Add(TEXT("-c"));
        OutOverrides.Add(QuoteCodexConfigValue(Value));
    }

    void AppendCodexEnvOverrideValues(
        const FString& ServerName,
        const TSharedPtr<FJsonObject>& ServerObject,
        TMap<FString, TSharedPtr<FJsonValue>>& OutOverrideValues)
    {
        const TSharedPtr<FJsonObject>* EnvObject = nullptr;
        if (!ServerObject->TryGetObjectField(TEXT("env"), EnvObject) || EnvObject == nullptr)
        {
            return;
        }

        TArray<FString> EnvKeys;
        for (const auto& Entry : (*EnvObject)->Values)
        {
            EnvKeys.Emplace(Entry.Key.ToView());
        }
        EnvKeys.Sort();

        for (const FString& EnvKey : EnvKeys)
        {
            const TSharedPtr<FJsonValue>* EnvValue = (*EnvObject)->Values.Find(UE::FSharedString(EnvKey));
            if (EnvValue == nullptr || !EnvValue->IsValid())
            {
                continue;
            }
            OutOverrideValues.Add(
                FString::Printf(TEXT("mcp_servers.%s.env.%s"), *ServerName, *EnvKey),
                *EnvValue);
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

    bool LoadMcpServersObject(const TSharedPtr<FJsonObject>& RootObject, const TSharedPtr<FJsonObject>*& OutMcpServersObject)
    {
        OutMcpServersObject = nullptr;
        if (RootObject.IsValid())
        {
            if (RootObject->TryGetObjectField(TEXT("mcpServers"), OutMcpServersObject) && OutMcpServersObject != nullptr)
            {
                return true;
            }

            if (RootObject->TryGetObjectField(TEXT("mcp_servers"), OutMcpServersObject) && OutMcpServersObject != nullptr)
            {
                return true;
            }
        }

        return false;
    }

    void AppendServerOverrideValues(
        const FString& ServerName,
        const TSharedPtr<FJsonObject>& ServerObject,
        TMap<FString, TSharedPtr<FJsonValue>>& OutOverrideValues)
    {
        FString Command;
        if (ServerObject->TryGetStringField(TEXT("command"), Command))
        {
            OutOverrideValues.Add(
                FString::Printf(TEXT("mcp_servers.%s.command"), *ServerName),
                MakeShared<FJsonValueString>(Command));
        }

        const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
        if (ServerObject->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray != nullptr)
        {
            TArray<TSharedPtr<FJsonValue>> Args;
            Args.Reserve(ArgsArray->Num());
            for (const TSharedPtr<FJsonValue>& ArgValue : *ArgsArray)
            {
                FString Arg;
                if (ArgValue.IsValid() && ArgValue->TryGetString(Arg))
                {
                    Args.Add(MakeShared<FJsonValueString>(Arg));
                }
            }

            if (Args.Num() > 0)
            {
                OutOverrideValues.Add(
                    FString::Printf(TEXT("mcp_servers.%s.args"), *ServerName),
                    MakeShared<FJsonValueArray>(Args));
            }
        }

        AppendCodexEnvOverrideValues(ServerName, ServerObject, OutOverrideValues);
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
    const TMap<FString, TSharedPtr<FJsonValue>> OverrideValues = BuildCodexConfigOverrideValues(McpConfigPath);

    TArray<FString> Keys;
    OverrideValues.GetKeys(Keys);
    Keys.Sort();
    for (const FString& Key : Keys)
    {
        const TSharedPtr<FJsonValue>* Value = OverrideValues.Find(Key);
        if (Value == nullptr || !Value->IsValid())
        {
            continue;
        }

        AddCodexConfigOverride(Overrides, FString::Printf(
            TEXT("%s=%s"),
            *Key,
            *JsonValueToTomlLiteral(*Value)));
    }

    return Overrides;
}

TMap<FString, TSharedPtr<FJsonValue>> FCortexMcpConfigTranslator::BuildCodexConfigOverrideValues(const FString& McpConfigPath)
{
    TMap<FString, TSharedPtr<FJsonValue>> OverrideValues;
    TSharedPtr<FJsonObject> RootObject;
    if (!LoadJsonFile(McpConfigPath, RootObject))
    {
        return OverrideValues;
    }

    const TSharedPtr<FJsonObject>* McpServersObject = nullptr;
    if (!LoadMcpServersObject(RootObject, McpServersObject) || McpServersObject == nullptr)
    {
        return OverrideValues;
    }

    TArray<FString> ServerNames;
    for (const auto& Entry : (*McpServersObject)->Values)
    {
        ServerNames.Emplace(Entry.Key.ToView());
    }
    ServerNames.Sort();

    for (const FString& ServerName : ServerNames)
    {
        const TSharedPtr<FJsonObject>* ServerObject = nullptr;
        if ((*McpServersObject)->TryGetObjectField(ServerName, ServerObject) && ServerObject != nullptr)
        {
            AppendServerOverrideValues(ServerName, *ServerObject, OverrideValues);
        }
    }

    return OverrideValues;
}
