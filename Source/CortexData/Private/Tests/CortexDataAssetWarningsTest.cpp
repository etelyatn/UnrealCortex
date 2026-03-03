#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"

namespace
{
	FCortexCommandRouter CreateDataRouterForWarningTest()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.0"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetDataAssetNoWarningsTest,
	"Cortex.Data.Warnings.GetDataAsset.NoWarningsOnCleanAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetDataAssetNoWarningsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandRouter Router = CreateDataRouterForWarningTest();

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	const FCortexCommandResult ListResult = Router.Execute(TEXT("data.list_data_assets"), ListParams);

	if (!ListResult.bSuccess || !ListResult.Data.IsValid())
	{
		AddInfo(TEXT("No DataAssets available for testing - skipping"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
	if (!ListResult.Data->TryGetArrayField(TEXT("data_assets"), AssetsArray)
		|| AssetsArray == nullptr
		|| AssetsArray->Num() == 0)
	{
		AddInfo(TEXT("No DataAssets available for testing - skipping"));
		return true;
	}

	const TSharedPtr<FJsonObject>* FirstAsset = nullptr;
	if (!(*AssetsArray)[0]->TryGetObject(FirstAsset) || FirstAsset == nullptr)
	{
		AddInfo(TEXT("Could not read asset entry - skipping"));
		return true;
	}

	FString AssetPath;
	(*FirstAsset)->TryGetStringField(TEXT("path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AddInfo(TEXT("Asset path empty - skipping"));
		return true;
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult GetResult = Router.Execute(TEXT("data.get_data_asset"), GetParams);

	TestTrue(TEXT("get_data_asset should succeed"), GetResult.bSuccess);
	TestEqual(TEXT("Clean asset should have no warnings"), GetResult.Warnings.Num(), 0);

	return true;
}
