#include "Misc/AutomationTest.h"
#include "Providers/CortexCodexCliProvider.h"
#include "Process/CortexStreamEvent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseSystemTest, "Cortex.Frontend.StreamEvent.ParseSystem", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseTextTest, "Cortex.Frontend.StreamEvent.ParseText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseToolUseTest, "Cortex.Frontend.StreamEvent.ParseToolUse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseToolResultTest, "Cortex.Frontend.StreamEvent.ParseToolResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseResultTest, "Cortex.Frontend.StreamEvent.ParseResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseMixedContentTest, "Cortex.Frontend.StreamEvent.ParseMixedContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseContentBlockDeltaTest, "Cortex.Frontend.StreamEvent.ParseContentBlockDelta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseSystemErrorTest, "Cortex.Frontend.StreamEvent.ParseSystemError", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseEmptyTest, "Cortex.Frontend.StreamEvent.ParseEmpty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseSystemInitModelTest, "Cortex.Frontend.StreamEvent.ParseSystemInitModel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseUsageTest, "Cortex.Frontend.StreamEvent.ParseUsage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseStreamEventDeltaTest, "Cortex.Frontend.StreamEvent.ParseStreamEventDelta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseStreamEventNonDeltaTest, "Cortex.Frontend.StreamEvent.ParseStreamEventNonDelta", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseUsageToolOnlyTest, "Cortex.Frontend.StreamEvent.ParseUsageToolOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventConsumeCodexChunkTest, "Cortex.Frontend.StreamEvent.ConsumeCodexChunk", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexStreamEventParseSystemTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"abc-123\"}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be SessionInit"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::SessionInit));
        TestEqual(TEXT("SessionId should match"), Events[0].SessionId, FString(TEXT("abc-123")));
    }
    return true;
}

bool FCortexStreamEventParseTextTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Hello world\"}]}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be TextContent"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::TextContent));
        TestEqual(TEXT("Text should match"), Events[0].Text, FString(TEXT("Hello world")));
    }
    return true;
}

bool FCortexStreamEventParseToolUseTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"list_actors\",\"input\":{\"level\":\"TestMap\"}}]}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be ToolUse"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::ToolUse));
        TestEqual(TEXT("ToolName should match"), Events[0].ToolName, FString(TEXT("list_actors")));
        TestEqual(TEXT("ToolCallId should match"), Events[0].ToolCallId, FString(TEXT("call_1")));
        TestTrue(TEXT("ToolInput should contain level"), Events[0].ToolInput.Contains(TEXT("TestMap")));
    }
    return true;
}

bool FCortexStreamEventParseToolResultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_1\",\"content\":\"12 actors found\"}]}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be ToolResult"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::ToolResult));
        TestEqual(TEXT("ToolCallId should match"), Events[0].ToolCallId, FString(TEXT("call_1")));
        TestEqual(TEXT("ToolResultContent should match"), Events[0].ToolResultContent, FString(TEXT("12 actors found")));
    }
    return true;
}

bool FCortexStreamEventParseResultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"result\",\"result\":\"Done\",\"is_error\":false,\"duration_ms\":1500,\"num_turns\":2,\"total_cost_usd\":0.05,\"session_id\":\"sess-1\"}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be Result"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::Result));
        TestEqual(TEXT("ResultText should match"), Events[0].ResultText, FString(TEXT("Done")));
        TestEqual(TEXT("bIsError should be false"), Events[0].bIsError, false);
        TestEqual(TEXT("DurationMs should be 1500"), Events[0].DurationMs, 1500);
        TestEqual(TEXT("NumTurns should be 2"), Events[0].NumTurns, 2);
        TestEqual(TEXT("SessionId should match"), Events[0].SessionId, FString(TEXT("sess-1")));
    }
    return true;
}

bool FCortexStreamEventParseMixedContentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Let me check\"},{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"get_status\",\"input\":{}}]}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 2 events"), Events.Num(), 2);
    if (Events.Num() == 2)
    {
        TestEqual(TEXT("First should be TextContent"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::TextContent));
        TestEqual(TEXT("Second should be ToolUse"), static_cast<uint8>(Events[1].Type), static_cast<uint8>(ECortexStreamEventType::ToolUse));
    }
    return true;
}

bool FCortexStreamEventParseContentBlockDeltaTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be ContentBlockDelta"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::ContentBlockDelta));
        TestEqual(TEXT("Text should be the delta"), Events[0].Text, FString(TEXT("Hello ")));
    }
    return true;
}

bool FCortexStreamEventParseSystemErrorTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"system\",\"subtype\":\"error\",\"message\":\"Rate limit exceeded\"}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Type should be SystemError"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::SystemError));
        TestEqual(TEXT("Text should contain error message"), Events[0].Text, FString(TEXT("Rate limit exceeded")));
        TestTrue(TEXT("bIsError should be true"), Events[0].bIsError);
    }
    return true;
}

bool FCortexStreamEventParseEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(TEXT(""));
    TestEqual(TEXT("Empty line should produce 0 events"), Events.Num(), 0);
    Events = CortexStreamEventParser::ParseNdjsonLine(TEXT("not json"));
    TestEqual(TEXT("Invalid JSON should produce 0 events"), Events.Num(), 0);
    Events = CortexStreamEventParser::ParseNdjsonLine(TEXT("{\"foo\":1}"));
    TestEqual(TEXT("Missing type should produce 0 events"), Events.Num(), 0);
    return true;
}

bool FCortexStreamEventParseSystemInitModelTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString JsonLine = TEXT("{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"abc-123\",\"model\":\"claude-sonnet-4-6\"}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
    if (Events.Num() == 1)
    {
        TestEqual(TEXT("Model should be extracted"), Events[0].Model, FString(TEXT("claude-sonnet-4-6")));
    }
    return true;
}

bool FCortexStreamEventParseStreamEventDeltaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Real Claude CLI format: content_block_delta wrapped in stream_event
	const FString JsonLine = TEXT("{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}},\"session_id\":\"sess-1\",\"uuid\":\"abc\"}");
	const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
	TestEqual(TEXT("Should produce 1 event"), Events.Num(), 1);
	if (Events.Num() == 1)
	{
		TestEqual(TEXT("Type should be ContentBlockDelta"), static_cast<uint8>(Events[0].Type), static_cast<uint8>(ECortexStreamEventType::ContentBlockDelta));
		TestEqual(TEXT("Text should be the delta"), Events[0].Text, FString(TEXT("Hi")));
	}
	return true;
}

bool FCortexStreamEventParseStreamEventNonDeltaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Non-delta stream events (message_start, content_block_stop, etc.) should produce 0 events
	const FString JsonLine = TEXT("{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"model\":\"claude-sonnet-4-6\"}},\"session_id\":\"sess-1\"}");
	const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
	TestEqual(TEXT("Non-delta stream_event should produce 0 events"), Events.Num(), 0);
	return true;
}

bool FCortexStreamEventParseUsageTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // assistant message with usage object at message level
    const FString JsonLine = TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}],\"usage\":{\"input_tokens\":1500,\"output_tokens\":200,\"cache_read_input_tokens\":800,\"cache_creation_input_tokens\":100}}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);
    TestTrue(TEXT("Should produce at least 1 event"), Events.Num() >= 1);

    // Find the text event — it should carry the usage data
    const FCortexStreamEvent* TextEvent = Events.FindByPredicate([](const FCortexStreamEvent& E) { return E.Type == ECortexStreamEventType::TextContent; });
    TestNotNull(TEXT("Should have a TextContent event"), TextEvent);
    if (TextEvent)
    {
        TestEqual(TEXT("InputTokens"), TextEvent->InputTokens, (int64)1500);
        TestEqual(TEXT("OutputTokens"), TextEvent->OutputTokens, (int64)200);
        TestEqual(TEXT("CacheReadTokens"), TextEvent->CacheReadTokens, (int64)800);
        TestEqual(TEXT("CacheCreationTokens"), TextEvent->CacheCreationTokens, (int64)100);
    }
    return true;
}

bool FCortexStreamEventParseUsageToolOnlyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Assistant message with ONLY tool_use (no text) but with usage data
    const FString JsonLine = TEXT("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"list_actors\",\"input\":{}}],\"usage\":{\"input_tokens\":2000,\"output_tokens\":150,\"cache_read_input_tokens\":500,\"cache_creation_input_tokens\":50}}}");
    const TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(JsonLine);

    TestTrue(TEXT("Should produce at least 1 event"), Events.Num() >= 1);

    // At least one event must carry the usage data
    bool bFoundUsage = false;
    for (const FCortexStreamEvent& E : Events)
    {
        if (E.InputTokens > 0 || E.OutputTokens > 0)
        {
            bFoundUsage = true;
            TestEqual(TEXT("InputTokens"), E.InputTokens, (int64)2000);
            TestEqual(TEXT("OutputTokens"), E.OutputTokens, (int64)150);
            TestEqual(TEXT("CacheReadTokens"), E.CacheReadTokens, (int64)500);
            TestEqual(TEXT("CacheCreationTokens"), E.CacheCreationTokens, (int64)50);
        }
    }
    TestTrue(TEXT("Usage data must be propagated even without text content"), bFoundUsage);
    return true;
}

bool FCortexStreamEventConsumeCodexChunkTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexCodexCliProvider Provider;
    FString ChunkBuffer;
    FString AssistantTextBuffer;
    TArray<FCortexStreamEvent> Events;

    const FString FirstChunk = TEXT(R"({"type":"thread.started","thread":{"id":"thread-1"}}
{"type":"item.completed","item":{"id":"msg-1","type":"agent_message","text":"Hel)");
    const FString SecondChunk = TEXT(R"(lo"}}
{"type":"turn.completed","usage":{"input_tokens":1,"output_tokens":2}}
)");

    Provider.ConsumeStreamChunk(FirstChunk, ChunkBuffer, AssistantTextBuffer, Events);
    TestEqual(TEXT("Codex chunk consumer should buffer partial JSON without emitting terminal events"), Events.Num(), 1);

    Provider.ConsumeStreamChunk(SecondChunk, ChunkBuffer, AssistantTextBuffer, Events);
    TestEqual(TEXT("Codex chunk consumer should emit session init, text, and result events once the JSON completes"), Events.Num(), 3);
    TestTrue(TEXT("Codex chunk consumer should emit the assistant text once the JSON completes"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::TextContent && Event.Text == TEXT("Hello");
    }));
    TestTrue(TEXT("Codex chunk consumer should emit a terminal result once the JSON completes"), Events.ContainsByPredicate([](const FCortexStreamEvent& Event)
    {
        return Event.Type == ECortexStreamEventType::Result && Event.ResultText == TEXT("Hello") && Event.InputTokens == 1 && Event.OutputTokens == 2;
    }));
    return true;
}
