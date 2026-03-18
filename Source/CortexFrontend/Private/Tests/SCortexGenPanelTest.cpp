#include "Misc/AutomationTest.h"
#include "Widgets/SCortexGenPanel.h"
#include "Widgets/CortexGenSessionTypes.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenPanelConstructTest,
    "Cortex.Frontend.GenStudio.Panel.Construct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenPanelConstructTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping"));
        return true;
    }

    TSharedRef<SCortexGenPanel> Panel = SNew(SCortexGenPanel);
    TestTrue(TEXT("Panel should be visible"),
        Panel->GetVisibility() == EVisibility::Visible);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenPanelAddSessionTest,
    "Cortex.Frontend.GenStudio.Panel.AddSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenPanelAddSessionTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping"));
        return true;
    }

    TSharedRef<SCortexGenPanel> Panel = SNew(SCortexGenPanel);

    TestEqual(TEXT("No sessions initially"), Panel->GetSessionCount(), 0);

    Panel->AddSession(ECortexGenSessionType::Image);
    TestEqual(TEXT("One session after add"), Panel->GetSessionCount(), 1);

    Panel->AddSession(ECortexGenSessionType::Mesh);
    TestEqual(TEXT("Two sessions after add"), Panel->GetSessionCount(), 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenPanelRemoveSessionTest,
    "Cortex.Frontend.GenStudio.Panel.RemoveSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenPanelRemoveSessionTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping"));
        return true;
    }

    TSharedRef<SCortexGenPanel> Panel = SNew(SCortexGenPanel);
    Panel->AddSession(ECortexGenSessionType::Image);
    Panel->AddSession(ECortexGenSessionType::Mesh);

    Panel->RemoveSession(0);
    TestEqual(TEXT("One session after remove"), Panel->GetSessionCount(), 1);

    return true;
}
