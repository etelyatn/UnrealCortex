#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchNoFilterTest,
	"Cortex.Data.SearchAssets.NoPathFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchNoFilterTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.0"), MakeShared<FCortexDataCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT("Default"));
	Params->SetNumberField(TEXT("limit"), 5);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("search_assets without path_filter should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
		Result.Data->TryGetArrayField(TEXT("assets"), AssetsArray);
		TestNotNull(TEXT("Should have assets array"), AssetsArray);
		if (AssetsArray)
		{
			TestTrue(TEXT("Should find at least one asset matching 'Default'"), AssetsArray->Num() > 0);
		}
	}

	return true;
}
