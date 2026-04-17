#pragma once

#include "CoreMinimal.h"

class FCortexMcpConfigTranslator
{
public:
    static TArray<FString> BuildClaudeArgs(const FString& McpConfigPath);
    static TArray<FString> BuildCodexConfigOverrides(const FString& McpConfigPath);
};
