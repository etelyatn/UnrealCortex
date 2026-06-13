#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FCortexMcpConfigTranslator
{
public:
    static TArray<FString> BuildClaudeArgs(const FString& McpConfigPath);
    static TArray<FString> BuildCodexConfigOverrides(const FString& McpConfigPath);
    static TMap<FString, TSharedPtr<FJsonValue>> BuildCodexConfigOverrideValues(const FString& McpConfigPath);
};
