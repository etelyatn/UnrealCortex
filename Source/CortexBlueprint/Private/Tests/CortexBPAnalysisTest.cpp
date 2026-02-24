#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeForMigrationBasicTest,
	"Cortex.Blueprint.AnalyzeForMigration.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAnalyzeForMigrationBasicTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> AnalyzeParams = MakeShared<FJsonObject>();
	AnalyzeParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_ComplexActor"));

	FCortexCommandResult AnalyzeResult = Handler.Execute(TEXT("analyze_for_migration"), AnalyzeParams);

	TestTrue(TEXT("AnalyzeForMigration command should succeed"), AnalyzeResult.bSuccess);
	TestTrue(TEXT("AnalyzeForMigration should return data"), AnalyzeResult.Data.IsValid());

	if (AnalyzeResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* TimelinesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* DispatchersArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* LatentNodesArray = nullptr;
		const TSharedPtr<FJsonObject>* ComplexityObj = nullptr;

		TestTrue(TEXT("variables field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("variables"), VariablesArray));
		TestTrue(TEXT("functions field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));
		TestTrue(TEXT("components field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("components"), ComponentsArray));
		TestTrue(TEXT("graphs field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));
		TestTrue(TEXT("timelines field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("timelines"), TimelinesArray));
		TestTrue(TEXT("event_dispatchers field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("event_dispatchers"), DispatchersArray));
		TestTrue(TEXT("interfaces_implemented field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("interfaces_implemented"), InterfacesArray));
		TestTrue(TEXT("latent_nodes field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("latent_nodes"), LatentNodesArray));
		TestTrue(TEXT("complexity_metrics field should exist"),
			AnalyzeResult.Data->TryGetObjectField(TEXT("complexity_metrics"), ComplexityObj));
	}

	return true;
}
