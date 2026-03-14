#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class ECortexStreamEventType : uint8
{
    SessionInit,
    TextContent,
    ContentBlockDelta,
    ToolUse,
    ToolResult,
    Result,
    SystemError,
    Unknown
};

struct FCortexStreamEvent
{
    ECortexStreamEventType Type = ECortexStreamEventType::Unknown;
    FString Text;
    FString ToolName;
    FString ToolCallId;
    FString ToolInput;
    FString ToolResultContent;
    FString SessionId;
    bool bIsError = false;
    int32 DurationMs = 0;
    int32 NumTurns = 0;
    float TotalCostUsd = 0.0f;
    FString ResultText;
    FString RawJson;
    // Token usage (from message_start/message_delta usage objects)
    int64 InputTokens = 0;
    int64 OutputTokens = 0;
    int64 CacheReadTokens = 0;
    int64 CacheCreationTokens = 0;
    // Model info (from system.init)
    FString Model;
};

namespace CortexStreamEventParser
{
    TArray<FCortexStreamEvent> ParseNdjsonLine(const FString& JsonLine);
}
