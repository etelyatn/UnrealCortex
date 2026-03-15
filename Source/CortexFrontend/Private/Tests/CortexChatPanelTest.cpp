#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Modules/ModuleManager.h"
#include "CortexFrontendModule.h"
#include "Widgets/SCortexChatPanel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelConstructTest, "Cortex.Frontend.ChatPanel.Construct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelSessionInitTest, "Cortex.Frontend.ChatPanel.BindsModuleSession", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelFailureCleanupTest, "Cortex.Frontend.ChatPanel.FailureRemovesEmptyStreamingEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelCodeBlockTest, "Cortex.Frontend.ChatPanel.SuccessMaterializesCodeBlocks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelRejectedSendDoesNotAppendEntriesTest, "Cortex.Frontend.ChatPanel.RejectedSendDoesNotAppendEntries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatPanelConstructTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TestTrue(TEXT("Panel should be visible"), Panel->GetVisibility() != EVisibility::Hidden);
    TestTrue(TEXT("Panel should acquire a session"), Panel->SessionWeak.Pin().IsValid());
    return true;
}

bool FCortexChatPanelSessionInitTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
    const TSharedPtr<FCortexCliSession> ModuleSession = Module.GetOrCreateSession().Pin();
    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TestTrue(TEXT("Panel should bind the module session"), Panel->SessionWeak.Pin() == ModuleSession);
    return true;
}

bool FCortexChatPanelFailureCleanupTest::RunTest(const FString& Parameters)
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
    Session->AddUserPromptEntry(TEXT("Hello"));
    Panel->RefreshVisibleEntries();

    FCortexTurnResult Result;
    Result.ResultText = TEXT("Failed to start Claude process");
    Result.bIsError = true;
    Panel->OnTurnComplete(Result);

    TestEqual(TEXT("Failure should produce 2 display rows"), Panel->DisplayRows.Num(), 2);
    if (Panel->DisplayRows.Num() == 2)
    {
        TestEqual(TEXT("Row 1 should be assistant error"),
            Panel->DisplayRows[1]->PrimaryEntry->Text,
            FString(TEXT("Error: Failed to start Claude process")));
    }
    return true;
}

bool FCortexChatPanelCodeBlockTest::RunTest(const FString& Parameters)
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
    Session->AddUserPromptEntry(TEXT("Show code"));
    Panel->RefreshVisibleEntries();

    FCortexTurnResult Result;
    Result.ResultText = TEXT("Before\n```cpp\nint Value = 42;\n```\nAfter");
    Panel->OnTurnComplete(Result);

    bool bFoundCodeBlock = false;
    bool bFoundLeadingText = false;
    bool bFoundTrailingText = false;
    for (const TSharedPtr<FCortexChatDisplayRow>& Row : Panel->DisplayRows)
    {
        if (Row->RowType == ECortexChatRowType::CodeBlock && Row->PrimaryEntry->Text.Contains(TEXT("int Value = 42;")))
        {
            bFoundCodeBlock = true;
        }
        if (Row->RowType == ECortexChatRowType::AssistantTurn && Row->PrimaryEntry->Text.Contains(TEXT("Before")))
        {
            bFoundLeadingText = true;
        }
        if (Row->RowType == ECortexChatRowType::AssistantTurn && Row->PrimaryEntry->Text.Contains(TEXT("After")))
        {
            bFoundTrailingText = true;
        }
    }

    TestTrue(TEXT("Success should materialize a code block entry"), bFoundCodeBlock);
    TestTrue(TEXT("Success should preserve text before code block"), bFoundLeadingText);
    TestTrue(TEXT("Success should preserve text after code block"), bFoundTrailingText);
    return true;
}

bool FCortexChatPanelRejectedSendDoesNotAppendEntriesTest::RunTest(const FString& Parameters)
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
    Session->SetStateForTest(ECortexSessionState::Terminated);

    Panel->SendMessage(TEXT("Should be rejected"));

    TestEqual(TEXT("Rejected sends should not append display rows"), Panel->DisplayRows.Num(), 0);
    return true;
}
