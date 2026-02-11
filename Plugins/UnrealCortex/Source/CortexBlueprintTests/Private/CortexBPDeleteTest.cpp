#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteTest,
	"Cortex.Blueprint.Delete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint to delete
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_DeleteTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Temp/CortexBPTest_Delete"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	// Verify it exists
	{
		UObject* LoadedObj = StaticLoadObject(
			UBlueprint::StaticClass(), nullptr,
			TEXT("/Temp/CortexBPTest_Delete/BP_DeleteTest"));
		TestNotNull(TEXT("Blueprint should exist before delete"), LoadedObj);
	}

	// Test: delete
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),
			TEXT("/Temp/CortexBPTest_Delete/BP_DeleteTest"));

		FCortexCommandResult Result = Handler.Execute(TEXT("delete"), Params);
		TestTrue(TEXT("delete should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bDeleted = false;
			Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
			TestTrue(TEXT("deleted should be true"), bDeleted);
		}
	}

	// Test: delete non-existent
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_Fake"));

		FCortexCommandResult Result = Handler.Execute(TEXT("delete"), Params);
		TestFalse(TEXT("delete non-existent should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be BLUEPRINT_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);
	}

	return true;
}
