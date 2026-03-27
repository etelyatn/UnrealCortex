#include "Misc/AutomationTest.h"
#include "Widgets/SCortexInputArea.h"
#include "AutoComplete/CortexAutoCompleteTypes.h"
#include "CortexFrontendSettings.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaEffortSettingTest,
    "Cortex.Frontend.InputArea.EffortSettingPersists",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaEffortSettingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const ECortexEffortLevel OriginalEffort = FCortexFrontendSettings::Get().GetEffortLevel();
    FCortexFrontendSettings::Get().SetEffortLevel(ECortexEffortLevel::High);
    TestEqual(TEXT("Effort set to High"),
        static_cast<int32>(FCortexFrontendSettings::Get().GetEffortLevel()),
        static_cast<int32>(ECortexEffortLevel::High));
    FCortexFrontendSettings::Get().SetEffortLevel(OriginalEffort);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaChipTest,
    "Cortex.Frontend.InputArea.ContextChips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaChipTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    TestEqual(TEXT("Initially empty"), Widget->GetContextChips().Num(), 0);

    FCortexContextChip Chip1;
    Chip1.Kind = ECortexContextChipKind::Asset;
    Chip1.Label = TEXT("/Game/Blueprints/BP_Test");
    Chip1.RouterCommand = TEXT("blueprint.get_blueprint");
    Widget->AddContextChip(Chip1);
    TestEqual(TEXT("One chip"), Widget->GetContextChips().Num(), 1);

    FCortexContextChip Chip2;
    Chip2.Kind = ECortexContextChipKind::Asset;
    Chip2.Label = TEXT("/Game/Data/DT_Items");
    Chip2.RouterCommand = TEXT("data.get_datatable");
    Widget->AddContextChip(Chip2);
    TestEqual(TEXT("Two chips"), Widget->GetContextChips().Num(), 2);

    Widget->RemoveContextChip(0);
    TestEqual(TEXT("After remove"), Widget->GetContextChips().Num(), 1);
    TestEqual(TEXT("Remaining chip label"), Widget->GetContextChips()[0].Label, TEXT("/Game/Data/DT_Items"));

    Widget->ClearContextChips();
    TestEqual(TEXT("After clear"), Widget->GetContextChips().Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaDropdownsTest,
    "Cortex.Frontend.InputArea.DropdownsInitialized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaDropdownsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);
    TestTrue(TEXT("Has children"), Widget->GetChildren()->Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaContextStorageTest,
    "Cortex.Frontend.InputArea.ContextChipsStoredCorrectly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaContextStorageTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    FCortexContextChip ChipA;
    ChipA.Kind = ECortexContextChipKind::Provider;
    ChipA.Label = TEXT("thisAsset");
    Widget->AddContextChip(ChipA);

    FCortexContextChip ChipB;
    ChipB.Kind = ECortexContextChipKind::Asset;
    ChipB.Label = TEXT("/Game/DT_Items");
    ChipB.RouterCommand = TEXT("data.get_datatable");
    Widget->AddContextChip(ChipB);

    const TArray<FCortexContextChip>& Chips = Widget->GetContextChips();
    TestEqual(TEXT("Chip count"), Chips.Num(), 2);
    TestEqual(TEXT("First label"), Chips[0].Label, TEXT("thisAsset"));
    TestTrue(TEXT("First kind Provider"), Chips[0].Kind == ECortexContextChipKind::Provider);
    TestEqual(TEXT("Second router"), Chips[1].RouterCommand, TEXT("data.get_datatable"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaPopupClosedInitiallyTest,
    "Cortex.Frontend.InputArea.AutoComplete.ClosedInitially",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaPopupClosedInitiallyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);
    TestFalse(TEXT("Popup closed on construction"), Widget->IsAutoCompleteOpen());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaAtTriggerTest,
    "Cortex.Frontend.InputArea.AutoComplete.AtTriggerOpensPopup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaAtTriggerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    // Simulate typing "@" (single char insertion from empty)
    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    TestTrue(TEXT("Popup opens on @"), Widget->IsAutoCompleteOpen());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaSlashTriggerTest,
    "Cortex.Frontend.InputArea.AutoComplete.SlashAtPos0OpensPopup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaSlashTriggerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    {
        // / at position 0 opens popup
        TSharedRef<SCortexInputArea> W = SNew(SCortexInputArea);
        W->HandleTextChanged(FText::FromString(TEXT("/")));
        TestTrue(TEXT("Popup opens on / at pos 0"), W->IsAutoCompleteOpen());
    }
    {
        // / mid-sentence: must be preceded by text — simulate "hello /" (len 7 from len 6)
        TSharedRef<SCortexInputArea> W = SNew(SCortexInputArea);
        W->HandleTextChanged(FText::FromString(TEXT("hello "))); // set previous text
        W->HandleTextChanged(FText::FromString(TEXT("hello /"))); // type /
        TestFalse(TEXT("Popup stays closed on / mid-sentence"), W->IsAutoCompleteOpen());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaPasteGuardTest,
    "Cortex.Frontend.InputArea.AutoComplete.PasteDoesNotTrigger",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaPasteGuardTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    // Multi-char insertion (paste) — should NOT open popup
    Widget->HandleTextChanged(FText::FromString(TEXT("@pasted text")));
    TestFalse(TEXT("Popup stays closed on paste"), Widget->IsAutoCompleteOpen());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaBackspaceClosesTest,
    "Cortex.Frontend.InputArea.AutoComplete.BackspaceCloses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaBackspaceClosesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    TestTrue(TEXT("Open after @"), Widget->IsAutoCompleteOpen());

    // Simulate backspace: text goes from "@" to ""
    Widget->HandleTextChanged(FText::FromString(TEXT("")));
    TestFalse(TEXT("Closed after backspace"), Widget->IsAutoCompleteOpen());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaProvidersTest,
    "Cortex.Frontend.InputArea.AutoComplete.ProvidersAlwaysPresent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaProvidersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    TestTrue(TEXT("Popup open"), Widget->IsAutoCompleteOpen());

    const auto& Items = Widget->GetFilteredItems();
    TestTrue(TEXT("At least 3 items (providers)"), Items.Num() >= 3);

    // Verify provider names
    bool bFoundThisAsset = false, bFoundSelection = false, bFoundProblems = false;
    for (const TSharedPtr<FCortexAutoCompleteItem>& Item : Items)
    {
        if (Item->Name == TEXT("thisAsset")) bFoundThisAsset = true;
        if (Item->Name == TEXT("selection")) bFoundSelection = true;
        if (Item->Name == TEXT("problems")) bFoundProblems = true;
    }
    TestTrue(TEXT("@thisAsset present"), bFoundThisAsset);
    TestTrue(TEXT("@selection present"), bFoundSelection);
    TestTrue(TEXT("@problems present"), bFoundProblems);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaFuzzyFilterTest,
    "Cortex.Frontend.InputArea.AutoComplete.FuzzyFilterNarrows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaFuzzyFilterTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    // Open popup and type a query that matches no providers and no assets (empty cache)
    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    Widget->HandleTextChanged(FText::FromString(TEXT("@thisA")));

    const auto& Items = Widget->GetFilteredItems();
    bool bFoundThisAsset = false;
    for (const TSharedPtr<FCortexAutoCompleteItem>& Item : Items)
    {
        if (Item->Name.Contains(TEXT("thisAsset"))) bFoundThisAsset = true;
    }
    TestTrue(TEXT("thisAsset matches @thisA query"), bFoundThisAsset);

    return true;
}
