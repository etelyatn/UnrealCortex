#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGModuleLoadTest,
    "Cortex.UMG.ModuleLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGModuleLoadTest::RunTest(const FString& Parameters)
{
    const bool bLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexUMG"));
    TestTrue(TEXT("CortexUMG module should be loaded"), bLoaded);

    FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
    FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

    bool bFound = false;
    for (const FCortexRegisteredDomain& Domain : Router.GetRegisteredDomains())
    {
        if (Domain.Namespace == TEXT("umg"))
        {
            bFound = true;
            TestEqual(TEXT("Domain display name"), Domain.DisplayName, FString(TEXT("Cortex UMG")));
            TestEqual(TEXT("Domain version"), Domain.Version, FString(TEXT("1.0.0")));
            TestTrue(TEXT("Handler should be valid"), Domain.Handler.IsValid());
            break;
        }
    }
    TestTrue(TEXT("Domain 'umg' should be registered"), bFound);

    return true;
}
