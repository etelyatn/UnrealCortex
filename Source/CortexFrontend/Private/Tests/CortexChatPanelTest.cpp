#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#define private public
#include "Widgets/SCortexChatPanel.h"
#undef private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelConstructTest, "Cortex.Frontend.ChatPanel.Construct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelSessionInitTest, "Cortex.Frontend.ChatPanel.SessionInitUpdatesSessionId", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelFailureCleanupTest, "Cortex.Frontend.ChatPanel.FailureRemovesEmptyStreamingEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelCodeBlockTest, "Cortex.Frontend.ChatPanel.SuccessMaterializesCodeBlocks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    const FString OriginalSessionId = Panel->SessionId;

    FCortexStreamEvent Event;
    Event.Type = ECortexStreamEventType::SessionInit;
    Event.SessionId = TEXT("cli-session-456");

    Panel->OnStreamEvent(Event);

    TestNotEqual(TEXT("Session id should update from CLI init event"), Panel->SessionId, OriginalSessionId);
    TestEqual(TEXT("Session id should match CLI session"), Panel->SessionId, FString(TEXT("cli-session-456")));
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
    Panel->CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
    Panel->CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
    Panel->CurrentStreamingEntry->Text = TEXT("");
    Panel->ChatEntries.Add(Panel->CurrentStreamingEntry);

    Panel->OnComplete(TEXT("Failed to start Claude process"), false);

    TestEqual(TEXT("Failure should leave only one visible error entry"), Panel->ChatEntries.Num(), 1);
    if (Panel->ChatEntries.Num() == 1)
    {
        TestEqual(TEXT("Remaining entry should be assistant error text"), Panel->ChatEntries[0]->Text, FString(TEXT("Error: Failed to start Claude process")));
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
    Panel->CurrentStreamingEntry = MakeShared<FCortexChatEntry>();
    Panel->CurrentStreamingEntry->Type = ECortexChatEntryType::AssistantMessage;
    Panel->CurrentStreamingEntry->Text = TEXT("");
    Panel->ChatEntries.Add(Panel->CurrentStreamingEntry);

    Panel->OnComplete(TEXT("Before\n```cpp\nint Value = 42;\n```\nAfter"), true);

    bool bFoundCodeBlock = false;
    bool bFoundLeadingText = false;
    bool bFoundTrailingText = false;
    for (const TSharedPtr<FCortexChatEntry>& Entry : Panel->ChatEntries)
    {
        if (Entry->Type == ECortexChatEntryType::CodeBlock && Entry->Text.Contains(TEXT("int Value = 42;")))
        {
            bFoundCodeBlock = true;
        }
        if (Entry->Type == ECortexChatEntryType::AssistantMessage && Entry->Text.Contains(TEXT("Before")))
        {
            bFoundLeadingText = true;
        }
        if (Entry->Type == ECortexChatEntryType::AssistantMessage && Entry->Text.Contains(TEXT("After")))
        {
            bFoundTrailingText = true;
        }
    }

    TestTrue(TEXT("Success should materialize a code block entry"), bFoundCodeBlock);
    TestTrue(TEXT("Success should preserve text before code block"), bFoundLeadingText);
    TestTrue(TEXT("Success should preserve text after code block"), bFoundTrailingText);
    return true;
}
