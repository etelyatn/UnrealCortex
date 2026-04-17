#pragma once

#include "CoreMinimal.h"

class FCortexMcpConfigTranslator
{
public:
    static FString BuildClaudeArgs(const FString& McpConfigPath);
    static TArray<FString> BuildCodexConfigOverrides(const FString& McpConfigPath);
};
