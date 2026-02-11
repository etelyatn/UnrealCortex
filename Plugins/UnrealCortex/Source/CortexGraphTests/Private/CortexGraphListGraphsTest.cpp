#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphListGraphsTest,
	"Cortex.Graph.ListGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphListGraphsTest::RunTest(const FString& Parameters)
{
	// Setup: Create a transient Blueprint for testing
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphListGraphsTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_ListGraphsTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None
	);

	TestNotNull(TEXT("Blueprint should be created"), TestBP);
	if (TestBP == nullptr) return false;

	FString AssetPath = TestBP->GetPathName();

	// Register handler via router (consistent with other tests)
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Test: Call list_graphs with valid asset_path
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_graphs"), Params);
	TestTrue(TEXT("list_graphs should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray;
		TestTrue(TEXT("Result should have graphs array"),
			Result.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));

		if (GraphsArray != nullptr)
		{
			TestTrue(TEXT("graphs array should not be empty"), GraphsArray->Num() > 0);

			// Verify first graph has expected fields
			if (GraphsArray->Num() > 0)
			{
				TSharedPtr<FJsonObject> FirstGraph = (*GraphsArray)[0]->AsObject();
				TestNotNull(TEXT("First graph should be an object"), FirstGraph.Get());

				if (FirstGraph.IsValid())
				{
					FString GraphName;
					TestTrue(TEXT("Graph should have name field"),
						FirstGraph->TryGetStringField(TEXT("name"), GraphName));
					TestFalse(TEXT("Graph name should not be empty"), GraphName.IsEmpty());

					FString GraphClass;
					TestTrue(TEXT("Graph should have class field"),
						FirstGraph->TryGetStringField(TEXT("class"), GraphClass));

					int32 NodeCount;
					TestTrue(TEXT("Graph should have node_count field"),
						FirstGraph->TryGetNumberField(TEXT("node_count"), NodeCount));
					TestTrue(TEXT("node_count should be >= 0"), NodeCount >= 0);
				}
			}
		}
	}

	// Test error case: missing asset_path parameter
	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	FCortexCommandResult ErrorResult = Router.Execute(TEXT("graph.list_graphs"), EmptyParams);
	TestFalse(TEXT("list_graphs without asset_path should fail"), ErrorResult.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		ErrorResult.ErrorCode, CortexErrorCodes::InvalidField);

	// Test error case: non-existent asset
	AddExpectedError(TEXT("SkipPackage"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Failed to find object"), EAutomationExpectedErrorFlags::Contains, 0);
	TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/Blueprint"));
	FCortexCommandResult NotFoundResult = Router.Execute(TEXT("graph.list_graphs"), BadParams);
	TestFalse(TEXT("list_graphs with non-existent asset should fail"), NotFoundResult.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"),
		NotFoundResult.ErrorCode, CortexErrorCodes::AssetNotFound);

	TestBP->MarkAsGarbage();

	return true;
}
