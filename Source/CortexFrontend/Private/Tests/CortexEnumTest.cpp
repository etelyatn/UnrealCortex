#include "Misc/AutomationTest.h"
#include "Session/CortexSessionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexEffortLevelEnumValuesTest,
    "Cortex.Frontend.ContextControls.EffortLevel.EnumValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEffortLevelEnumValuesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify all five enum values exist and have distinct uint8 values
    TestEqual(TEXT("Default is 0"), static_cast<uint8>(ECortexEffortLevel::Default), 0);
    TestEqual(TEXT("Low is 1"), static_cast<uint8>(ECortexEffortLevel::Low), 1);
    TestEqual(TEXT("Medium is 2"), static_cast<uint8>(ECortexEffortLevel::Medium), 2);
    TestEqual(TEXT("High is 3"), static_cast<uint8>(ECortexEffortLevel::High), 3);
    TestEqual(TEXT("Maximum is 4"), static_cast<uint8>(ECortexEffortLevel::Maximum), 4);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkflowModeEnumValuesTest,
    "Cortex.Frontend.ContextControls.WorkflowMode.EnumValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkflowModeEnumValuesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    TestEqual(TEXT("Direct is 0"), static_cast<uint8>(ECortexWorkflowMode::Direct), 0);
    TestEqual(TEXT("Thorough is 1"), static_cast<uint8>(ECortexWorkflowMode::Thorough), 1);
    return true;
}
