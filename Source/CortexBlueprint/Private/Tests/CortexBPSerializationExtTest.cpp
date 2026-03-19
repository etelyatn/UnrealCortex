// CortexBPSerializationExtTest.cpp
#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Operations/CortexBPSerializationOps.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializationNodeMappingTest,
	"Cortex.Blueprint.Serialization.NodeIdMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializationNodeMappingTest::RunTest(const FString& Parameters)
{
	// Create a test Blueprint
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();

	const FString PackagePath = TEXT("/Game/__CortexTest/SerExt_Mapping");
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* TestBP = Cast<UBlueprint>(Factory->FactoryCreateNew(
		UBlueprint::StaticClass(), Package, TEXT("SerExt_Mapping"),
		RF_Transient, nullptr, GWarn));

	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) return false;

	// Request serialization with node ID mapping
	FCortexSerializationRequest Request;
	Request.BlueprintPath = TestBP->GetPathName();
	Request.Scope = ECortexConversionScope::EntireBlueprint;
	Request.bConversionMode = true;  // Use compact format
	Request.bBuildNodeIdMapping = true;

	bool bGotResult = false;
	FCortexBPSerializationOps::Serialize(Request,
		FOnSerializationComplete::CreateLambda(
			[&](const FCortexSerializationResult& Result)
			{
				bGotResult = true;
				TestTrue(TEXT("Serialization succeeded"), Result.bSuccess);
				TestTrue(TEXT("JSON is non-empty"), Result.JsonPayload.Len() > 0);
				TestTrue(TEXT("NodeIdMapping is non-empty"), Result.NodeIdMapping.Num() > 0);
				TestTrue(TEXT("NodeDisplayNames is non-empty"), Result.NodeDisplayNames.Num() > 0);
				TestEqual(TEXT("Mapping and display names have same count"),
					Result.NodeIdMapping.Num(), Result.NodeDisplayNames.Num());

				// Verify all mapped GUIDs are valid (non-zero)
				for (const auto& Pair : Result.NodeIdMapping)
				{
					TestTrue(
						FString::Printf(TEXT("Node %d has valid GUID"), Pair.Key),
						Pair.Value.IsValid());
				}
			}));

	TestTrue(TEXT("Callback was invoked"), bGotResult);

	// Cleanup
	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializationPositionsTest,
	"Cortex.Blueprint.Serialization.IncludePositions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializationPositionsTest::RunTest(const FString& Parameters)
{
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();

	const FString PackagePath = TEXT("/Game/__CortexTest/SerExt_Positions");
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* TestBP = Cast<UBlueprint>(Factory->FactoryCreateNew(
		UBlueprint::StaticClass(), Package, TEXT("SerExt_Positions"),
		RF_Transient, nullptr, GWarn));

	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) return false;

	// Without positions
	FCortexSerializationRequest ReqNoPos;
	ReqNoPos.BlueprintPath = TestBP->GetPathName();
	ReqNoPos.Scope = ECortexConversionScope::EntireBlueprint;
	ReqNoPos.bConversionMode = true;
	ReqNoPos.bIncludePositions = false;

	FString JsonNoPos;
	FCortexBPSerializationOps::Serialize(ReqNoPos,
		FOnSerializationComplete::CreateLambda(
			[&](const FCortexSerializationResult& Result)
			{
				JsonNoPos = Result.JsonPayload;
			}));

	// With positions
	FCortexSerializationRequest ReqWithPos;
	ReqWithPos.BlueprintPath = TestBP->GetPathName();
	ReqWithPos.Scope = ECortexConversionScope::EntireBlueprint;
	ReqWithPos.bConversionMode = true;
	ReqWithPos.bIncludePositions = true;

	FString JsonWithPos;
	FCortexBPSerializationOps::Serialize(ReqWithPos,
		FOnSerializationComplete::CreateLambda(
			[&](const FCortexSerializationResult& Result)
			{
				JsonWithPos = Result.JsonPayload;
			}));

	// JSON with positions should contain "x" and "y" fields
	TestTrue(TEXT("Positions JSON contains 'x' field"), JsonWithPos.Contains(TEXT("\"x\"")));
	TestTrue(TEXT("Positions JSON contains 'y' field"), JsonWithPos.Contains(TEXT("\"y\"")));
	TestFalse(TEXT("No-positions JSON lacks 'x' field"), JsonNoPos.Contains(TEXT("\"x\"")));

	// Cleanup
	TestBP->MarkAsGarbage();
	return true;
}
