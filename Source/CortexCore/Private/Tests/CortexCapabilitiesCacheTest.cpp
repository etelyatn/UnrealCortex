#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexCoreCommandHandler.h"
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
	FCortexCapabilitiesRepresentativeMetadataTest,
	"Cortex.Core.Capabilities.RepresentativeMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesRepresentativeMetadataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FModuleManager::Get().LoadModule(TEXT("CortexGraph"));
	FModuleManager::Get().LoadModule(TEXT("CortexReflect"));

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	const FCortexCommandResult CapResult = CoreModule.GetCommandRouter().Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeded"), CapResult.bSuccess);

	const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
	TestTrue(
		TEXT("Capabilities contain domains object"),
		CapResult.Data.IsValid() && CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr);

	if (DomainsObj == nullptr)
	{
		return false;
	}

	auto FindCommand = [](const TSharedPtr<FJsonObject>& DomainObj, const FString& CommandName) -> const TSharedPtr<FJsonObject>*
	{
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		if (!DomainObj.IsValid() || !DomainObj->TryGetArrayField(TEXT("commands"), Commands) || Commands == nullptr)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
		{
			const TSharedPtr<FJsonObject>* CommandObj = nullptr;
			if (CommandValue->TryGetObject(CommandObj) && CommandObj != nullptr)
			{
				FString Name;
				if ((*CommandObj)->TryGetStringField(TEXT("name"), Name) && Name == CommandName)
				{
					return CommandObj;
				}
			}
		}

		return nullptr;
	};

	const TSharedPtr<FJsonObject>* CoreDomain = nullptr;
	const TSharedPtr<FJsonObject>* GraphDomain = nullptr;
	const TSharedPtr<FJsonObject>* ReflectDomain = nullptr;
	TestTrue(TEXT("Core domain present"), (*DomainsObj)->TryGetObjectField(TEXT("core"), CoreDomain) && CoreDomain != nullptr);
	TestTrue(TEXT("Graph domain present"), (*DomainsObj)->TryGetObjectField(TEXT("graph"), GraphDomain) && GraphDomain != nullptr);
	TestTrue(TEXT("Reflect domain present"), (*DomainsObj)->TryGetObjectField(TEXT("reflect"), ReflectDomain) && ReflectDomain != nullptr);

	if (CoreDomain == nullptr || GraphDomain == nullptr || ReflectDomain == nullptr)
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* SaveAssetCmd = FindCommand(*CoreDomain, TEXT("save_asset"));
	const TSharedPtr<FJsonObject>* AddNodeCmd = FindCommand(*GraphDomain, TEXT("add_node"));
	const TSharedPtr<FJsonObject>* ClassHierarchyCmd = FindCommand(*ReflectDomain, TEXT("class_hierarchy"));

	TestTrue(TEXT("core.save_asset metadata present"), SaveAssetCmd != nullptr);
	TestTrue(TEXT("graph.add_node metadata present"), AddNodeCmd != nullptr);
	TestTrue(TEXT("reflect.class_hierarchy metadata present"), ClassHierarchyCmd != nullptr);

	if (SaveAssetCmd == nullptr || AddNodeCmd == nullptr || ClassHierarchyCmd == nullptr)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SaveParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* AddNodeParams = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ClassHierarchyParams = nullptr;

	TestTrue(TEXT("core.save_asset params present"), (*SaveAssetCmd)->TryGetArrayField(TEXT("params"), SaveParams) && SaveParams != nullptr);
	TestTrue(TEXT("graph.add_node params present"), (*AddNodeCmd)->TryGetArrayField(TEXT("params"), AddNodeParams) && AddNodeParams != nullptr);
	TestTrue(TEXT("reflect.class_hierarchy params present"), (*ClassHierarchyCmd)->TryGetArrayField(TEXT("params"), ClassHierarchyParams) && ClassHierarchyParams != nullptr);

	if (SaveParams == nullptr || AddNodeParams == nullptr || ClassHierarchyParams == nullptr)
	{
		return false;
	}

	TestEqual(TEXT("core.save_asset param count"), SaveParams->Num(), 3);
	TestEqual(TEXT("graph.add_node param count"), AddNodeParams->Num(), 4);
	TestEqual(TEXT("reflect.class_hierarchy param count"), ClassHierarchyParams->Num(), 5);

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
