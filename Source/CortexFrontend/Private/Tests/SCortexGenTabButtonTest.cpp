#include "Misc/AutomationTest.h"
#include "Widgets/SCortexGenTabButton.h"
#include "Widgets/CortexGenSessionTypes.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenTabButtonConstructTest,
    "Cortex.Frontend.GenStudio.TabButton.Construct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenTabButtonConstructTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping widget test"));
        return true;
    }

    bool bCloseClicked = false;
    bool bTabClicked = false;

    TSharedRef<SCortexGenTabButton> Button = SNew(SCortexGenTabButton)
        .DisplayName(FText::FromString(TEXT("Image #1")))
        .IsActive(true)
        .OnClicked_Lambda([&bTabClicked]() { bTabClicked = true; return FReply::Handled(); })
        .OnCloseClicked_Lambda([&bCloseClicked]() { bCloseClicked = true; });

    TestTrue(TEXT("Widget should be valid"), Button->GetVisibility() == EVisibility::Visible);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenTabButtonStatusUpdateTest,
    "Cortex.Frontend.GenStudio.TabButton.StatusUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenTabButtonStatusUpdateTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping widget test"));
        return true;
    }

    TSharedRef<SCortexGenTabButton> Button = SNew(SCortexGenTabButton)
        .DisplayName(FText::FromString(TEXT("Image #1")))
        .IsActive(false);

    // Should not crash on status updates
    Button->SetStatus(ECortexGenSessionStatus::Idle);
    Button->SetStatus(ECortexGenSessionStatus::Generating);
    Button->SetStatus(ECortexGenSessionStatus::Complete);
    Button->SetStatus(ECortexGenSessionStatus::Error);
    Button->SetStatus(ECortexGenSessionStatus::PartialComplete);

    return true;
}
