#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexBPCommandHandler.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexBPAssetOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBlueprintLevelBPSaveRejectionTest,
	"Cortex.Blueprint.LevelBP.SaveRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBlueprintLevelBPSaveRejectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("blueprint"), TEXT("Cortex Blueprint"), TEXT("1.0.0"),
		MakeShared<FCortexBPCommandHandler>());

	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("asset_path"), TEXT("__level_bp__:/Game/Maps/TestMap"));

	FCortexCommandResult Result = Router.Execute(TEXT("blueprint.save"), SaveParams);

	TestFalse(TEXT("blueprint.save on level BP path should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be LevelBlueprintSaveError"),
		Result.ErrorCode, FString(TEXT("LevelBlueprintSaveError")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBlueprintLevelBPLoadTest,
	"Cortex.Blueprint.LevelBP.LoadFromCurrentWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBlueprintLevelBPLoadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	if (!GEditor)
	{
		AddInfo(TEXT("Skipping: GEditor not available"));
		return true;
	}

	UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
	if (!CurrentWorld)
	{
		AddInfo(TEXT("Skipping: No world in editor context"));
		return true;
	}

	const FString MapPath = CurrentWorld->GetOutermost()->GetName();
	const FString SyntheticPath = FString::Printf(TEXT("__level_bp__:%s"), *MapPath);

	FString LoadError;
	UBlueprint* LSB = FCortexBPAssetOps::LoadBlueprint(SyntheticPath, LoadError);
	TestNotNull(TEXT("LoadBlueprint should return Level Script Blueprint"), LSB);
	if (!LSB)
	{
		AddError(FString::Printf(TEXT("LoadBlueprint failed: %s"), *LoadError));
		return false;
	}

	TestTrue(TEXT("LSB should be a valid Blueprint"), IsValid(LSB));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), SyntheticPath);

	FCortexCommandResult ListResult = Router.Execute(TEXT("graph.list_graphs"), ListParams);
	TestTrue(TEXT("graph.list_graphs should succeed on level BP"), ListResult.bSuccess);

	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (ListResult.Data->TryGetArrayField(TEXT("graphs"), Graphs))
		{
			bool bFoundEventGraph = false;
			for (const TSharedPtr<FJsonValue>& GraphVal : *Graphs)
			{
				const TSharedPtr<FJsonObject>* GraphObj = nullptr;
				if (GraphVal->TryGetObject(GraphObj))
				{
					FString GraphName;
					if ((*GraphObj)->TryGetStringField(TEXT("name"), GraphName) && GraphName == TEXT("EventGraph"))
					{
						bFoundEventGraph = true;
					}
				}
			}
			TestTrue(TEXT("Level BP should have EventGraph"), bFoundEventGraph);
		}
	}

	return true;
}
