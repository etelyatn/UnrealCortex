#include "Misc/AutomationTest.h"
#include "Widgets/SCortexCodeBlock.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeBlockConstructTest,
    "Cortex.Frontend.CodeBlock.ConstructsWithBrush",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeBlockConstructTest::RunTest(const FString& Parameters)
{
    TSharedRef<SCortexCodeBlock> Widget = SNew(SCortexCodeBlock)
        .Code(TEXT("int x = 1;"))
        .Language(TEXT("cpp"));
    TestTrue(TEXT("Has children"), Widget->GetChildren()->Num() > 0);
    return true;
}
