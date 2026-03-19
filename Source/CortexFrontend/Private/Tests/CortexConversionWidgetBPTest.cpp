#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Conversion/CortexConversionPrompts.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetPromptsExistTest,
    "Cortex.Frontend.Conversion.Widget.PromptsExist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetPromptsExistTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FString PerfShell = CortexConversionPrompts::WidgetDepthLayerPerformanceShell();
    TestTrue(TEXT("Widget perf shell prompt should contain BindWidget"),
        PerfShell.Contains(TEXT("BindWidget")));

    FString CppCore = CortexConversionPrompts::WidgetDepthLayerCppCore();
    TestTrue(TEXT("Widget CppCore prompt should contain NativeConstruct"),
        CppCore.Contains(TEXT("NativeConstruct")));
    TestTrue(TEXT("Widget CppCore prompt should contain NativeOnInitialized"),
        CppCore.Contains(TEXT("NativeOnInitialized")));
    TestTrue(TEXT("Widget CppCore prompt should contain BindWidget"),
        CppCore.Contains(TEXT("BindWidget")));

    FString FullExtract = CortexConversionPrompts::WidgetDepthLayerFullExtraction();
    TestTrue(TEXT("Widget FullExtraction prompt should contain NativeDestruct"),
        FullExtract.Contains(TEXT("NativeDestruct")));

    return true;
}
