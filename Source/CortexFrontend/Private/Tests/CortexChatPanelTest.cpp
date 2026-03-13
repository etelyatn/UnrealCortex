#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SCortexChatPanel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelConstructTest, "Cortex.Frontend.ChatPanel.Construct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
