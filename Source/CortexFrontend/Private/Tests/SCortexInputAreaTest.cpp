#include "Misc/AutomationTest.h"
#include "Widgets/SCortexInputArea.h"
#include "AutoComplete/CortexAutoCompleteTypes.h"
#include "CortexFrontendSettings.h"
#include "CortexFrontendProviderSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Text/STextBlock.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Selection.h"
#include "Context/CortexEditorContextGatherer.h"

namespace
{
void CollectInputAreaWidgets(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SWidget>>& OutWidgets)
{
    OutWidgets.Add(Widget);

    FChildren* Children = Widget->GetChildren();
    if (Children == nullptr)
    {
        return;
    }

    for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
    {
        CollectInputAreaWidgets(Children->GetChildAt(ChildIndex), OutWidgets);
    }
}

bool InputAreaWidgetTreeContainsText(const TSharedRef<SWidget>& RootWidget, const FString& ExpectedText)
{
    TArray<TSharedRef<SWidget>> Widgets;
    CollectInputAreaWidgets(RootWidget, Widgets);

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

// Task 16 helpers. Names are module-unique because unity builds merge anonymous
// namespaces across the CortexFrontend test translation units.
int32 Task16CountOccurrences(const FString& Haystack, const FString& Needle)
{
    if (Needle.IsEmpty() || Haystack.IsEmpty())
    {
        return 0;
    }

    int32 Count = 0;
    int32 SearchIndex = 0;
    while (SearchIndex != INDEX_NONE)
    {
        SearchIndex = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
        if (SearchIndex != INDEX_NONE)
        {
            ++Count;
            SearchIndex += Needle.Len();
        }
    }
    return Count;
}

// Owns a transient actor spawned into the editor world for the duration of a test.
// Spawned actors get an explicit display label (from the requested name) because
// SpawnParams.Name only sets the object name while GetActorLabel() otherwise falls back
// to the class name ("Actor") for every instance, making two actors indistinguishable.
// Teardown restores the previous selection and the previous world dirty flag, and
// deletes every owned actor even when the test fails early.
struct FTask16ActorFixture
{
    FTask16ActorFixture() = default;
    FTask16ActorFixture(const FTask16ActorFixture&) = delete;
    FTask16ActorFixture& operator=(const FTask16ActorFixture&) = delete;

    UWorld* World = nullptr;
    TArray<AActor*> OwnedActors;
    TArray<AActor*> PreviousSelection;
    bool bPreviousWorldDirty = false;
    bool bBegun = false;

    bool Begin()
    {
        if (GEditor == nullptr || !FSlateApplication::IsInitialized())
        {
            return false;
        }

        World = GEditor->GetEditorWorldContext().World();
        if (World == nullptr)
        {
            return false;
        }

        bPreviousWorldDirty = World->GetOutermost()->IsDirty();

        if (USelection* Selection = GEditor->GetSelectedActors())
        {
            for (FSelectionIterator It(*Selection); It; ++It)
            {
                if (AActor* Actor = Cast<AActor>(*It))
                {
                    PreviousSelection.Add(Actor);
                }
            }
        }

        bBegun = true;
        return true;
    }

    AActor* Spawn(const TCHAR* Name)
    {
        if (World == nullptr)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.Name = FName(Name);
        SpawnParams.ObjectFlags |= RF_Transient;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
        if (Actor != nullptr)
        {
#if WITH_EDITOR
            // SpawnParams.Name sets only the object name; the display label would otherwise default
            // to the class name ("Actor") for every spawned instance. Set it explicitly (bMarkDirty
            // = false so the fixture does not dirty the level; the dirty flag is also restored).
            Actor->SetActorLabel(FString(Name), false);
#endif
            OwnedActors.Add(Actor);
        }
        return Actor;
    }

    void Select(AActor* Actor)
    {
        if (GEditor == nullptr)
        {
            return;
        }

        GEditor->SelectNone(false, true, false);
        if (Actor != nullptr)
        {
            GEditor->SelectActor(Actor, true, false, true);
        }
        GEditor->NoteSelectionChange();
    }

    ~FTask16ActorFixture()
    {
        if (!bBegun || GEditor == nullptr)
        {
            return;
        }

        GEditor->SelectNone(false, true, false);
        for (AActor* Actor : OwnedActors)
        {
            if (IsValid(Actor))
            {
                Actor->Destroy();
            }
        }
        for (AActor* Actor : PreviousSelection)
        {
            if (IsValid(Actor))
            {
                GEditor->SelectActor(Actor, true, false, true);
            }
        }
        GEditor->NoteSelectionChange();

        if (World != nullptr && World->GetOutermost() != nullptr)
        {
            World->GetOutermost()->SetDirtyFlag(bPreviousWorldDirty);
        }
    }
};
}

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
    TestTrue(TEXT("At least 4 items (providers)"), Items.Num() >= 4);

