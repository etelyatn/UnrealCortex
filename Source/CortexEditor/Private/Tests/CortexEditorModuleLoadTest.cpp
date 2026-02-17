#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorModuleLoadTest,
	"Cortex.Editor.ModuleLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorModuleLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FModuleManager::Get().LoadModule(TEXT("CortexEditor"));

	const bool bIsLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("CortexEditor"));
	TestTrue(TEXT("CortexEditor module should be loaded"), bIsLoaded);

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	const FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities should succeed"), CapResult.bSuccess);

	bool bFoundEditor = false;
	if (CapResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
		if (CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr)
		{
			bFoundEditor = (*DomainsObj)->HasField(TEXT("editor"));
		}
	}

	TestTrue(TEXT("editor domain should be registered"), bFoundEditor);

	return true;
}
