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

FString FCortexMcpConfigTranslator::BuildClaudeArgs(const FString& McpConfigPath)
{
    if (McpConfigPath.IsEmpty())
    {
        return FString();
    }

    return FString::Printf(TEXT("--mcp-config \"%s\""), *McpConfigPath.Replace(TEXT("\\"), TEXT("/")));
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

    return Overrides;
}
