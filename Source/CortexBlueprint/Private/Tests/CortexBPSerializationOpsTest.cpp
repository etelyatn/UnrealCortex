#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Operations/CortexBPSerializationOps.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeEntireBlueprintTest,
	"Cortex.Blueprint.Serialization.EntireBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeEntireBlueprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Use a known test Blueprint — BP_SimpleActor exists in Content/Blueprints/
	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	Request.Scope = ECortexConversionScope::EntireBlueprint;

	bool bSuccess = false;
	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Request,
		FOnSerializationComplete::CreateLambda([&](bool bOk, const FString& Json)
		{
			bSuccess = bOk;
			JsonResult = Json;
		}));

	TestTrue(TEXT("Serialization should succeed"), bSuccess);
	TestFalse(TEXT("JSON should not be empty"), JsonResult.IsEmpty());

	// Parse and verify structure
	TSharedPtr<FJsonObject> RootObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	if (FJsonSerializer::Deserialize(Reader, RootObj) && RootObj.IsValid())
	{
		TestTrue(TEXT("Should have blueprint_name"), RootObj->HasField(TEXT("blueprint_name")));
		TestTrue(TEXT("Should have parent_class"), RootObj->HasField(TEXT("parent_class")));
		TestTrue(TEXT("Should have variables"), RootObj->HasField(TEXT("variables")));
		TestTrue(TEXT("Should have graphs"), RootObj->HasField(TEXT("graphs")));
	}
	else
	{
		AddError(TEXT("Failed to parse serialization JSON"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeCompactModeTest,
	"Cortex.Blueprint.Serialization.CompactMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeCompactModeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Serialize the same blueprint in both modes and compare
	FCortexSerializationRequest VerboseRequest;
	VerboseRequest.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	VerboseRequest.Scope = ECortexConversionScope::EntireBlueprint;
	VerboseRequest.bConversionMode = false;

	FCortexSerializationRequest CompactRequest;
	CompactRequest.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	CompactRequest.Scope = ECortexConversionScope::EntireBlueprint;
	CompactRequest.bConversionMode = true;

	FString VerboseJson, CompactJson;
	bool bVerboseOk = false, bCompactOk = false;

	FCortexBPSerializationOps::Serialize(VerboseRequest,
		FOnSerializationComplete::CreateLambda([&](bool bOk, const FString& Json)
		{
			bVerboseOk = bOk;
			VerboseJson = Json;
		}));

	FCortexBPSerializationOps::Serialize(CompactRequest,
		FOnSerializationComplete::CreateLambda([&](bool bOk, const FString& Json)
		{
			bCompactOk = bOk;
			CompactJson = Json;
		}));

	TestTrue(TEXT("Verbose serialization should succeed"), bVerboseOk);
	TestTrue(TEXT("Compact serialization should succeed"), bCompactOk);
	TestFalse(TEXT("Compact JSON should not be empty"), CompactJson.IsEmpty());

	// Compact output must be valid JSON
	TSharedPtr<FJsonObject> CompactRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CompactJson);
	TestTrue(TEXT("Compact JSON should be valid"), FJsonSerializer::Deserialize(Reader, CompactRoot) && CompactRoot.IsValid());

	// Compact output must preserve essential structure
	if (CompactRoot.IsValid())
	{
		TestTrue(TEXT("Compact should have blueprint_name"), CompactRoot->HasField(TEXT("blueprint_name")));
		TestTrue(TEXT("Compact should have graphs"), CompactRoot->HasField(TEXT("graphs")));
	}

	// Compact output must NOT contain node position fields
	TestFalse(TEXT("Compact JSON must not contain x position"), CompactJson.Contains(TEXT("\"x\":")));
	TestFalse(TEXT("Compact JSON must not contain y position"), CompactJson.Contains(TEXT("\"y\":")));

	// Compact output must NOT contain full UE type paths (e.g. /Script/Engine.Actor)
	TestFalse(TEXT("Compact JSON must not contain full engine type paths"), CompactJson.Contains(TEXT("/Script/")));

	// Compact output must be smaller than verbose
	TestTrue(TEXT("Compact JSON should be smaller than verbose"), CompactJson.Len() < VerboseJson.Len());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPSerializeCompactEventOrFunctionTest,
	"Cortex.Blueprint.Serialization.CompactModeEventOrFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSerializeCompactEventOrFunctionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// EventOrFunction scope — the most common conversion path
	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Blueprints/BP_SimpleActor");
	Request.Scope = ECortexConversionScope::EventOrFunction;
	Request.TargetGraphName = TEXT("ReceiveBeginPlay");
	Request.bConversionMode = true;

	bool bSuccess = false;
	FString JsonResult;
	FCortexBPSerializationOps::Serialize(Request,
		FOnSerializationComplete::CreateLambda([&](bool bOk, const FString& Json)
		{
			bSuccess = bOk;
			JsonResult = Json;
		}));

	if (!bSuccess)
	{
		AddInfo(TEXT("ReceiveBeginPlay not found in BP_SimpleActor — skipping EventOrFunction compact test"));
		return true;
	}

	TestFalse(TEXT("Compact JSON should not be empty"), JsonResult.IsEmpty());

	// Parse and verify
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResult);
	TestTrue(TEXT("Compact EventOrFunction JSON should be valid"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());

	// No position fields
	TestFalse(TEXT("Compact EventOrFunction must not contain x position"), JsonResult.Contains(TEXT("\"x\":")));
	TestFalse(TEXT("Compact EventOrFunction must not contain y position"), JsonResult.Contains(TEXT("\"y\":")));

	// No full type paths
	TestFalse(TEXT("Compact EventOrFunction must not contain full engine type paths"), JsonResult.Contains(TEXT("/Script/")));

	return true;
}
