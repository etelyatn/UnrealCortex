#include "Misc/AutomationTest.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Widgets/SCortexChatMessage.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatMessageMarkdownWidgetTest,
    "Cortex.Frontend.ChatMessage.MarkdownWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatMessageMarkdownWidgetTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    TSharedRef<SCortexChatMessage> Message = SNew(SCortexChatMessage);
    const FString MarkdownText = TEXT("Hello **bold**\n\n```cpp\nvoid Foo() {}\n```\n\nEnd paragraph");
    Message->SetText(MarkdownText);

    TestTrue(TEXT("Message should be visible"), Message->GetVisibility() != EVisibility::Hidden);

    // Size checks require a valid render target — skip under NullRHI
    const FVector2D DesiredSize = Message->GetDesiredSize();
    if (DesiredSize.Y > 0.0f)
    {
        TSharedRef<SCortexChatMessage> EmptyMessage = SNew(SCortexChatMessage);
        EmptyMessage->SetText(TEXT(""));
        TestTrue(TEXT("Empty message desired height should differ from markdown"),
            DesiredSize.Y > EmptyMessage->GetDesiredSize().Y);
    }
    else
    {
        AddInfo(TEXT("Desired size is 0 (NullRHI / no layout pass) — size assertions skipped"));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatMessagePrefixTest,
    "Cortex.Frontend.ChatMessage.PrefixStyle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatMessagePrefixTest::RunTest(const FString& Parameters)
{
    TSharedRef<SCortexChatMessage> UserMsg = SNew(SCortexChatMessage)
        .Message(TEXT("Hello"))
        .IsUser(true);

    // User message should have ">" prefix (not "You")
    TestEqual(TEXT("User prefix"), UserMsg->GetPrefixChar(), TEXT(">"));

    TSharedRef<SCortexChatMessage> AssistantMsg = SNew(SCortexChatMessage)
        .Message(TEXT("Hi"))
        .IsUser(false);

    // Assistant message should have "●" dot prefix (not "Claude")
    TestEqual(TEXT("Assistant prefix"), AssistantMsg->GetPrefixChar(), FString(TEXT("\u25CF")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCortexChatMessageWrapWidthDefaultTest,
    "Cortex.Frontend.ChatMessage.WrapWidthDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCortexChatMessageWrapWidthDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Ensure style set is initialized — module startup handles this in production,
    // but NullRHI test runner may not fully initialize editor modules.
    FCortexRichTextStyle::Initialize();

    // This test guards the WrapWidth default value initialization.
    // It does NOT verify that SRichTextBlock uses WrapTextAt — that is enforced
    // by code review of BuildContentForText. A unit test cannot inspect Slate
    // widget attributes without a rendered layout pass (NullRHI has no geometry).
    TSharedRef<SCortexChatMessage> Widget = SNew(SCortexChatMessage)
        .Message(TEXT("Test message"))
        .IsUser(false);

    // Default WrapWidth must be positive and large enough for SListView to calculate
    // a meaningful row height before the first tick runs.
    TestTrue(TEXT("Default WrapWidth should be positive"), Widget->GetWrapWidth() > 0.0f);
    TestTrue(TEXT("Default WrapWidth should be at least 100px"), Widget->GetWrapWidth() >= 100.0f);
    return true;
}
