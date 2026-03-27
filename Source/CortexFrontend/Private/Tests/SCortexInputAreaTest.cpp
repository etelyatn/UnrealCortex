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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaCoreCommandsTest,
    "Cortex.Frontend.InputArea.AutoComplete.CoreCommandsPresent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaCoreCommandsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("/")));
    TestTrue(TEXT("Popup open"), Widget->IsAutoCompleteOpen());

    const auto& Items = Widget->GetFilteredItems();
    bool bFoundHelp = false, bFoundClear = false, bFoundCompact = false;
    for (const TSharedPtr<FCortexAutoCompleteItem>& Item : Items)
    {
        if (Item->Name == TEXT("help")) bFoundHelp = true;
        if (Item->Name == TEXT("clear")) bFoundClear = true;
        if (Item->Name == TEXT("compact")) bFoundCompact = true;
    }
    TestTrue(TEXT("/help present"), bFoundHelp);
    TestTrue(TEXT("/clear present"), bFoundClear);
    TestTrue(TEXT("/compact present"), bFoundCompact);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaFrontmatterParseTest,
    "Cortex.Frontend.InputArea.AutoComplete.FrontmatterParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaFrontmatterParseTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Content = TEXT("---\nname: cortex-blueprint\ndescription: Create and edit Blueprints\n---\n\n# Body text");
    TestEqual(TEXT("Parses name"),
        SCortexInputArea::ParseFrontmatterField(Content, TEXT("name")),
        TEXT("cortex-blueprint"));
    TestEqual(TEXT("Parses description"),
        SCortexInputArea::ParseFrontmatterField(Content, TEXT("description")),
        TEXT("Create and edit Blueprints"));
    TestEqual(TEXT("Returns empty for missing field"),
        SCortexInputArea::ParseFrontmatterField(Content, TEXT("nonexistent")),
        TEXT(""));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaNavigationTest,
    "Cortex.Frontend.InputArea.AutoComplete.NavigationChangesIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaNavigationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("/")));
    TestTrue(TEXT("Open"), Widget->IsAutoCompleteOpen());
    TestEqual(TEXT("Index starts at 0"), Widget->GetAutoCompleteSelectedIndex(), 0);

    // Simulate Down arrow
    FKeyEvent DownKey(EKeys::Down, FModifierKeysState(), 0, false, 0, 0);
    Widget->OnKeyDown(FGeometry(), DownKey);
    TestEqual(TEXT("Index moves to 1 on Down"), Widget->GetAutoCompleteSelectedIndex(), 1);

    // Simulate Up arrow back
    FKeyEvent UpKey(EKeys::Up, FModifierKeysState(), 0, false, 0, 0);
    Widget->OnKeyDown(FGeometry(), UpKey);
    TestEqual(TEXT("Index returns to 0 on Up"), Widget->GetAutoCompleteSelectedIndex(), 0);

    // Up at 0 does not wrap below 0
    Widget->OnKeyDown(FGeometry(), UpKey);
    TestEqual(TEXT("Index clamps at 0"), Widget->GetAutoCompleteSelectedIndex(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaEscDismissTest,
    "Cortex.Frontend.InputArea.AutoComplete.EscCloses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaEscDismissTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("/")));
    TestTrue(TEXT("Open"), Widget->IsAutoCompleteOpen());

    FKeyEvent EscKey(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
    Widget->OnKeyDown(FGeometry(), EscKey);
    TestFalse(TEXT("Closed after Esc"), Widget->IsAutoCompleteOpen());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaCommitAtTest,
    "Cortex.Frontend.InputArea.AutoComplete.CommitAtInline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaCommitAtTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    // Open @ popup — providers should be in FilteredItems at index 0
    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    TestTrue(TEXT("Open"), Widget->IsAutoCompleteOpen());
    TestTrue(TEXT("Has items"), Widget->GetFilteredItems().Num() > 0);
    TestEqual(TEXT("First item is thisAsset"), Widget->GetFilteredItems()[0]->Name, TEXT("thisAsset"));

    // Commit with Enter
    FKeyEvent EnterKey(EKeys::Enter, FModifierKeysState(), 0, false, 0, 0);
    Widget->OnKeyDown(FGeometry(), EnterKey);

    TestFalse(TEXT("Popup closed after commit"), Widget->IsAutoCompleteOpen());
    // No chip — @mention is inline in the text box instead
    TestEqual(TEXT("No chip created"), Widget->GetContextChips().Num(), 0);
    TestTrue(TEXT("@mention inline in text"), Widget->GetInputText().ToString().Contains(TEXT("@thisAsset")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaCommitSlashTest,
    "Cortex.Frontend.InputArea.AutoComplete.CommitSlashReplacesText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaCommitSlashTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("/")));
    TestTrue(TEXT("Open"), Widget->IsAutoCompleteOpen());

    // Commit first item (should be /help)
    FKeyEvent EnterKey(EKeys::Enter, FModifierKeysState(), 0, false, 0, 0);
    Widget->OnKeyDown(FGeometry(), EnterKey);

    TestFalse(TEXT("Popup closed"), Widget->IsAutoCompleteOpen());
    TestEqual(TEXT("No chips added for slash"), Widget->GetContextChips().Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaEmptyProviderDropTest,
    "Cortex.Frontend.InputArea.Resolution.EmptyProviderSilentDrop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaEmptyProviderDropTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FString CapturedPrompt;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&CapturedPrompt](const FString& Prompt) { CapturedPrompt = Prompt; });

    // Add a provider chip (thisAsset — will return empty since no editor context in test)
    FCortexContextChip ProviderChip;
    ProviderChip.Kind = ECortexContextChipKind::Provider;
    ProviderChip.Label = TEXT("thisAsset");
    Widget->AddContextChip(ProviderChip);

    // Directly call ResolveAndSend to bypass async (test helper)
    Widget->TestResolveAndSend(TEXT("Hello"));

    // Provider returned empty → section silently dropped
    TestFalse(TEXT("No ## Context header for empty provider"),
        CapturedPrompt.Contains(TEXT("## Context:")));
    TestTrue(TEXT("User message preserved"), CapturedPrompt.Contains(TEXT("Hello")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaAssetFallbackTest,
    "Cortex.Frontend.InputArea.Resolution.AssetRouterErrorFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaAssetFallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FString CapturedPrompt;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&CapturedPrompt](const FString& Prompt) { CapturedPrompt = Prompt; });

    // Add asset chip with empty RouterCommand (unmapped type)
    FCortexContextChip AssetChip;
    AssetChip.Kind = ECortexContextChipKind::Asset;
    AssetChip.Label = TEXT("/Game/SomeUnknownAsset");
    AssetChip.RouterCommand = TEXT(""); // Unmapped — should fall back to @path
    Widget->AddContextChip(AssetChip);

    Widget->TestResolveAndSend(TEXT("Hello"));

    TestTrue(TEXT("Falls back to @path for unmapped asset"),
        CapturedPrompt.Contains(TEXT("@/Game/SomeUnknownAsset")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaSelectionDescriptionTest,
    "Cortex.Frontend.InputArea.Selection.DescriptionMentionsNodesAndActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaSelectionDescriptionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    Widget->HandleTextChanged(FText::FromString(TEXT("@")));
    Widget->HandleTextChanged(FText::FromString(TEXT("@sel")));

    const auto& Items = Widget->GetFilteredItems();
    FString SelectionDesc;
    for (const TSharedPtr<FCortexAutoCompleteItem>& Item : Items)
    {
        if (Item->Name == TEXT("selection"))
        {
            SelectionDesc = Item->Description;
            break;
        }
    }

    TestFalse(TEXT("Selection description is not empty"), SelectionDesc.IsEmpty());
    TestTrue(TEXT("Description mentions nodes"),
        SelectionDesc.Contains(TEXT("node"), ESearchCase::IgnoreCase) ||
        SelectionDesc.Contains(TEXT("graph"), ESearchCase::IgnoreCase));

    return true;
}
