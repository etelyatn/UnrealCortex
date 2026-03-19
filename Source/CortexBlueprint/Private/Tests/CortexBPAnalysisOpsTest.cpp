// CortexBPAnalysisOpsTest.cpp
#include "Misc/AutomationTest.h"
#include "Operations/CortexBPAnalysisOps.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisOpsCountNodesTest,
	"Cortex.Blueprint.AnalysisOps.CountTotalNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisOpsCountNodesTest::RunTest(const FString& Parameters)
{
	// Create a test Blueprint
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();

	const FString PackagePath = TEXT("/Game/__CortexTest/AnalysisOps_CountNodes");
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* TestBP = Cast<UBlueprint>(Factory->FactoryCreateNew(
		UBlueprint::StaticClass(), Package, TEXT("AnalysisOps_CountNodes"),
		RF_Transient, nullptr, GWarn));

	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) return false;

	// Count nodes — a fresh BP has at least 1 event node in EventGraph
	const int32 NodeCount = FCortexBPAnalysisOps::CountTotalNodes(TestBP);
	TestTrue(TEXT("Fresh BP has at least 1 node"), NodeCount >= 1);

	// Cleanup
	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisOpsPreScanEmptyTest,
	"Cortex.Blueprint.AnalysisOps.PreScan.EmptyBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisOpsPreScanEmptyTest::RunTest(const FString& Parameters)
{
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();

	const FString PackagePath = TEXT("/Game/__CortexTest/AnalysisOps_PreScan");
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* TestBP = Cast<UBlueprint>(Factory->FactoryCreateNew(
		UBlueprint::StaticClass(), Package, TEXT("AnalysisOps_PreScan"),
		RF_Transient, nullptr, GWarn));

	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) return false;

	// Pre-scan a clean BP should find no issues
	TArray<FCortexPreScanFinding> Findings = FCortexBPAnalysisOps::RunPreScan(TestBP);
	TestEqual(TEXT("Clean BP has no pre-scan findings"), Findings.Num(), 0);

	// Cleanup
	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisOpsTickCheckTest,
	"Cortex.Blueprint.AnalysisOps.IsTickEnabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisOpsTickCheckTest::RunTest(const FString& Parameters)
{
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();

	const FString PackagePath = TEXT("/Game/__CortexTest/AnalysisOps_Tick");
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* TestBP = Cast<UBlueprint>(Factory->FactoryCreateNew(
		UBlueprint::StaticClass(), Package, TEXT("AnalysisOps_Tick"),
		RF_Transient, nullptr, GWarn));

	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) return false;

	// Default AActor has Tick disabled by default in CDO
	const bool bTickEnabled = FCortexBPAnalysisOps::IsTickEnabled(TestBP);
	// AActor's default bCanEverTick is false
	TestFalse(TEXT("Default AActor BP has Tick disabled"), bTickEnabled);

	// Cleanup
	TestBP->MarkAsGarbage();
	return true;
}
