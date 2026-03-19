#include "Misc/AutomationTest.h"
#include "Utilities/CortexTokenUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenUtilsEstimateFromJsonTest, "Cortex.Frontend.TokenUtils.EstimateFromJson", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenUtilsEstimateSecondsTest, "Cortex.Frontend.TokenUtils.EstimateSeconds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenUtilsFormatEstimateTest, "Cortex.Frontend.TokenUtils.FormatEstimate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenUtilsFormatCountTest, "Cortex.Frontend.TokenUtils.FormatCount", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenUtilsEstimateScopeTest, "Cortex.Frontend.TokenUtils.EstimateScope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexTokenUtilsEstimateFromJsonTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TestEqual(TEXT("4000 chars -> 1000 tokens"), CortexTokenUtils::EstimateTokensFromJson(4000), 1000);
    TestEqual(TEXT("0 chars -> 0 tokens"), CortexTokenUtils::EstimateTokensFromJson(0), 0);
    TestEqual(TEXT("3 chars -> 0 tokens (integer division)"), CortexTokenUtils::EstimateTokensFromJson(3), 0);
    TestEqual(TEXT("100 chars -> 25 tokens"), CortexTokenUtils::EstimateTokensFromJson(100), 25);
    return true;
}

bool FCortexTokenUtilsEstimateSecondsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Formula: 10 + tokens/1000*10 + gapBuffer
    TestEqual(TEXT("1000 tokens -> 20s"), FMath::RoundToInt(CortexTokenUtils::EstimateSecondsForTokens(1000)), 20);
    TestEqual(TEXT("6000 tokens -> 85s"), FMath::RoundToInt(CortexTokenUtils::EstimateSecondsForTokens(6000)), 85);
    TestEqual(TEXT("25000 tokens -> 290s"), FMath::RoundToInt(CortexTokenUtils::EstimateSecondsForTokens(25000)), 290);
    TestEqual(TEXT("0 tokens -> 0s"), FMath::RoundToInt(CortexTokenUtils::EstimateSecondsForTokens(0)), 0);
    return true;
}

bool FCortexTokenUtilsFormatEstimateTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Result = CortexTokenUtils::FormatTokenEstimate(1000);
    TestTrue(TEXT("Should contain ~1k tokens"), Result.Contains(TEXT("~1k tokens")));
    TestTrue(TEXT("Should contain est."), Result.Contains(TEXT("est.")));
    TestTrue(TEXT("0 tokens -> empty"), CortexTokenUtils::FormatTokenEstimate(0).IsEmpty());
    Result = CortexTokenUtils::FormatTokenEstimate(25000);
    TestTrue(TEXT("Large should contain 'm'"), Result.Contains(TEXT("m")));
    return true;
}

bool FCortexTokenUtilsFormatCountTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TestEqual(TEXT("500 tokens"), CortexTokenUtils::FormatTokenCount(500), FString(TEXT("~500 tokens")));
    TestEqual(TEXT("2500 tokens"), CortexTokenUtils::FormatTokenCount(2500), FString(TEXT("~2.5k tokens")));
    TestEqual(TEXT("15000 tokens"), CortexTokenUtils::FormatTokenCount(15000), FString(TEXT("~15k tokens")));
    TestTrue(TEXT("0 tokens -> empty"), CortexTokenUtils::FormatTokenCount(0).IsEmpty());
    TestTrue(TEXT("-1 tokens -> empty"), CortexTokenUtils::FormatTokenCount(-1).IsEmpty());
    return true;
}

bool FCortexTokenUtilsEstimateScopeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // EntireBlueprint -> full total
    TestEqual(TEXT("EntireBlueprint returns total"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::EntireBlueprint,
            10000, 3, 100, 10, {}, {}),
        10000);

    // CurrentGraph -> total / numGraphs
    TestEqual(TEXT("CurrentGraph divides by graph count"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::CurrentGraph,
            9000, 3, 100, 0, {}, {}),
        3000);

    // SelectedNodes -> proportional to node selection ratio
    TestEqual(TEXT("SelectedNodes proportional: 5 of 100 nodes = 500"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::SelectedNodes,
            10000, 3, 100, 5, {}, {}),
        500);

    // SelectedNodes with TotalNodeCount=0 falls back to minimum 500
    TestEqual(TEXT("SelectedNodes with 0 total nodes -> minimum 500"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::SelectedNodes,
            10000, 3, 0, 5, {}, {}),
        500);

    // EventOrFunction with per-function data -> sums selected
    TArray<FString> SelectedFunctions = { TEXT("EventBeginPlay"), TEXT("Tick") };
    TMap<FString, int32> PerFunctionTokens;
    PerFunctionTokens.Add(TEXT("EventBeginPlay"), 2000);
    PerFunctionTokens.Add(TEXT("Tick"), 3000);
    TestEqual(TEXT("EventOrFunction sums per-function"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::EventOrFunction,
            10000, 3, 100, 0, SelectedFunctions, PerFunctionTokens),
        5000);

    // EventOrFunction without per-function data -> proportional fallback
    // 2 selected / 5 total = 10000 * 2 / 5 = 4000
    TMap<FString, int32> EmptyMap;
    TestEqual(TEXT("EventOrFunction fallback: 2 of 5 functions = 4000"),
        CortexTokenUtils::EstimateTokensForScope(
            ECortexConversionScope::EventOrFunction,
            10000, 3, 100, 0, SelectedFunctions, EmptyMap, 5),
        4000);

    return true;
}
