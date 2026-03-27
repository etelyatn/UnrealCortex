#include "Misc/AutomationTest.h"
#include "AutoComplete/CortexAutoCompleteTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SCortexAutoCompletePopup.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFilterAndScoreExactTest,
    "Cortex.Frontend.AutoComplete.FilterScore.ExactMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFilterAndScoreExactTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TestTrue(TEXT("Exact match scores > 0"),
        CortexAutoComplete::FilterAndScore(TEXT("jump"), TEXT("BP_JumpPad")) > 0);
    TestEqual(TEXT("No match scores 0"),
        CortexAutoComplete::FilterAndScore(TEXT("xyz"), TEXT("BP_JumpPad")), 0);
    TestEqual(TEXT("Empty candidate scores 0"),
        CortexAutoComplete::FilterAndScore(TEXT("jump"), TEXT("")), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFilterAndScoreSubseqTest,
    "Cortex.Frontend.AutoComplete.FilterScore.Subsequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFilterAndScoreSubseqTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Consecutive chars score higher than scattered
    const int32 ConsecScore = CortexAutoComplete::FilterAndScore(TEXT("Jump"), TEXT("BP_JumpPad"));
    const int32 ScatterScore = CortexAutoComplete::FilterAndScore(TEXT("BJP"), TEXT("BP_JumpPad"));
    TestTrue(TEXT("Consecutive beats scattered"), ConsecScore > ScatterScore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexFilterAndScoreCaseTest,
    "Cortex.Frontend.AutoComplete.FilterScore.CaseInsensitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFilterAndScoreCaseTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const int32 LowerScore = CortexAutoComplete::FilterAndScore(TEXT("jump"), TEXT("BP_JumpPad"));
    const int32 UpperScore = CortexAutoComplete::FilterAndScore(TEXT("JUMP"), TEXT("BP_JumpPad"));
    TestTrue(TEXT("Lower matches"), LowerScore > 0);
    TestTrue(TEXT("Upper matches"), UpperScore > 0);
    TestEqual(TEXT("Lower and Upper scores match"), LowerScore, UpperScore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAutoCompletePopupConstructTest,
    "Cortex.Frontend.AutoComplete.Popup.Constructs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAutoCompletePopupConstructTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexAutoCompletePopup> Popup = SNew(SCortexAutoCompletePopup);
    TestEqual(TEXT("Initially zero items"), Popup->GetItemCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAutoCompletePopupRefreshTest,
    "Cortex.Frontend.AutoComplete.Popup.Refresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAutoCompletePopupRefreshTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate not initialized")); return true; }
    TSharedRef<SCortexAutoCompletePopup> Popup = SNew(SCortexAutoCompletePopup);

    TArray<TSharedPtr<FCortexAutoCompleteItem>> Items;
    auto A = MakeShared<FCortexAutoCompleteItem>();
    A->Name = TEXT("thisAsset");
    A->Description = TEXT("Refer to currently open asset");
    A->Kind = ECortexAutoCompleteKind::ContextProvider;
    Items.Add(A);

    auto B = MakeShared<FCortexAutoCompleteItem>();
    B->Name = TEXT("BP_JumpPad");
    B->Description = TEXT("Blueprint · LevelPrototyping");
    B->Kind = ECortexAutoCompleteKind::Asset;
    Items.Add(B);

    Popup->Refresh(Items, 0, 0); // divider after index 0
    TestEqual(TEXT("Two items after refresh"), Popup->GetItemCount(), 2);

    Popup->Refresh({}, INDEX_NONE, INDEX_NONE);
    TestEqual(TEXT("Zero items after clear refresh"), Popup->GetItemCount(), 0);

    return true;
}
