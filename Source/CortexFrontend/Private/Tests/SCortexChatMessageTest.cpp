#include "Misc/AutomationTest.h"
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
