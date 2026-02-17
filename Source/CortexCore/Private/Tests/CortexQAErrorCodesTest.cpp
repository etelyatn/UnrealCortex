#include "Misc/AutomationTest.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAErrorCodesTest,
    "Cortex.Core.ErrorCodes.QA",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAErrorCodesTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("NAVIGATION_FAILED should be non-empty"), CortexErrorCodes::NavigationFailed.IsEmpty());
    TestFalse(TEXT("INTERACTION_FAILED should be non-empty"), CortexErrorCodes::InteractionFailed.IsEmpty());
    TestFalse(TEXT("CONDITION_TIMEOUT should be non-empty"), CortexErrorCodes::ConditionTimeout.IsEmpty());
    TestFalse(TEXT("ASSERTION_FAILED should be non-empty"), CortexErrorCodes::AssertionFailed.IsEmpty());
    TestFalse(TEXT("INVALID_CONDITION should be non-empty"), CortexErrorCodes::InvalidCondition.IsEmpty());
    TestFalse(TEXT("UNSUPPORTED_TYPE should be non-empty"), CortexErrorCodes::UnsupportedType.IsEmpty());
    TestFalse(TEXT("MOVEMENT_METHOD_UNAVAILABLE should be non-empty"), CortexErrorCodes::MovementMethodUnavailable.IsEmpty());
    return true;
}
