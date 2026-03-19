#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionPayloadWidgetFlagTest,
    "Cortex.Frontend.Conversion.Widget.PayloadFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionPayloadWidgetFlagTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    TestFalse(TEXT("Default payload should not be widget BP"), Payload.bIsWidgetBlueprint);

    Payload.bIsWidgetBlueprint = true;
    TestTrue(TEXT("Should be widget BP after setting flag"), Payload.bIsWidgetBlueprint);

    return true;
}
