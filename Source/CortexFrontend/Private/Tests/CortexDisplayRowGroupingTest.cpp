#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Modules/ModuleManager.h"
#include "CortexFrontendModule.h"
#include "Widgets/SCortexChatPanel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDisplayRowGroupingTest,
    "Cortex.Frontend.ChatPanel.DisplayRowGrouping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDisplayRowGroupingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TSharedPtr<FCortexCliSession> Session = Panel->SessionWeak.Pin();
    Session->ClearConversation();

    // Simulate: user message -> 2 tool calls -> assistant reply
    Session->AddUserPromptEntry(TEXT("Do something"));

    // Add tool calls at same turn index
    TSharedPtr<FCortexChatEntry> Tool1 = MakeShared<FCortexChatEntry>();
    Tool1->Type = ECortexChatEntryType::ToolCall;
    Tool1->ToolName = TEXT("list_actors");
    Tool1->TurnIndex = 1;
    Tool1->bIsToolComplete = true;
    Session->GetChatEntriesMutable().Insert(Tool1, Session->GetChatEntries().Num() - 1);

    TSharedPtr<FCortexChatEntry> Tool2 = MakeShared<FCortexChatEntry>();
    Tool2->Type = ECortexChatEntryType::ToolCall;
    Tool2->ToolName = TEXT("get_actor");
    Tool2->TurnIndex = 1;
    Tool2->bIsToolComplete = true;
    Session->GetChatEntriesMutable().Insert(Tool2, Session->GetChatEntries().Num() - 1);

    // Complete the turn
    FCortexTurnResult Result;
    Result.ResultText = TEXT("Here is the response");
    Panel->OnTurnComplete(Result);

    // Verify display rows
    // Expected: UserMessage row, AssistantTurn row (with 2 tool calls + text)
    TestEqual(TEXT("Should have 2 display rows"), Panel->DisplayRows.Num(), 2);

    if (Panel->DisplayRows.Num() >= 2)
    {
        TestEqual(TEXT("Row 0 should be UserMessage"),
            static_cast<uint8>(Panel->DisplayRows[0]->RowType),
            static_cast<uint8>(ECortexChatRowType::UserMessage));

        TestEqual(TEXT("Row 1 should be AssistantTurn"),
            static_cast<uint8>(Panel->DisplayRows[1]->RowType),
            static_cast<uint8>(ECortexChatRowType::AssistantTurn));

        TestEqual(TEXT("Row 1 should have 2 tool calls"),
            Panel->DisplayRows[1]->ToolCalls.Num(), 2);
    }

    return true;
}
