#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileTest,
	"Cortex.Blueprint.Compile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Test: compile pre-built BP_SimpleActor (already clean, should succeed)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),
			TEXT("/Game/Blueprints/BP_SimpleActor"));

		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);
		TestTrue(TEXT("compile should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bCompiled = false;
			Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
			TestTrue(TEXT("compiled should be true"), bCompiled);

			FString AssetPath;
			Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
			TestEqual(TEXT("asset_path should match"),
				AssetPath, TEXT("/Game/Blueprints/BP_SimpleActor"));
		}
	}

	// Test: compile with missing asset_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);
		TestFalse(TEXT("compile without asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: compile non-existent Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_Fake"));
		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);
		TestFalse(TEXT("compile non-existent should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be BLUEPRINT_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);
	}

	return true;
}
