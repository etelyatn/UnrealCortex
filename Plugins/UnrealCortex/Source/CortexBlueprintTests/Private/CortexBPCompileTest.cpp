#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileTest,
	"Cortex.Blueprint.Compile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a valid Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_CompileTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Temp/CortexBPTest_Compile"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create should succeed"), Result.bSuccess);
	}

	// Test: compile a valid Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),
			TEXT("/Temp/CortexBPTest_Compile/BP_CompileTest"));

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
				AssetPath, TEXT("/Temp/CortexBPTest_Compile/BP_CompileTest"));
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

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, TEXT("/Temp/CortexBPTest_Compile/BP_CompileTest"));
	if (CreatedBP)
	{
		CreatedBP->MarkAsGarbage();
	}

	return true;
}
