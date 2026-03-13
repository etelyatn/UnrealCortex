#include "Misc/AutomationTest.h"
#include "Process/CortexStreamEvent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseSystemTest, "Cortex.Frontend.StreamEvent.ParseSystem", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseTextTest, "Cortex.Frontend.StreamEvent.ParseText", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseToolUseTest, "Cortex.Frontend.StreamEvent.ParseToolUse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseToolResultTest, "Cortex.Frontend.StreamEvent.ParseToolResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseResultTest, "Cortex.Frontend.StreamEvent.ParseResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseMixedContentTest, "Cortex.Frontend.StreamEvent.ParseMixedContent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexStreamEventParseEmptyTest, "Cortex.Frontend.StreamEvent.ParseEmpty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
