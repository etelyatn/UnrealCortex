#include "Misc/AutomationTest.h"
#include "Process/CortexCliDiscovery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCliDiscoveryTest, "Cortex.Frontend.CliDiscovery.FindClaude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCliDiscoveryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexCliDiscovery::ClearCache();
    const FCortexCliInfo Info = FCortexCliDiscovery::FindClaude();
    if (Info.bIsValid)
    {
        TestFalse(TEXT("Path should not be empty when valid"), Info.Path.IsEmpty());
        AddInfo(FString::Printf(TEXT("Found Claude at: %s (isCmd=%d)"), *Info.Path, Info.bIsCmd));
    }
    else
    {
        AddInfo(TEXT("Claude CLI not found - search completed without crash"));
    }
    const FCortexCliInfo Info2 = FCortexCliDiscovery::FindClaude();
    TestEqual(TEXT("Second call should return same path"), Info.Path, Info2.Path);
    return true;
}
