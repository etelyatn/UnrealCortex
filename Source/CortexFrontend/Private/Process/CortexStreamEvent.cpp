#include "Process/CortexStreamEvent.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TArray<FCortexStreamEvent> CortexStreamEventParser::ParseNdjsonLine(const FString& JsonLine)
{
    TArray<FCortexStreamEvent> Events;

    if (JsonLine.IsEmpty())
    {
        return Events;
    }

    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonLine);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
    {
        return Events;
    }

    FString Type;
    if (!JsonObj->TryGetStringField(TEXT("type"), Type))
    {
        return Events;
    }

    if (Type == TEXT("system"))
    {
        FString SubType;
        JsonObj->TryGetStringField(TEXT("subtype"), SubType);
        if (SubType == TEXT("init"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::SessionInit;
            JsonObj->TryGetStringField(TEXT("session_id"), Event.SessionId);
            JsonObj->TryGetStringField(TEXT("model"), Event.Model);
            Event.RawJson = JsonLine;
            Events.Add(MoveTemp(Event));
        }
        else if (SubType == TEXT("error") || SubType == TEXT("warning"))
        {
            FCortexStreamEvent Event;
            Event.Type = ECortexStreamEventType::SystemError;
            JsonObj->TryGetStringField(TEXT("message"), Event.Text);
            Event.bIsError = SubType == TEXT("error");
            Event.RawJson = JsonLine;
            Events.Add(MoveTemp(Event));
        }
        return Events;
    }

    // Claude CLI wraps streaming events in {"type":"stream_event","event":{...}}.
    // Unwrap and extract content_block_delta for incremental text streaming.
    if (Type == TEXT("stream_event"))
    {
        const TSharedPtr<FJsonObject>* InnerEventObj = nullptr;
        if (JsonObj->TryGetObjectField(TEXT("event"), InnerEventObj) && InnerEventObj != nullptr)
        {
            FString InnerType;
            if ((*InnerEventObj)->TryGetStringField(TEXT("type"), InnerType) && InnerType == TEXT("content_block_delta"))
            {
                const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
                if ((*InnerEventObj)->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj != nullptr)
                {
                    FString DeltaType;
                    if ((*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType) && DeltaType == TEXT("text_delta"))
                    {
                        FCortexStreamEvent Event;
                        Event.Type = ECortexStreamEventType::ContentBlockDelta;
                        (*DeltaObj)->TryGetStringField(TEXT("text"), Event.Text);
                        Event.RawJson = JsonLine;
                        Events.Add(MoveTemp(Event));
                    }
                }
            }
        }
        return Events;
    }

    // Legacy format: content_block_delta at root level (kept for backward compatibility)
    if (Type == TEXT("content_block_delta"))
    {
        const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
        if (JsonObj->TryGetObjectField(TEXT("delta"), DeltaObj) && DeltaObj != nullptr)
        {
            FString DeltaType;
            if ((*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType) && DeltaType == TEXT("text_delta"))
            {
                FCortexStreamEvent Event;
                Event.Type = ECortexStreamEventType::ContentBlockDelta;
                (*DeltaObj)->TryGetStringField(TEXT("text"), Event.Text);
                Event.RawJson = JsonLine;
                Events.Add(MoveTemp(Event));
            }
        }
        return Events;
    }

    if (Type == TEXT("assistant") || Type == TEXT("user"))
    {
        const TSharedPtr<FJsonObject>* MessageObj = nullptr;
        if (!JsonObj->TryGetObjectField(TEXT("message"), MessageObj) || MessageObj == nullptr)
        {
            return Events;
        }

        const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
        if (!(*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray) || ContentArray == nullptr)
        {
            return Events;
        }

        // Extract usage if present on the message object
        int64 MsgInputTokens = 0, MsgOutputTokens = 0, MsgCacheReadTokens = 0, MsgCacheCreationTokens = 0;
        const TSharedPtr<FJsonObject>* UsageObj = nullptr;
        if ((*MessageObj)->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj != nullptr)
        {
            double Val = 0.0;
            if ((*UsageObj)->TryGetNumberField(TEXT("input_tokens"), Val)) MsgInputTokens = static_cast<int64>(Val);
            if ((*UsageObj)->TryGetNumberField(TEXT("output_tokens"), Val)) MsgOutputTokens = static_cast<int64>(Val);
            if ((*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), Val)) MsgCacheReadTokens = static_cast<int64>(Val);
            if ((*UsageObj)->TryGetNumberField(TEXT("cache_creation_input_tokens"), Val)) MsgCacheCreationTokens = static_cast<int64>(Val);
        }

        // For assistant messages, concatenate all text blocks into a single TextContent event
        FString ConcatenatedText;
        TArray<FCortexStreamEvent> NonTextEvents;

        for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
        {
            const TSharedPtr<FJsonObject>* ContentObj = nullptr;
            if (!ContentValue.IsValid() || !ContentValue->TryGetObject(ContentObj) || ContentObj == nullptr)
            {
                continue;
            }

            FString ContentType;
            if (!(*ContentObj)->TryGetStringField(TEXT("type"), ContentType))
            {
                continue;
            }

            if (Type == TEXT("assistant") && ContentType == TEXT("text"))
            {
                FString BlockText;
                (*ContentObj)->TryGetStringField(TEXT("text"), BlockText);
                ConcatenatedText += BlockText;
            }
            else if (Type == TEXT("assistant") && ContentType == TEXT("tool_use"))
            {
                FCortexStreamEvent Event;
                Event.Type = ECortexStreamEventType::ToolUse;
                (*ContentObj)->TryGetStringField(TEXT("name"), Event.ToolName);
                (*ContentObj)->TryGetStringField(TEXT("id"), Event.ToolCallId);

                const TSharedPtr<FJsonObject>* InputObj = nullptr;
                if ((*ContentObj)->TryGetObjectField(TEXT("input"), InputObj) && InputObj != nullptr)
                {
                    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Event.ToolInput);
                    FJsonSerializer::Serialize((*InputObj).ToSharedRef(), Writer);
                    Writer->Close();
                }

                Event.RawJson = JsonLine;
                NonTextEvents.Add(MoveTemp(Event));
            }
            else if (Type == TEXT("user") && ContentType == TEXT("tool_result"))
            {
                FCortexStreamEvent Event;
                Event.Type = ECortexStreamEventType::ToolResult;
                (*ContentObj)->TryGetStringField(TEXT("tool_use_id"), Event.ToolCallId);

                if (!(*ContentObj)->TryGetStringField(TEXT("content"), Event.ToolResultContent))
                {
                    const TArray<TSharedPtr<FJsonValue>>* ResultArray = nullptr;
                    if ((*ContentObj)->TryGetArrayField(TEXT("content"), ResultArray) && ResultArray != nullptr)
                    {
                        for (const TSharedPtr<FJsonValue>& Block : *ResultArray)
                        {
                            const TSharedPtr<FJsonObject>* BlockObj = nullptr;
                            if (!Block.IsValid() || !Block->TryGetObject(BlockObj) || BlockObj == nullptr)
                            {
                                continue;
                            }

                            FString BlockType;
                            if ((*BlockObj)->TryGetStringField(TEXT("type"), BlockType) && BlockType == TEXT("text"))
                            {
                                FString BlockText;
                                if ((*BlockObj)->TryGetStringField(TEXT("text"), BlockText))
                                {
                                    if (!Event.ToolResultContent.IsEmpty())
                                    {
                                        Event.ToolResultContent += TEXT("\n");
                                    }
                                    Event.ToolResultContent += BlockText;
                                }
                            }
                        }
                    }
                }

                Event.RawJson = JsonLine;
                NonTextEvents.Add(MoveTemp(Event));
            }
        }

        // Emit concatenated text first, then tool events
        if (!ConcatenatedText.IsEmpty())
        {
            FCortexStreamEvent TextEvent;
            TextEvent.Type = ECortexStreamEventType::TextContent;
            TextEvent.Text = ConcatenatedText;
            TextEvent.InputTokens = MsgInputTokens;
            TextEvent.OutputTokens = MsgOutputTokens;
            TextEvent.CacheReadTokens = MsgCacheReadTokens;
            TextEvent.CacheCreationTokens = MsgCacheCreationTokens;
            TextEvent.RawJson = JsonLine;
            Events.Add(MoveTemp(TextEvent));
        }
        else if ((MsgInputTokens > 0 || MsgOutputTokens > 0) && NonTextEvents.Num() > 0)
        {
            // No text content but usage data present — attach to first non-text event
            NonTextEvents[0].InputTokens = MsgInputTokens;
            NonTextEvents[0].OutputTokens = MsgOutputTokens;
            NonTextEvents[0].CacheReadTokens = MsgCacheReadTokens;
            NonTextEvents[0].CacheCreationTokens = MsgCacheCreationTokens;
        }
        Events.Append(MoveTemp(NonTextEvents));

        return Events;
    }

    if (Type == TEXT("result"))
    {
        FCortexStreamEvent Event;
        Event.Type = ECortexStreamEventType::Result;
        JsonObj->TryGetStringField(TEXT("result"), Event.ResultText);
        JsonObj->TryGetBoolField(TEXT("is_error"), Event.bIsError);

        double DurationMs = 0.0;
        JsonObj->TryGetNumberField(TEXT("duration_ms"), DurationMs);
        Event.DurationMs = static_cast<int32>(DurationMs);

        double NumTurns = 0.0;
        JsonObj->TryGetNumberField(TEXT("num_turns"), NumTurns);
        Event.NumTurns = static_cast<int32>(NumTurns);

        double TotalCostUsd = 0.0;
        JsonObj->TryGetNumberField(TEXT("total_cost_usd"), TotalCostUsd);
        Event.TotalCostUsd = static_cast<float>(TotalCostUsd);

        JsonObj->TryGetStringField(TEXT("session_id"), Event.SessionId);
        Event.RawJson = JsonLine;
        Events.Add(MoveTemp(Event));
    }

    return Events;
}
