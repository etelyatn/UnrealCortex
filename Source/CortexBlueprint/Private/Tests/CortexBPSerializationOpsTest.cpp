#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Operations/CortexBPSerializationOps.h"

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
