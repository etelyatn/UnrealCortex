#pragma once

#include "CoreMinimal.h"
#include <atomic>

enum class ECortexAccessMode : uint8
{
    ReadOnly,
    Guided,
    FullAccess
};

enum class ECortexEffortLevel : uint8
{
    Default,
    Low,
    Medium,
    High,
    Maximum
};

enum class ECortexWorkflowMode : uint8
{
    Direct,
    Thorough
};

enum class ECortexSessionState : uint8
{
    Inactive,
    Spawning,
    Idle,
    Processing,
    Cancelling,
    Respawning,
    Terminated
};

struct FCortexSessionConfig
{
    FString SessionId;
    FString WorkingDirectory;
    FString McpConfigPath;
    FString SystemPrompt;  // Optional system prompt override (used by conversion tabs)
    bool bSkipPermissions = true;
    bool bConversionMode = false;  // Lightweight mode: no MCP, no project context, no tools
};

struct FCortexPromptRequest
{
    FString Prompt;
    ECortexAccessMode AccessMode = ECortexAccessMode::ReadOnly;
};

struct FCortexTurnResult
{
    FString ResultText;
    bool bIsError = false;
    int32 DurationMs = 0;
    int32 NumTurns = 0;
    double TotalCostUsd = 0.0;
    FString SessionId;
};

struct FCortexSessionStateChange
{
    ECortexSessionState PreviousState = ECortexSessionState::Inactive;
    ECortexSessionState NewState = ECortexSessionState::Inactive;
    FString Reason;
};

enum class ECortexChatEntryType : uint8
{
    UserMessage,
    AssistantMessage,
    ToolCall,
    CodeBlock,
    Table,
    AuthError
};

struct FCortexChatEntry
{
    ECortexChatEntryType Type = ECortexChatEntryType::AssistantMessage;
    FString Text;
    FString Language;
    FString CodeBlockTarget;  // "header", "implementation", "snippet", or empty
    FString ToolName;
    FString ToolInput;
    FString ToolResult;
    FString ToolCallId;
    int32 TurnIndex = 0;
    int32 DurationMs = 0;
    double ToolStartTime = 0.0;
    bool bIsToolComplete = false;
    TArray<FString> TableHeaders;
    TArray<TArray<FString>> TableRows;
};