    // Verify provider names
    bool bFoundThisAsset = false, bFoundSelection = false, bFoundProblems = false, bFoundEditorContext = false;
    for (const TSharedPtr<FCortexAutoCompleteItem>& Item : Items)
    {
        if (Item->Name == TEXT("thisAsset")) bFoundThisAsset = true;
        if (Item->Name == TEXT("selection")) bFoundSelection = true;
        if (Item->Name == TEXT("problems")) bFoundProblems = true;
        if (Item->Name == TEXT("editorContext")) bFoundEditorContext = true;
    }
    TestTrue(TEXT("@thisAsset present"), bFoundThisAsset);
    TestTrue(TEXT("@selection present"), bFoundSelection);
    TestTrue(TEXT("@problems present"), bFoundProblems);
    TestTrue(TEXT("@editorContext present"), bFoundEditorContext);

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaAutoContextSuppressedTest,
    "Cortex.Frontend.InputArea.Resolution.AutoContextSuppressedWhenDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaAutoContextSuppressedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginal = Settings.GetAutoContext();

    // Disable auto-context
    Settings.SetAutoContext(false);

    FString CapturedPrompt;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&CapturedPrompt](const FString& Prompt) { CapturedPrompt = Prompt; });

    Widget->TestResolveAndSend(TEXT("Hello"));

    // Auto-context disabled → header must not appear
    TestFalse(TEXT("No auto-context header when disabled"),
        CapturedPrompt.Contains(TEXT("## Editor Context (auto)")));
    TestTrue(TEXT("User message preserved"), CapturedPrompt.Contains(TEXT("Hello")));

    // Restore
    Settings.SetAutoContext(bOriginal);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaProviderResolvedModelLabelTest,
    "Cortex.Frontend.InputArea.ProviderResolvedModelLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaProviderHelpCopyTest,
    "Cortex.Frontend.InputArea.ProviderHelpCopy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaEffortOptionsUseActiveProviderTest,
    "Cortex.Frontend.InputArea.EffortOptionsUseActiveProvider",
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

bool FCortexInputAreaProviderResolvedModelLabelTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const FString OriginalModel = Settings.GetSelectedModel();

    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetSelectedModel(OriginalModel);
        Settings.ClearPendingChanges();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetSelectedModel(TEXT("claude-opus-4-6"));

    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);

    TestTrue(TEXT("Input area should display the active provider's resolved model"),
        InputAreaWidgetTreeContainsText(Widget, TEXT("gpt-5.4")));
    TestFalse(TEXT("Input area should not display the stale provider-incompatible model"),
        InputAreaWidgetTreeContainsText(Widget, TEXT("claude-opus-4-6")));

    return true;
}

bool FCortexInputAreaProviderHelpCopyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea);
    Widget->HandleTextChanged(FText::FromString(TEXT("/")));

    const TArray<TSharedPtr<FCortexAutoCompleteItem>>& Items = Widget->GetFilteredItems();
    const TSharedPtr<FCortexAutoCompleteItem>* HelpItem = Items.FindByPredicate(
        [](const TSharedPtr<FCortexAutoCompleteItem>& Item)
        {
            return Item.IsValid() && Item->Name == TEXT("help");
        });

    TestNotNull(TEXT("/help item should be present"), HelpItem);
    if (HelpItem != nullptr)
    {
        TestEqual(TEXT("/help should use provider-neutral help copy"),
            (*HelpItem)->Description,
            FString(TEXT("Get help with the active AI provider")));
    }

    return true;
}

bool FCortexInputAreaEffortOptionsUseActiveProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
    const FString OriginalModel = Settings.GetSelectedModel();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetSelectedModel(OriginalModel);
        Settings.ClearPendingChanges();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetSelectedModel(TEXT("gpt-5.4"));

    const TArray<ECortexEffortLevel> Options = SCortexInputArea::GetEffortOptionsForActiveProvider();
    TestFalse(TEXT("Codex effort menu should not expose unsupported Default effort"), Options.Contains(ECortexEffortLevel::Default));
    TestTrue(TEXT("Codex effort menu should expose Maximum/xhigh effort"), Options.Contains(ECortexEffortLevel::Maximum));
    return true;
}

