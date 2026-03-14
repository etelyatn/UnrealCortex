#include "Misc/AutomationTest.h"
#include "Widgets/SCortexContextBar.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextBarCalculationTest,
    "Cortex.Frontend.ContextBar.Calculation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextBarCalculationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const float Pct = SCortexContextBar::CalculatePercentage(84000, 200000);
    TestEqual(TEXT("Should be 42%"), FMath::RoundToInt(Pct), 42);

    const float PctZero = SCortexContextBar::CalculatePercentage(0, 200000);
    TestEqual(TEXT("Zero should be 0%"), FMath::RoundToInt(PctZero), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextBarColorThresholdTest,
    "Cortex.Frontend.ContextBar.ColorThresholds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextBarColorThresholdTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FLinearColor Blue = SCortexContextBar::GetContextColor(30.0f);
    const FLinearColor Yellow = SCortexContextBar::GetContextColor(70.0f);
    const FLinearColor Red = SCortexContextBar::GetContextColor(85.0f);

    TestTrue(TEXT("30% should be blue-ish"), Blue.B > Blue.R);
    TestTrue(TEXT("70% should be yellow-ish"), Yellow.R > 0.5f && Yellow.G > 0.5f);
    TestTrue(TEXT("85% should be red-ish"), Red.R > Red.G && Red.R > Red.B);

    return true;
}
