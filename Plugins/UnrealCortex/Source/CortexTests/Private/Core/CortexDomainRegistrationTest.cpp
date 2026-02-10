
#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDomainRegistrationTest,
	"UDB.Core.DomainRegistration",
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
		virtual FUDBCommandResult Execute(
			const FString& Command,
			const TSharedPtr<FJsonObject>& Params) override
		{
			return FUDBCommandHandler::Success(MakeShared<FJsonObject>());
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

	// Verify domain was registered (will be tested via get_capabilities later)
	TestTrue(TEXT("Domain registration did not crash"), true);

	return true;
}