// Task 16 RED: an explicit editorContext provider chip must not be duplicated by the
// auto-context preamble on the same send.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaResolutionProviderEditorContextDedupeTest,
    "Cortex.Frontend.InputArea.Resolution.ProviderEditorContextNotDuplicatedByAutoContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaResolutionProviderEditorContextDedupeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FTask16ActorFixture Fixture;
    if (!Fixture.Begin()) { AddInfo(TEXT("No editor world available")); return true; }

    AActor* SelectedActor = Fixture.Spawn(TEXT("CortexTask16SelectedActor"));
    if (!TestNotNull(TEXT("Spawned task-owned selected actor"), SelectedActor))
    {
        return true;
    }
    Fixture.Select(SelectedActor);
    const FString SelectedIdentity = SelectedActor->GetActorLabel();
    TestFalse(TEXT("Selected identity is observable"), SelectedIdentity.IsEmpty());

    // Precondition: the gatherer must observe the task-owned selection exactly once, otherwise the
    // duplicate-identity assertion below would fail for a fixture reason instead of the real defect.
    TestEqual(TEXT("Gatherer observes the task-owned selection exactly once"),
        Task16CountOccurrences(FCortexEditorContextGatherer::GatherSelectedActors(), SelectedIdentity), 1);

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginalAutoContext = Settings.GetAutoContext();
    ON_SCOPE_EXIT { Settings.SetAutoContext(bOriginalAutoContext); Settings.ClearPendingChanges(); };
    Settings.SetAutoContext(true);

    FString Captured;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&Captured](const FString& Prompt) { Captured = Prompt; });

    FCortexContextChip EditorContextChip;
    EditorContextChip.Kind = ECortexContextChipKind::Provider;
    EditorContextChip.Label = TEXT("editorContext");
    Widget->AddContextChip(EditorContextChip);

    FCortexContextChip RawChip;
    RawChip.Kind = ECortexContextChipKind::RawText;
    RawChip.Label = TEXT("CortexTask16RawChip");
    Widget->AddContextChip(RawChip);

    Widget->TestResolveAndSend(TEXT("Inspect selection"));

    TestEqual(TEXT("Auto editor-context preamble is not duplicated by an explicit editorContext chip"),
        Task16CountOccurrences(Captured, TEXT("## Editor Context (auto)")), 1);
    TestEqual(TEXT("Selected actor identity is emitted exactly once"),
        Task16CountOccurrences(Captured, SelectedIdentity), 1);
    TestTrue(TEXT("User message preserved"), Captured.Contains(TEXT("Inspect selection")));
    TestEqual(TEXT("Unrelated chip preserved exactly once"),
        Task16CountOccurrences(Captured, TEXT("@CortexTask16RawChip")), 1);

    return true;
}

// Task 16: the next send must carry the current selection, never a cached snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaResolutionNextTurnUsesCurrentSelectionTest,
    "Cortex.Frontend.InputArea.Resolution.NextTurnUsesCurrentSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaResolutionNextTurnUsesCurrentSelectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FTask16ActorFixture Fixture;
    if (!Fixture.Begin()) { AddInfo(TEXT("No editor world available")); return true; }

    AActor* ActorA = Fixture.Spawn(TEXT("CortexTask16ActorA"));
    AActor* ActorB = Fixture.Spawn(TEXT("CortexTask16ActorB"));
    if (!TestNotNull(TEXT("Spawned task-owned actor A"), ActorA) ||
        !TestNotNull(TEXT("Spawned task-owned actor B"), ActorB))
    {
        return true;
    }
    const FString IdentityA = ActorA->GetActorLabel();
    const FString IdentityB = ActorB->GetActorLabel();
    TestNotEqual(TEXT("Task actors have distinct identities"), IdentityA, IdentityB);

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginalAutoContext = Settings.GetAutoContext();
    ON_SCOPE_EXIT { Settings.SetAutoContext(bOriginalAutoContext); Settings.ClearPendingChanges(); };
    Settings.SetAutoContext(true);

    FString Captured;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&Captured](const FString& Prompt) { Captured = Prompt; });

    auto SendWithEditorContext = [&Widget](const FString& Message)
    {
        FCortexContextChip Chip;
        Chip.Kind = ECortexContextChipKind::Provider;
        Chip.Label = TEXT("editorContext");
        Widget->AddContextChip(Chip);
        Widget->TestResolveAndSend(Message);
    };

    Fixture.Select(ActorA);
    Captured.Empty();
    SendWithEditorContext(TEXT("First turn"));
    TestTrue(TEXT("First turn carries the then-current selection"), Captured.Contains(IdentityA));

    Fixture.Select(ActorB);
    Captured.Empty();
    SendWithEditorContext(TEXT("Second turn"));
    TestTrue(TEXT("Second turn carries the new selection"), Captured.Contains(IdentityB));
    TestFalse(TEXT("Second turn does not reuse the previous selection"), Captured.Contains(IdentityA));
    TestTrue(TEXT("Second turn preserves its user message"), Captured.Contains(TEXT("Second turn")));

    return true;
}

