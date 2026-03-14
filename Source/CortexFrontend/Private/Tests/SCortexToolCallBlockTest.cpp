#include "Misc/AutomationTest.h"
#include "Session/CortexSessionTypes.h"

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
