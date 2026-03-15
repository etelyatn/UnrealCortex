
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
			const TSharedPtr<FJsonObject>& Params,
			FDeferredResponseCallback DeferredCallback = nullptr) override
		{
			(void)DeferredCallback;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		}

		virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
		{
			return {
				FCortexCommandInfo{ TEXT("test_cmd"), TEXT("A test command") }
					.Required(TEXT("asset_path"), TEXT("string"), TEXT("Path to the target asset"))
			};
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

	const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
	TestTrue(
		TEXT("Capabilities contain domains object"),
		CapResult.Data.IsValid() && CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr);

	const TSharedPtr<FJsonObject>* TestDomainObj = nullptr;
	TestTrue(
		TEXT("Capabilities include test domain metadata"),
		DomainsObj != nullptr && (*DomainsObj)->TryGetObjectField(TEXT("test"), TestDomainObj) && TestDomainObj != nullptr);

	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	TestTrue(
		TEXT("Test domain publishes commands"),
		TestDomainObj != nullptr && (*TestDomainObj)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr && Commands->Num() == 1);

	const TSharedPtr<FJsonObject>* CommandObj = nullptr;
	TestTrue(
		TEXT("Test command serializes as object"),
		Commands != nullptr && (*Commands)[0]->TryGetObject(CommandObj) && CommandObj != nullptr);

	TestTrue(TEXT("Builder-backed command serializes params"), CommandObj != nullptr && (*CommandObj)->HasTypedField<EJson::Array>(TEXT("params")));

	// Verify the test domain command routes correctly
	FCortexCommandResult CmdResult = Router.Execute(TEXT("test.test_cmd"), MakeShared<FJsonObject>());
	TestTrue(TEXT("test.test_cmd dispatched successfully"), CmdResult.bSuccess);

	return true;
}