// Task 16: disabled auto-context must still resolve an explicit provider chip exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaResolutionDisabledAutoContextProviderChipTest,
    "Cortex.Frontend.InputArea.Resolution.DisabledAutoContextStillResolvesProviderChipOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaResolutionDisabledAutoContextProviderChipTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FTask16ActorFixture Fixture;
    if (!Fixture.Begin()) { AddInfo(TEXT("No editor world available")); return true; }

    AActor* SelectedActor = Fixture.Spawn(TEXT("CortexTask16DisabledActor"));
    if (!TestNotNull(TEXT("Spawned task-owned selected actor"), SelectedActor))
    {
        return true;
    }
    Fixture.Select(SelectedActor);
    const FString SelectedIdentity = SelectedActor->GetActorLabel();

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginalAutoContext = Settings.GetAutoContext();
    ON_SCOPE_EXIT { Settings.SetAutoContext(bOriginalAutoContext); Settings.ClearPendingChanges(); };
    Settings.SetAutoContext(false);

    FString Captured;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&Captured](const FString& Prompt) { Captured = Prompt; });

    FCortexContextChip EditorContextChip;
    EditorContextChip.Kind = ECortexContextChipKind::Provider;
    EditorContextChip.Label = TEXT("editorContext");
    Widget->AddContextChip(EditorContextChip);

    Widget->TestResolveAndSend(TEXT("Hello"));

    TestEqual(TEXT("Explicit editorContext chip still resolves exactly once"),
        Task16CountOccurrences(Captured, TEXT("## Editor Context (auto)")), 1);
    TestEqual(TEXT("Selected actor identity emitted exactly once"),
        Task16CountOccurrences(Captured, SelectedIdentity), 1);
    TestTrue(TEXT("User message preserved"), Captured.Contains(TEXT("Hello")));

    return true;
}

// Task 16: a provider that resolves to nothing (or fails) must be dropped, not surfaced
// as a bogus context block, and other chip fallbacks must survive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexInputAreaResolutionProviderFallbackTest,
    "Cortex.Frontend.InputArea.Resolution.FailedProviderAndAssetFallbacksRemain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexInputAreaResolutionProviderFallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }

    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginalAutoContext = Settings.GetAutoContext();
    ON_SCOPE_EXIT { Settings.SetAutoContext(bOriginalAutoContext); Settings.ClearPendingChanges(); };
    Settings.SetAutoContext(false);

    FString Captured;
    TSharedRef<SCortexInputArea> Widget = SNew(SCortexInputArea)
        .OnSendMessage_Lambda([&Captured](const FString& Prompt) { Captured = Prompt; });

    FCortexContextChip FailedProvider;
    FailedProvider.Kind = ECortexContextChipKind::Provider;
    FailedProvider.Label = TEXT("unknownProvider");
    Widget->AddContextChip(FailedProvider);

    FCortexContextChip AssetChip;
    AssetChip.Kind = ECortexContextChipKind::Asset;
    AssetChip.Label = TEXT("/Game/SomeUnknownAsset");
    AssetChip.RouterCommand = TEXT("");
    Widget->AddContextChip(AssetChip);

    Widget->TestResolveAndSend(TEXT("Hello"));

    TestFalse(TEXT("Failed provider emits no context block"),
        Captured.Contains(TEXT("## Context: unknownProvider")));
    TestTrue(TEXT("Unmapped asset still falls back to its @path"),
        Captured.Contains(TEXT("@/Game/SomeUnknownAsset")));
    TestTrue(TEXT("User message preserved"), Captured.Contains(TEXT("Hello")));

    return true;
}
