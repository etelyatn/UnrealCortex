#include "Misc/AutomationTest.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/SCortexToolCallBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolCallGroupingTest,
    "Cortex.Frontend.ToolCallBlock.Grouping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolCallGroupingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TArray<TSharedPtr<FCortexChatEntry>> Entries;

    auto MakeToolCall = [](int32 Turn, const FString& Name) -> TSharedPtr<FCortexChatEntry>
    {
        TSharedPtr<FCortexChatEntry> Entry = MakeShared<FCortexChatEntry>();
        Entry->Type = ECortexChatEntryType::ToolCall;
        Entry->ToolName = Name;
        Entry->TurnIndex = Turn;
        Entry->bIsToolComplete = true;
        Entry->DurationMs = 100;
        return Entry;
    };

    Entries.Add(MakeToolCall(1, TEXT("list_actors")));
    Entries.Add(MakeToolCall(1, TEXT("get_actor")));
    Entries.Add(MakeToolCall(1, TEXT("set_property")));
    Entries.Add(MakeToolCall(2, TEXT("compile_blueprint")));

    TMap<int32, TArray<TSharedPtr<FCortexChatEntry>>> Groups;
    for (const auto& Entry : Entries)
    {
        Groups.FindOrAdd(Entry->TurnIndex).Add(Entry);
    }

    TestEqual(TEXT("Should have 2 turn groups"), Groups.Num(), 2);
    TestEqual(TEXT("Turn 1 should have 3 calls"), Groups[1].Num(), 3);
    TestEqual(TEXT("Turn 2 should have 1 call"), Groups[2].Num(), 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexToolCallCategorizeTest,
    "Cortex.Frontend.ToolCallBlock.CategorizeToolCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexToolCallCategorizeTest::RunTest(const FString& Parameters)
{
    auto Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("Read"));
    TestEqual(TEXT("Read label"), Cat.Label, TEXT("Read"));

    Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("Glob"));
    TestEqual(TEXT("Glob label"), Cat.Label, TEXT("Search"));

    Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("Edit"));
    TestEqual(TEXT("Edit label"), Cat.Label, TEXT("Edit"));

    Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("Bash"));
    TestEqual(TEXT("Bash label"), Cat.Label, TEXT("Shell"));

    Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("mcp__cortex_mcp__blueprint_cmd"));
    TestEqual(TEXT("MCP label"), Cat.Label, TEXT("MCP"));

    Cat = SCortexToolCallBlock::CategorizeToolCall(TEXT("UnknownTool"));
    TestEqual(TEXT("Default label"), Cat.Label, TEXT("Tool"));

    return true;
}
