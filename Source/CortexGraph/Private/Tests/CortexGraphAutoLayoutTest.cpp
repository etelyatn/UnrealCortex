#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAutoLayoutTest,
	"Cortex.Graph.AutoLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAutoLayoutTest::RunTest(const FString& Parameters)
{
	// Create transient Blueprint with nodes
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphAutoLayoutTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), TestPackage, TEXT("BP_GraphAutoLayoutTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) return false;

	FString AssetPath = TestBP->GetPathName();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Add two PrintString nodes at (0,0)
	for (int32 i = 0; i < 2; ++i)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		P->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		P->SetObjectField(TEXT("params"), NP);
		FCortexCommandResult R = Router.Execute(TEXT("graph.add_node"), P);
		TestTrue(TEXT("add_node succeeded"), R.bSuccess);
	}

	// Call graph.auto_layout
	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	LayoutParams->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult LayoutResult = Router.Execute(TEXT("graph.auto_layout"), LayoutParams);

	TestTrue(TEXT("auto_layout succeeded"), LayoutResult.bSuccess);
	if (LayoutResult.bSuccess && LayoutResult.Data.IsValid())
	{
		double NodeCount = 0;
		LayoutResult.Data->TryGetNumberField(TEXT("node_count"), NodeCount);
		TestTrue(TEXT("node_count > 0"), NodeCount > 0);

		double GraphsProcessed = 0;
		LayoutResult.Data->TryGetNumberField(TEXT("graphs_processed"), GraphsProcessed);
		TestTrue(TEXT("graphs_processed > 0"), GraphsProcessed > 0);
	}

	// Test empty graph (only default event nodes)
	UPackage* EmptyPkg = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphAutoLayoutEmptyTest"), RF_Transient);
	EmptyPkg->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* EmptyBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), EmptyPkg, TEXT("BP_GraphAutoLayoutEmpty"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	if (EmptyBP)
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		EmptyParams->SetStringField(TEXT("asset_path"), EmptyBP->GetPathName());
		FCortexCommandResult EmptyResult = Router.Execute(TEXT("graph.auto_layout"), EmptyParams);
		TestTrue(TEXT("auto_layout on empty graph succeeds"), EmptyResult.bSuccess);
		EmptyBP->MarkAsGarbage();
	}

	// Test with graph_name filter
	{
		TSharedPtr<FJsonObject> FilterParams = MakeShared<FJsonObject>();
		FilterParams->SetStringField(TEXT("asset_path"), AssetPath);
		FilterParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		FCortexCommandResult FilterResult = Router.Execute(TEXT("graph.auto_layout"), FilterParams);
		TestTrue(TEXT("auto_layout with graph_name filter succeeded"), FilterResult.bSuccess);
		if (FilterResult.bSuccess && FilterResult.Data.IsValid())
		{
			double GraphsProcessed = 0;
			FilterResult.Data->TryGetNumberField(TEXT("graphs_processed"), GraphsProcessed);
			TestEqual(TEXT("Should process exactly 1 graph"), static_cast<int32>(GraphsProcessed), 1);
		}
	}

	// Test missing asset_path returns error
	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		FCortexCommandResult BadResult = Router.Execute(TEXT("graph.auto_layout"), BadParams);
		TestFalse(TEXT("auto_layout without asset_path should fail"), BadResult.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), BadResult.ErrorCode, CortexErrorCodes::InvalidField);
	}

	TestBP->MarkAsGarbage();
	return true;
}
