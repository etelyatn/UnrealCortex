#include "Misc/AutomationTest.h"
#include "Widgets/SCortexChatToolbar.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatToolbarNewChatCallbackTest,
    "Cortex.Frontend.ChatToolbar.NewChatCallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatToolbarNewChatCallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    bool bNewChatCalled = false;
    TSharedRef<SCortexChatToolbar> Toolbar = SNew(SCortexChatToolbar)
        .OnNewChat(FOnCortexNewChat::CreateLambda([&bNewChatCalled]() { bNewChatCalled = true; }));

    // Verify construction succeeded
    TestTrue(TEXT("Toolbar should be visible"), Toolbar->GetVisibility() != EVisibility::Hidden);

    // Verify the OnNewChat callback fires when invoked
    TestFalse(TEXT("bNewChatCalled should be false before trigger"), bNewChatCalled);
    Toolbar->OnNewChat.ExecuteIfBound();
    TestTrue(TEXT("bNewChatCalled should be true after trigger"), bNewChatCalled);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatToolbarContextIndicatorTest,
    "Cortex.Frontend.ChatToolbar.ContextIndicatorConstructs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatToolbarContextIndicatorTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    // Construct toolbar without a session — context indicator should default gracefully
    TSharedRef<SCortexChatToolbar> Toolbar = SNew(SCortexChatToolbar);
    TestTrue(TEXT("Toolbar with no session should construct without crash"),
        Toolbar->GetVisibility() != EVisibility::Hidden);

    // SetSessionId should not crash
    Toolbar->SetSessionId(TEXT("test-session-abc"));

    return true;
}
