#include "Misc/AutomationTest.h"
#include "Conversion/CortexDiffParser.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserSinglePairTest,
    "Cortex.Frontend.DiffParser.SinglePair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserSinglePairTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("float JumpForce;\n")
        TEXT("=======\n")
        TEXT("float JumpForce = 1000.0f;\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestTrue(TEXT("Should detect as diff"), bIsDiff);
    TestEqual(TEXT("Should produce 1 pair"), Pairs.Num(), 1);
    if (Pairs.Num() == 1)
    {
        TestEqual(TEXT("SearchText"), Pairs[0].SearchText, FString(TEXT("float JumpForce;\n")));
        TestEqual(TEXT("ReplaceText"), Pairs[0].ReplaceText, FString(TEXT("float JumpForce = 1000.0f;\n")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserMultiplePairsTest,
    "Cortex.Frontend.DiffParser.MultiplePairs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserMultiplePairsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("int A = 1;\n")
        TEXT("=======\n")
        TEXT("int A = 2;\n")
        TEXT(">>>>>>> REPLACE\n")
        TEXT("<<<<<<< SEARCH\n")
        TEXT("int B = 3;\n")
        TEXT("=======\n")
        TEXT("int B = 4;\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestTrue(TEXT("Should detect as diff"), bIsDiff);
    TestEqual(TEXT("Should produce 2 pairs"), Pairs.Num(), 2);
    if (Pairs.Num() == 2)
    {
        TestEqual(TEXT("Pair 1 search"), Pairs[0].SearchText, FString(TEXT("int A = 1;\n")));
        TestEqual(TEXT("Pair 1 replace"), Pairs[0].ReplaceText, FString(TEXT("int A = 2;\n")));
        TestEqual(TEXT("Pair 2 search"), Pairs[1].SearchText, FString(TEXT("int B = 3;\n")));
        TestEqual(TEXT("Pair 2 replace"), Pairs[1].ReplaceText, FString(TEXT("int B = 4;\n")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserNoMarkersTest,
    "Cortex.Frontend.DiffParser.NoMarkers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserNoMarkersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("#pragma once\nclass ATest {};");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestFalse(TEXT("Should not detect as diff"), bIsDiff);
    TestEqual(TEXT("Should produce 0 pairs"), Pairs.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserMalformedMissingSeparatorTest,
    "Cortex.Frontend.DiffParser.MalformedMissingSeparator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserMalformedMissingSeparatorTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Missing ======= separator
    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("int A = 1;\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestFalse(TEXT("Should reject malformed block"), bIsDiff);
    TestEqual(TEXT("Should produce 0 pairs"), Pairs.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserMalformedUnterminatedTest,
    "Cortex.Frontend.DiffParser.MalformedUnterminated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserMalformedUnterminatedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Missing >>>>>>> REPLACE
    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("int A = 1;\n")
        TEXT("=======\n")
        TEXT("int A = 2;\n");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestFalse(TEXT("Should reject unterminated block"), bIsDiff);
    TestEqual(TEXT("Should produce 0 pairs"), Pairs.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserEmptyReplacementTest,
    "Cortex.Frontend.DiffParser.EmptyReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserEmptyReplacementTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Delete: search text with empty replacement
    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("// TODO: remove this\n")
        TEXT("=======\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestTrue(TEXT("Should detect as diff"), bIsDiff);
    TestEqual(TEXT("Should produce 1 pair"), Pairs.Num(), 1);
    if (Pairs.Num() == 1)
    {
        TestEqual(TEXT("SearchText should have content"), Pairs[0].SearchText,
            FString(TEXT("// TODO: remove this\n")));
        TestTrue(TEXT("ReplaceText should be empty"), Pairs[0].ReplaceText.IsEmpty());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserCRLFCanonTest,
    "Cortex.Frontend.DiffParser.CRLFCanon",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserCRLFCanonTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Same content but with \r\n line endings
    const FString Input =
        TEXT("<<<<<<< SEARCH\r\n")
        TEXT("int A = 1;\r\n")
        TEXT("=======\r\n")
        TEXT("int A = 2;\r\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestTrue(TEXT("Should detect as diff with CRLF"), bIsDiff);
    TestEqual(TEXT("Should produce 1 pair"), Pairs.Num(), 1);
    if (Pairs.Num() == 1)
    {
        // After canonicalization, should be \n only
        TestFalse(TEXT("SearchText should not contain CR"),
            Pairs[0].SearchText.Contains(TEXT("\r")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserEmptySearchTextTest,
    "Cortex.Frontend.DiffParser.EmptySearchText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserEmptySearchTextTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Empty search text — would match every position, must be rejected
    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("=======\n")
        TEXT("int X = 0;\n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestFalse(TEXT("Should reject empty search text"), bIsDiff);
    TestEqual(TEXT("Should produce 0 pairs"), Pairs.Num(), 0);

    return true;
}

// ---------------------------------------------------------------------------
// NormalizeForDiff tests
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserNormalizeStripsCRTest,
    "Cortex.Frontend.DiffParser.NormalizeStripsCR",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserNormalizeStripsCRTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("line1\r\nline2\rline3\n");
    const FString Result = CortexDiffParser::NormalizeForDiff(Input);

    TestFalse(TEXT("Should not contain CR"), Result.Contains(TEXT("\r")));
    TestTrue(TEXT("Should contain line1"), Result.Contains(TEXT("line1")));
    TestTrue(TEXT("Should contain line2"), Result.Contains(TEXT("line2")));
    TestTrue(TEXT("Should contain line3"), Result.Contains(TEXT("line3")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserNormalizeStripsTrailingWSTest,
    "Cortex.Frontend.DiffParser.NormalizeStripsTrailingWS",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserNormalizeStripsTrailingWSTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("int A = 1;   \n    int B = 2;\t\n");
    const FString Result = CortexDiffParser::NormalizeForDiff(Input);

    // Trailing spaces/tabs should be stripped, but leading indentation preserved
    TestEqual(TEXT("Normalized text"),
        Result, FString(TEXT("int A = 1;\n    int B = 2;\n")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserNormalizePreservesBlankLinesTest,
    "Cortex.Frontend.DiffParser.NormalizePreservesBlankLines",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserNormalizePreservesBlankLinesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("A\n\nB\n");
    const FString Result = CortexDiffParser::NormalizeForDiff(Input);

    TestEqual(TEXT("Blank line should be preserved"),
        Result, FString(TEXT("A\n\nB\n")));

    return true;
}

// ---------------------------------------------------------------------------
// Parser test: trailing whitespace in content lines normalized
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDiffParserTrailingWSInContentTest,
    "Cortex.Frontend.DiffParser.TrailingWSInContent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDiffParserTrailingWSInContentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Content lines have trailing spaces — parser should strip them
    const FString Input =
        TEXT("<<<<<<< SEARCH\n")
        TEXT("int A = 1;   \n")
        TEXT("=======\n")
        TEXT("int A = 2;  \n")
        TEXT(">>>>>>> REPLACE");

    TArray<FCortexFrontendSearchReplacePair> Pairs;
    const bool bIsDiff = CortexDiffParser::Parse(Input, Pairs);

    TestTrue(TEXT("Should detect as diff"), bIsDiff);
    if (Pairs.Num() == 1)
    {
        // Trailing whitespace should be stripped from content
        TestEqual(TEXT("SearchText stripped"), Pairs[0].SearchText,
            FString(TEXT("int A = 1;\n")));
        TestEqual(TEXT("ReplaceText stripped"), Pairs[0].ReplaceText,
            FString(TEXT("int A = 2;\n")));
    }

    return true;
}
