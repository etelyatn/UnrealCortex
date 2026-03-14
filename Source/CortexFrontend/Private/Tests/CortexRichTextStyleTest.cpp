#include "Misc/AutomationTest.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Styling/SlateStyleRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexRichTextStyleRegistrationTest,
    "Cortex.Frontend.RichTextStyle.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexRichTextStyleRegistrationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Initialize if not already (module startup handles this in production)
    FCortexRichTextStyle::Initialize();

    const ISlateStyle& Style = FCortexRichTextStyle::Get();

    // Verify the three required text styles exist
    TestTrue(TEXT("Should have Bold style"),
        Style.HasWidgetStyle<FTextBlockStyle>(FName("Bold")));
    TestTrue(TEXT("Should have Italic style"),
        Style.HasWidgetStyle<FTextBlockStyle>(FName("Italic")));
    TestTrue(TEXT("Should have Code style"),
        Style.HasWidgetStyle<FTextBlockStyle>(FName("Code")));

    // Clean up
    FCortexRichTextStyle::Shutdown();

    return true;
}
