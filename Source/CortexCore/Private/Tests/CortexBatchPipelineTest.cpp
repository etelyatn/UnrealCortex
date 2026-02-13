#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ── MaxBatchSize ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchMaxSize200Test,
	"Cortex.Core.Batch.MaxBatchSize200",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchMaxSize200Test::RunTest(const FString& Parameters)
{
	// MaxBatchSize should be 200
	TestEqual(TEXT("MaxBatchSize should be 200"),
		FCortexCommandRouter::MaxBatchSize, 200);

	// A batch of 201 commands should be rejected
	FCortexCommandRouter Router;

	TArray<TSharedPtr<FJsonValue>> Commands;
	for (int32 i = 0; i < 201; ++i)
	{
		TSharedPtr<FJsonObject> Cmd = MakeShared<FJsonObject>();
		Cmd->SetStringField(TEXT("command"), TEXT("ping"));
		Commands.Add(MakeShared<FJsonValueObject>(Cmd));
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestFalse(TEXT("Batch of 201 should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be BATCH_LIMIT_EXCEEDED"),
		Result.ErrorCode, CortexErrorCodes::BatchLimitExceeded);

	// A batch of 200 commands should be accepted
	Commands.SetNum(200);
	Params->SetArrayField(TEXT("commands"), Commands);
	FCortexCommandResult Result2 = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch of 200 should succeed"), Result2.bSuccess);

	return true;
}

// ── BatchRefResolutionFailed error code exists ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefErrorCodeTest,
	"Cortex.Core.Batch.RefErrorCodeExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefErrorCodeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("BatchRefResolutionFailed error code should exist"),
		CortexErrorCodes::BatchRefResolutionFailed,
		FString(TEXT("BATCH_REF_RESOLUTION_FAILED")));
	return true;
}
