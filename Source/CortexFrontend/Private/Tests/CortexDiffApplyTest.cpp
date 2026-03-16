#include "Misc/AutomationTest.h"
#include "Conversion/CortexDiffParser.h"

// ---------------------------------------------------------------------------
// Helper: simulate the production apply algorithm from SCortexConversionChat
// Continues past failures (partial apply), detects ambiguity, uses accumulator.
// ---------------------------------------------------------------------------
namespace
{
    struct FApplyResult
    {
        FString ResultText;
        int32 AppliedCount = 0;
        int32 SkippedNotFound = 0;
        int32 SkippedAmbiguous = 0;
    };

    FApplyResult ApplySearchReplacePairs(
        const FString& OriginalText,
        const TArray<FCortexFrontendSearchReplacePair>& Pairs)
    {
        FApplyResult Result;
        FString WorkingText = OriginalText;
        WorkingText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

        for (int32 i = 0; i < Pairs.Num(); ++i)
        {
            const int32 Pos = WorkingText.Find(
                Pairs[i].SearchText, ESearchCase::CaseSensitive, ESearchDir::FromStart);

            if (Pos == INDEX_NONE)
            {
                ++Result.SkippedNotFound;
                continue;
            }

            // Check ambiguity: second occurrence of same search text
            const int32 Pos2 = WorkingText.Find(
                Pairs[i].SearchText, ESearchCase::CaseSensitive, ESearchDir::FromStart,
                Pos + Pairs[i].SearchText.Len());
            if (Pos2 != INDEX_NONE)
            {
                ++Result.SkippedAmbiguous;
                continue;
            }

            // Splice — replace first (and only) occurrence
            WorkingText = WorkingText.Left(Pos)
                + Pairs[i].ReplaceText
                + WorkingText.Mid(Pos + Pairs[i].SearchText.Len());
            ++Result.AppliedCount;
        }

        Result.ResultText = WorkingText;
        return Result;
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

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 1"), Result.AppliedCount, 1);
    TestEqual(TEXT("Result"), Result.ResultText, FString(TEXT("float JumpForce = 1000.0f;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: multiple pairs applied in order (accumulator)
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

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 2"), Result.AppliedCount, 2);
    TestEqual(TEXT("Result"),
        Result.ResultText,
        FString(TEXT("int A = 2;\n") TEXT("int B = 4;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: search text not found — skips pair, continues
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

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 0"), Result.AppliedCount, 0);
    TestEqual(TEXT("SkippedNotFound should be 1"), Result.SkippedNotFound, 1);
    TestEqual(TEXT("ResultText unchanged"), Result.ResultText, Original);

    return true;
}

// ---------------------------------------------------------------------------
// Test: partial failure — first pair applies, second pair not found
//       Production continues and applies what it can (partial apply)
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
    Pairs.Add({ TEXT("int A = 1;\n"), TEXT("int A = 2;\n") });          // found, applied
    Pairs.Add({ TEXT("int MISSING = 99;\n"), TEXT("int MISSING = 0;\n") });  // not found, skipped

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    // Production semantics: continues past failure, applies pair 1, warns about pair 2
    TestEqual(TEXT("AppliedCount should be 1"), Result.AppliedCount, 1);
    TestEqual(TEXT("SkippedNotFound should be 1"), Result.SkippedNotFound, 1);
    // Result contains pair 1's change
    TestTrue(TEXT("Result should contain applied change"),
        Result.ResultText.Contains(TEXT("int A = 2;")));
    // Result preserves non-matched content
    TestTrue(TEXT("Result should preserve other content"),
        Result.ResultText.Contains(TEXT("int B = 3;")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: ambiguous match (search text appears twice) — pair is skipped
//       Production detects ambiguity and skips rather than guessing
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyAmbiguousTest,
    "Cortex.Frontend.DiffApply.Ambiguous",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyAmbiguousTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Two identical lines — ambiguous, should be skipped
    const FString Original =
        TEXT("int X = 0;\n")
        TEXT("int X = 0;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("int X = 0;\n"), TEXT("int X = 1;\n") });

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 0"), Result.AppliedCount, 0);
    TestEqual(TEXT("SkippedAmbiguous should be 1"), Result.SkippedAmbiguous, 1);
    TestEqual(TEXT("ResultText unchanged"), Result.ResultText, Original);

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

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 1"), Result.AppliedCount, 1);
    TestEqual(TEXT("Result should have line deleted"),
        Result.ResultText,
        FString(TEXT("int X = 0;\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: accumulator order — second pair's search matches result of first pair
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffApplyAccumulatorOrderTest,
    "Cortex.Frontend.DiffApply.AccumulatorOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffApplyAccumulatorOrderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Second pair's search text matches the RESULT of the first pair
    const FString Original = TEXT("AAA\nBBB\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    Pairs.Add({ TEXT("AAA\n"), TEXT("CCC\n") });
    Pairs.Add({ TEXT("CCC\n"), TEXT("DDD\n") });  // matches first pair's output

    const FApplyResult Result = ApplySearchReplacePairs(Original, Pairs);

    TestEqual(TEXT("AppliedCount should be 2"), Result.AppliedCount, 2);
    TestEqual(TEXT("Result"), Result.ResultText, FString(TEXT("DDD\nBBB\n")));

    return true;
}
