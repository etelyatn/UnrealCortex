#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class ECortexStreamEventType : uint8
{
    SessionInit,
    TextContent,
    ToolUse,
    ToolResult,
    Result,
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
};

namespace CortexStreamEventParser
{
    TArray<FCortexStreamEvent> ParseNdjsonLine(const FString& JsonLine);
}
