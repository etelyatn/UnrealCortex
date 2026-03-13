#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "ICortexDomainHandler.h"
#include "CortexCoreModule.h"
#include "HAL/FileManager.h"
#include "Containers/Ticker.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
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

FString GetCapabilitiesCachePath()
{
	return FPaths::ProjectSavedDir() / TEXT("Cortex/capabilities-cache.json");
}

void DeleteCapabilitiesCache()
{
	const FString CachePath = GetCapabilitiesCachePath();
	IFileManager::Get().Delete(*CachePath);
	IFileManager::Get().Delete(*(CachePath + TEXT(".tmp")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesParamSerializationTest,
	"Cortex.Core.Capabilities.ParamSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesParamSerializationTest::RunTest(const FString& Parameters)
{
	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	CoreModule.GetCommandRegistry().RegisterDomain(
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesCacheCreationTest,
	"Cortex.Core.Cache.CapabilitiesCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesCacheCreationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	DeleteCapabilitiesCache();

	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("cache_test"),
			TEXT("Cache Test"),
			TEXT("1.0.0"),
			MakeShared<FCapabilitiesTestHandler>());

		FTSTicker::GetCoreTicker().Tick(0.016f);
		TestTrue(TEXT("Capabilities cache created after ticker"), FPaths::FileExists(GetCapabilitiesCachePath()));
	}

	DeleteCapabilitiesCache();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesCacheRecreationTest,
	"Cortex.Core.Cache.CapabilitiesRecreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesCacheRecreationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	DeleteCapabilitiesCache();

	FCortexCommandRouter Router;
	Router.RegisterDomain(
		TEXT("cache_test"),
		TEXT("Cache Test"),
		TEXT("1.0.0"),
		MakeShared<FCapabilitiesTestHandler>());

	FTSTicker::GetCoreTicker().Tick(0.016f);
	const FString CachePath = GetCapabilitiesCachePath();
	TestTrue(TEXT("Capabilities cache created after ticker"), FPaths::FileExists(CachePath));

	IFileManager::Get().Delete(*CachePath);
	TestFalse(TEXT("Capabilities cache removed before regeneration"), FPaths::FileExists(CachePath));

	const FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);
	TestTrue(TEXT("Capabilities cache recreated on get_capabilities"), FPaths::FileExists(CachePath));

	DeleteCapabilitiesCache();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesCacheDestroyBeforeTickerTest,
	"Cortex.Core.Cache.DestroyBeforeTicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesCacheDestroyBeforeTickerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	DeleteCapabilitiesCache();

	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("cache_test"),
			TEXT("Cache Test"),
			TEXT("1.0.0"),
			MakeShared<FCapabilitiesTestHandler>());
	}

	TestTrue(TEXT("Router destruction before ticker does not crash"), true);
	DeleteCapabilitiesCache();
	return true;
}
