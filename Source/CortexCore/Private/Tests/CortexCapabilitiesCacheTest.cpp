#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesParamSerializationTest,
	"Cortex.Core.Capabilities.ParamSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesParamSerializationTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	ICortexCommandRegistry& Registry = CoreModule.GetCommandRegistry();

	class FCapabilitiesTestHandler : public ICortexDomainHandler
	{
	public:
		virtual FCortexCommandResult Execute(
			const FString& Command,
			const TSharedPtr<FJsonObject>& Params,
			FDeferredResponseCallback DeferredCallback = nullptr) override
		{
			(void)Command;
			(void)Params;
			(void)DeferredCallback;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		}

		virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
		{
			return {
				FCortexCommandInfo{ TEXT("query_datatable"), TEXT("Query rows") }
					.Required(TEXT("table_path"), TEXT("string"), TEXT("Full asset path"))
					.Optional(TEXT("limit"), TEXT("number"), TEXT("Max rows")),
				{ TEXT("list_gameplay_tags"), TEXT("List all gameplay tags") }
			};
		}
	};

	Registry.RegisterDomain(
		TEXT("capabilities_test"),
		TEXT("Capabilities Test"),
		TEXT("1.0.0"),
		MakeShared<FCapabilitiesTestHandler>());

	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();
	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
	TestTrue(
		TEXT("Capabilities contain domains object"),
		CapResult.Data.IsValid() && CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr);

	if (DomainsObj == nullptr)
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* TestDomainObj = nullptr;
	TestTrue(
		TEXT("Capabilities include test domain"),
		(*DomainsObj)->TryGetObjectField(TEXT("capabilities_test"), TestDomainObj) && TestDomainObj != nullptr);

	if (TestDomainObj == nullptr)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	TestTrue(
		TEXT("Capabilities include commands array"),
		(*TestDomainObj)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr);

	if (Commands == nullptr || Commands->Num() != 2)
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* QueryCmd = nullptr;
	const TSharedPtr<FJsonObject>* PlainCmd = nullptr;
	TestTrue(TEXT("First command is object"), (*Commands)[0]->TryGetObject(QueryCmd) && QueryCmd != nullptr);
	TestTrue(TEXT("Second command is object"), (*Commands)[1]->TryGetObject(PlainCmd) && PlainCmd != nullptr);

	if (QueryCmd == nullptr || PlainCmd == nullptr)
	{
		return false;
	}

	TestTrue(TEXT("Enriched command has params"), (*QueryCmd)->HasTypedField<EJson::Array>(TEXT("params")));
	TestFalse(TEXT("Plain command omits params"), (*PlainCmd)->HasField(TEXT("params")));

	return true;
}
