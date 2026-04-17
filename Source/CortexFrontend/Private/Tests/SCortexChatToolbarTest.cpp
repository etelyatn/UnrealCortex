#include "Misc/AutomationTest.h"
#include "Widgets/SCortexChatToolbar.h"
#include "Session/CortexSessionTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
void CollectToolbarWidgets(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SWidget>>& OutWidgets)
{
    OutWidgets.Add(Widget);

    FChildren* Children = Widget->GetChildren();
    if (Children == nullptr)
    {
        return;
    }

    for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
    {
        CollectToolbarWidgets(Children->GetChildAt(ChildIndex), OutWidgets);
    }
}

bool ToolbarWidgetTreeContainsText(const TSharedRef<SWidget>& RootWidget, const FString& ExpectedText)
{
    TArray<TSharedRef<SWidget>> Widgets;
    CollectToolbarWidgets(RootWidget, Widgets);

    for (const TSharedRef<SWidget>& Widget : Widgets)
    {
        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            const TSharedRef<STextBlock> TextBlock = StaticCastSharedRef<STextBlock>(Widget);
            if (TextBlock->GetText().ToString() == ExpectedText)
            {
                return true;
            }
        }
    }

    return false;
}
}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatToolbarProviderLabelTest,
    "Cortex.Frontend.ChatToolbar.ProviderModelLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatToolbarSessionContextLimitTest,
    "Cortex.Frontend.ChatToolbar.SessionContextLimit",
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

bool FCortexChatToolbarProviderLabelTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(FCortexSessionConfig{
        TEXT("toolbar-provider"),
        TEXT(""),
        TEXT(""),
        TEXT(""),
        FName(TEXT("codex")),
        FCortexResolvedSessionOptions{
            FName(TEXT("codex")),
            TEXT("Codex"),
            TEXT("gpt-5.3-codex-spark"),
            ECortexEffortLevel::Maximum,
            128000},
        FCortexResolvedLaunchOptions{},
        true,
        TEXT(""),
        ECortexEffortLevel::Default,
        false,
        false});

    TSharedRef<SCortexChatToolbar> Toolbar = SNew(SCortexChatToolbar)
        .Session(Session);

    FCortexSessionStateChange Change;
    Change.PreviousState = ECortexSessionState::Spawning;
    Change.NewState = ECortexSessionState::Idle;
    Session->OnStateChanged.Broadcast(Change);

    TestTrue(TEXT("Toolbar label should include provider, model, and non-default effort"),
        ToolbarWidgetTreeContainsText(Toolbar, TEXT("Codex · gpt-5.3-codex-spark [max]")));

    return true;
}

bool FCortexChatToolbarSessionContextLimitTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(FCortexSessionConfig{
        TEXT("toolbar-context"),
        TEXT(""),
        TEXT(""),
        TEXT(""),
        FName(TEXT("codex")),
        FCortexResolvedSessionOptions{
            FName(TEXT("codex")),
            TEXT("Codex"),
            TEXT("gpt-5.3-codex-spark"),
            ECortexEffortLevel::Medium,
            128000},
        FCortexResolvedLaunchOptions{},
        true,
        TEXT(""),
        ECortexEffortLevel::Default,
        false,
        false});

    TSharedRef<SCortexChatToolbar> Toolbar = SNew(SCortexChatToolbar)
        .Session(Session);

    FCortexStreamEvent Event;
    Event.Type = ECortexStreamEventType::Result;
    Event.InputTokens = 64000;
    Session->HandleWorkerEvent(Event);

    TestTrue(TEXT("Toolbar context label should use the session's pinned context limit"),
        ToolbarWidgetTreeContainsText(Toolbar, TEXT("64k / 128k")));

    return true;
}
