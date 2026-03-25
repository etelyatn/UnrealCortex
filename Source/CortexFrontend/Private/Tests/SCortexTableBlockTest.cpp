#include "Misc/AutomationTest.h"
#include "Widgets/SCortexTableBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTableCellClassifyTest,
    "Cortex.Frontend.TableBlock.ClassifyTableCell",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexTableCellClassifyTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("pass"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("Pass"))),
        static_cast<int32>(ECortexTableCellType::Badge_OK));
    TestEqual(TEXT("fail"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("Fail"))),
        static_cast<int32>(ECortexTableCellType::Badge_Error));
    TestEqual(TEXT("skip"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("Skip"))),
        static_cast<int32>(ECortexTableCellType::Badge_Info));
    TestEqual(TEXT("92%"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("92%"))),
        static_cast<int32>(ECortexTableCellType::Progress));
    TestEqual(TEXT("path"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("/Game/Maps/Test"))),
        static_cast<int32>(ECortexTableCellType::Mono));
    TestEqual(TEXT("plain"), static_cast<int32>(SCortexTableBlock::ClassifyTableCell(TEXT("Hello"))),
        static_cast<int32>(ECortexTableCellType::Plain));

    return true;
}
