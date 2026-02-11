
#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDomainRegistrationTest,
	"Cortex.Core.DomainRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDomainRegistrationTest::RunTest(const FString& Parameters)
{
	// Get the core module
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	// Verify GetCommandRegistry returns a valid reference
	ICortexCommandRegistry& Registry = CoreModule.GetCommandRegistry();

	// Register a test domain
	class FTestHandler : public ICortexDomainHandler
	{
	public:
		virtual FCortexCommandResult Execute(
			const FString& Command,
			const TSharedPtr<FJsonObject>& Params) override
		{
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		}

		virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
		{
			return { { TEXT("test_cmd"), TEXT("A test command") } };
		}
	};

	Registry.RegisterDomain(
		TEXT("test"),
		TEXT("Test Domain"),
		TEXT("1.0.0"),
		MakeShared<FTestHandler>()
	);

	// Verify domain was registered by executing get_capabilities
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();
	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	// Verify the test domain command routes correctly
	FCortexCommandResult CmdResult = Router.Execute(TEXT("test.test_cmd"), MakeShared<FJsonObject>());
	TestTrue(TEXT("test.test_cmd dispatched successfully"), CmdResult.bSuccess);

	return true;
}
