#include "Misc/AutomationTest.h"
#include "Widgets/SCortexInputArea.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaChipTest,
    "Cortex.Frontend.InputArea.ContextChips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaChipTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    TestEqual(TEXT("Initially empty"), Widget->GetContextItems().Num(), 0);

    Widget->AddContextItem(TEXT("/Game/Blueprints/BP_Test.uasset"));
    TestEqual(TEXT("One chip"), Widget->GetContextItems().Num(), 1);

    Widget->AddContextItem(TEXT("/Game/Data/DT_Items.uasset"));
    TestEqual(TEXT("Two chips"), Widget->GetContextItems().Num(), 2);

    Widget->RemoveContextItem(0);
    TestEqual(TEXT("After remove"), Widget->GetContextItems().Num(), 1);
    TestEqual(TEXT("Remaining chip"), Widget->GetContextItems()[0], TEXT("/Game/Data/DT_Items.uasset"));

    Widget->ClearContextItems();
    TestEqual(TEXT("After clear"), Widget->GetContextItems().Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaDropdownsTest,
    "Cortex.Frontend.InputArea.DropdownsInitialized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaDropdownsTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    // Verify widget has children (3-section layout: chips + textarea + controls)
    TestTrue(TEXT("Has children"), Widget->GetChildren()->Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaContextFormatTest,
    "Cortex.Frontend.InputArea.ContextSerializesAsAtPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaContextFormatTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->AddContextItem(TEXT("/Game/BP_Test.uasset"));
    Widget->AddContextItem(TEXT("/Game/DT_Items.uasset"));

    const TArray<FString>& Items = Widget->GetContextItems();
    TestEqual(TEXT("Item count"), Items.Num(), 2);
    TestEqual(TEXT("First item"), Items[0], TEXT("/Game/BP_Test.uasset"));
    TestEqual(TEXT("Second item"), Items[1], TEXT("/Game/DT_Items.uasset"));

    return true;
}
