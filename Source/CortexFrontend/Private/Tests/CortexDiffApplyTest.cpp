#include "Misc/AutomationTest.h"
#include "Conversion/CortexDiffParser.h"

// ---------------------------------------------------------------------------
// Helper: simulate the accumulator apply algorithm
// Returns the modified text, or the original text on failure (setting OutFailedIndex)
// ---------------------------------------------------------------------------
namespace
{
    FString ApplySearchReplacePairs(
        const FString& OriginalText,
        const TArray<FCortexFrontendSearchReplacePair>& Pairs,
        int32& OutFailedIndex)
    {
        OutFailedIndex = -1;
        FString Current = OriginalText;

        for (int32 i = 0; i < Pairs.Num(); ++i)
        {
            const int32 Pos = Current.Find(Pairs[i].SearchText, ESearchCase::CaseSensitive);
            if (Pos == INDEX_NONE)
            {
                OutFailedIndex = i;
                return OriginalText;  // Return untouched original on failure
            }
            // Replace first occurrence only
            Current = Current.Left(Pos)
                + Pairs[i].ReplaceText
                + Current.Mid(Pos + Pairs[i].SearchText.Len());
        }

        return Current;
    }
}

// ---------------------------------------------------------------------------
// Test: single pair apply
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplySinglePairTest,
    "Cortex.Frontend.DiffApply.SinglePair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplySinglePairTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Original = TEXT("float JumpForce;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("float JumpForce;\n"), TEXT("float JumpForce = 1000.0f;\n") });

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be -1 (success)"), FailedIndex, -1);
    TestEqual(TEXT("Result"), Result, FString(TEXT("float JumpForce = 1000.0f;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: multiple pairs applied in order
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyMultiplePairsTest,
    "Cortex.Frontend.DiffApply.MultiplePairs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyMultiplePairsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Original =
        TEXT("int A = 1;\n")
        TEXT("int B = 3;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("int A = 1;\n"), TEXT("int A = 2;\n") });
    Pairs.Add({ TEXT("int B = 3;\n"), TEXT("int B = 4;\n") });

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be -1"), FailedIndex, -1);
    TestEqual(TEXT("Result"),
        Result,
        FString(TEXT("int A = 2;\n") TEXT("int B = 4;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: search text not found — returns original, sets failed index
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyNotFoundTest,
    "Cortex.Frontend.DiffApply.NotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyNotFoundTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Original = TEXT("float X = 5.0f;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("float Y = 5.0f;\n"), TEXT("float Y = 10.0f;\n") });  // Y not in text

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be 0"), FailedIndex, 0);
    TestEqual(TEXT("Result should be unchanged original"), Result, Original);

    return true;
}

// ---------------------------------------------------------------------------
// Test: second pair fails — returns original, sets failed index to 1
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyPartialFailureTest,
    "Cortex.Frontend.DiffApply.PartialFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyPartialFailureTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Original =
        TEXT("int A = 1;\n")
        TEXT("int B = 3;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("int A = 1;\n"), TEXT("int A = 2;\n") });          // ok
    Pairs.Add({ TEXT("int MISSING = 99;\n"), TEXT("int MISSING = 0;\n") });  // fails

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be 1"), FailedIndex, 1);
    TestEqual(TEXT("Result should be unchanged original"), Result, Original);

    return true;
}

// ---------------------------------------------------------------------------
// Test: first occurrence only (not all occurrences)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyFirstOccurrenceOnlyTest,
    "Cortex.Frontend.DiffApply.FirstOccurrenceOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyFirstOccurrenceOnlyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Two identical lines — only first should be replaced
    const FString Original =
        TEXT("int X = 0;\n")
        TEXT("int X = 0;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("int X = 0;\n"), TEXT("int X = 1;\n") });

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be -1"), FailedIndex, -1);
    TestEqual(TEXT("Only first occurrence replaced"),
        Result,
        FString(TEXT("int X = 1;\n") TEXT("int X = 0;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: deletion (empty replace text)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyDeletionTest,
    "Cortex.Frontend.DiffApply.Deletion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyDeletionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Original =
        TEXT("// TODO: remove this\n")
        TEXT("int X = 0;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("// TODO: remove this\n"), TEXT("") });

    int32 FailedIndex = -1;
    const FString Result = ApplySearchReplacePairs(Original, Pairs, FailedIndex);

    TestEqual(TEXT("FailedIndex should be -1"), FailedIndex, -1);
    TestEqual(TEXT("Result should have line deleted"),
        Result,
        FString(TEXT("int X = 0;\n")));

    return true;
}
