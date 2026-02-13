#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexBatchScope.h"
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

// ── Deep-Copy: Original Params Not Mutated ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchDeepCopyTest,
	"Cortex.Core.Batch.DeepCopyPreservesOriginal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchDeepCopyTest::RunTest(const FString& Parameters)
{
	// Create a batch that uses $ref
	// Verify original params are not mutated after resolution
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1Params = MakeShared<FJsonObject>();
	Step1Params->SetStringField(TEXT("test_field"), TEXT("$steps[0].data.message"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));
	Step1->SetObjectField(TEXT("params"), Step1Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	// Store original value
	FString OriginalValue;
	Step1Params->TryGetStringField(TEXT("test_field"), OriginalValue);
	TestEqual(TEXT("Original value should be $ref string"), OriginalValue, TEXT("$steps[0].data.message"));

	// Execute batch
	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);

	// Verify original params NOT mutated
	FString AfterValue;
	Step1Params->TryGetStringField(TEXT("test_field"), AfterValue);
	TestEqual(TEXT("Original params should not be mutated"), AfterValue, TEXT("$steps[0].data.message"));

	return true;
}

// ── $ref: Simple Field Resolution ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefSimpleTest,
	"Cortex.Core.Batch.RefResolveSimple",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefSimpleTest::RunTest(const FString& Parameters)
{
	// Step 0: ping (returns {message: "pong"})
	// Step 1: ping with $ref to step 0's message
	// Verify step 1 succeeds and original $ref is resolved
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1Params = MakeShared<FJsonObject>();
	Step1Params->SetStringField(TEXT("verify"), TEXT("$steps[0].data.message"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));
	Step1->SetObjectField(TEXT("params"), Step1Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch with simple $ref should succeed"), Result.bSuccess);

	// Check step 1 succeeded
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 2)
	{
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bStep1Success = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bStep1Success);
			TestTrue(TEXT("Step 1 should succeed"), bStep1Success);
		}
	}

	return true;
}

// ── $ref: Future Step (Error) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefFutureStepTest,
	"Cortex.Core.Batch.RefFutureStepFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefFutureStepTest::RunTest(const FString& Parameters)
{
	// Step 0: ping with $ref to step 1 (future) -> should fail
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0Params = MakeShared<FJsonObject>();
	Step0Params->SetStringField(TEXT("invalid"), TEXT("$steps[1].data.message"));

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));
	Step0->SetObjectField(TEXT("params"), Step0Params);

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed (with step 0 failure)"), Result.bSuccess);

	// Verify step 0 failed with BatchRefResolutionFailed
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 1)
	{
		const TSharedPtr<FJsonObject>* Step0Result = nullptr;
		if ((*ResultsArray)[0]->TryGetObject(Step0Result) && Step0Result != nullptr)
		{
			bool bStep0Success = false;
			(*Step0Result)->TryGetBoolField(TEXT("success"), bStep0Success);
			TestFalse(TEXT("Step 0 should fail"), bStep0Success);

			FString ErrorCode;
			(*Step0Result)->TryGetStringField(TEXT("error_code"), ErrorCode);
			TestEqual(TEXT("Error code should be BATCH_REF_RESOLUTION_FAILED"),
				ErrorCode, CortexErrorCodes::BatchRefResolutionFailed);
		}
	}

	return true;
}

// ── $ref: Self-Reference (Error) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefSelfTest,
	"Cortex.Core.Batch.RefSelfFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefSelfTest::RunTest(const FString& Parameters)
{
	// Step 0: ping with $ref to itself (step 0) -> should fail
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0Params = MakeShared<FJsonObject>();
	Step0Params->SetStringField(TEXT("invalid"), TEXT("$steps[0].data.message"));

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));
	Step0->SetObjectField(TEXT("params"), Step0Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed (with step 0 failure)"), Result.bSuccess);

	// Verify step 0 failed with BatchRefResolutionFailed
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 1)
	{
		const TSharedPtr<FJsonObject>* Step0Result = nullptr;
		if ((*ResultsArray)[0]->TryGetObject(Step0Result) && Step0Result != nullptr)
		{
			bool bStep0Success = false;
			(*Step0Result)->TryGetBoolField(TEXT("success"), bStep0Success);
			TestFalse(TEXT("Step 0 should fail"), bStep0Success);

			FString ErrorCode;
			(*Step0Result)->TryGetStringField(TEXT("error_code"), ErrorCode);
			TestEqual(TEXT("Error code should be BATCH_REF_RESOLUTION_FAILED"),
				ErrorCode, CortexErrorCodes::BatchRefResolutionFailed);
		}
	}

	return true;
}

// ── $ref: Missing Field (Error) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefMissingFieldTest,
	"Cortex.Core.Batch.RefMissingFieldFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefMissingFieldTest::RunTest(const FString& Parameters)
{
	// Step 0: ping (returns {message: "pong"})
	// Step 1: ping with $ref to non-existent field -> should fail
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1Params = MakeShared<FJsonObject>();
	Step1Params->SetStringField(TEXT("invalid"), TEXT("$steps[0].data.nonexistent"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));
	Step1->SetObjectField(TEXT("params"), Step1Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed (with step 1 failure)"), Result.bSuccess);

	// Verify step 1 failed with BatchRefResolutionFailed
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 2)
	{
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bStep1Success = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bStep1Success);
			TestFalse(TEXT("Step 1 should fail"), bStep1Success);

			FString ErrorCode;
			(*Step1Result)->TryGetStringField(TEXT("error_code"), ErrorCode);
			TestEqual(TEXT("Error code should be BATCH_REF_RESOLUTION_FAILED"),
				ErrorCode, CortexErrorCodes::BatchRefResolutionFailed);
		}
	}

	return true;
}

// ── $ref: Escape Mechanism ($$steps[) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefEscapeTest,
	"Cortex.Core.Batch.RefEscapeLiteral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefEscapeTest::RunTest(const FString& Parameters)
{
	// Step 0: ping with $$steps[ -> should pass through as literal $steps[
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0Params = MakeShared<FJsonObject>();
	Step0Params->SetStringField(TEXT("literal"), TEXT("$$steps[0].data.message"));

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));
	Step0->SetObjectField(TEXT("params"), Step0Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch with escaped $ref should succeed"), Result.bSuccess);

	// NOTE: We can't directly inspect the executed params here without modifying ping command
	// This test verifies no error occurs - actual escape behavior will be verified in implementation
	return true;
}

// ── $ref: Malformed Reference (Error) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefMalformedTest,
	"Cortex.Core.Batch.RefMalformedFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefMalformedTest::RunTest(const FString& Parameters)
{
	// Step 0: ping (returns {message: "pong"})
	// Step 1: ping with malformed $ref (missing index) -> should fail
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1Params = MakeShared<FJsonObject>();
	Step1Params->SetStringField(TEXT("invalid"), TEXT("$steps[].data.message"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));
	Step1->SetObjectField(TEXT("params"), Step1Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed (with step 1 failure)"), Result.bSuccess);

	// Verify step 1 failed with BatchRefResolutionFailed
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 2)
	{
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bStep1Success = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bStep1Success);
			TestFalse(TEXT("Step 1 should fail"), bStep1Success);

			FString ErrorCode;
			(*Step1Result)->TryGetStringField(TEXT("error_code"), ErrorCode);
			TestEqual(TEXT("Error code should be BATCH_REF_RESOLUTION_FAILED"),
				ErrorCode, CortexErrorCodes::BatchRefResolutionFailed);
		}
	}

	return true;
}

// ── $ref: Empty Path After Index (Error) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchRefEmptyPathTest,
	"Cortex.Core.Batch.RefEmptyPathFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchRefEmptyPathTest::RunTest(const FString& Parameters)
{
	// Step 0: ping (returns {message: "pong"})
	// Step 1: ping with $ref missing field path -> should fail
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1Params = MakeShared<FJsonObject>();
	Step1Params->SetStringField(TEXT("invalid"), TEXT("$steps[0].data"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("ping"));
	Step1->SetObjectField(TEXT("params"), Step1Params);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed (with step 1 failure)"), Result.bSuccess);

	// Verify step 1 failed with BatchRefResolutionFailed
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr && ResultsArray->Num() >= 2)
	{
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bStep1Success = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bStep1Success);
			TestFalse(TEXT("Step 1 should fail"), bStep1Success);

			FString ErrorCode;
			(*Step1Result)->TryGetStringField(TEXT("error_code"), ErrorCode);
			TestEqual(TEXT("Error code should be BATCH_REF_RESOLUTION_FAILED"),
				ErrorCode, CortexErrorCodes::BatchRefResolutionFailed);
		}
	}

	return true;
}

// ── stop_on_error: Halts Execution ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchStopOnErrorHaltsTest,
	"Cortex.Core.Batch.StopOnErrorHalts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchStopOnErrorHaltsTest::RunTest(const FString& Parameters)
{
	// 3 commands: ping (succeeds), nonexistent.command (fails), ping (should NOT execute)
	// Expected: 2 results (step 0 ok, step 1 fail, step 2 not executed)
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("nonexistent.command"));

	TSharedPtr<FJsonObject> Step2 = MakeShared<FJsonObject>();
	Step2->SetStringField(TEXT("command"), TEXT("ping"));

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));
	Commands.Add(MakeShared<FJsonValueObject>(Step2));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), true);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed overall"), Result.bSuccess);

	// Verify only 2 results (step 2 was not executed)
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		TestEqual(TEXT("Should have exactly 2 results"), ResultsArray->Num(), 2);

		// Step 0 succeeded
		const TSharedPtr<FJsonObject>* Step0Result = nullptr;
		if ((*ResultsArray)[0]->TryGetObject(Step0Result) && Step0Result != nullptr)
		{
			bool bSuccess = false;
			(*Step0Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestTrue(TEXT("Step 0 should succeed"), bSuccess);
		}

		// Step 1 failed
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bSuccess = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestFalse(TEXT("Step 1 should fail"), bSuccess);
		}
	}

	return true;
}

// ── stop_on_error: Continues Past Failures ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchStopOnErrorContinuesTest,
	"Cortex.Core.Batch.StopOnErrorContinues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchStopOnErrorContinuesTest::RunTest(const FString& Parameters)
{
	// 3 commands: ping (succeeds), nonexistent.command (fails), ping (succeeds)
	// Expected: 3 results (all executed, step 1 failed, steps 0 and 2 succeeded)
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TSharedPtr<FJsonObject> Step1 = MakeShared<FJsonObject>();
	Step1->SetStringField(TEXT("command"), TEXT("nonexistent.command"));

	TSharedPtr<FJsonObject> Step2 = MakeShared<FJsonObject>();
	Step2->SetStringField(TEXT("command"), TEXT("ping"));

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));
	Commands.Add(MakeShared<FJsonValueObject>(Step1));
	Commands.Add(MakeShared<FJsonValueObject>(Step2));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	Params->SetBoolField(TEXT("stop_on_error"), false);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch should succeed overall"), Result.bSuccess);

	// Verify all 3 results (execution continued past failure)
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		TestEqual(TEXT("Should have exactly 3 results"), ResultsArray->Num(), 3);

		// Step 0 succeeded
		const TSharedPtr<FJsonObject>* Step0Result = nullptr;
		if ((*ResultsArray)[0]->TryGetObject(Step0Result) && Step0Result != nullptr)
		{
			bool bSuccess = false;
			(*Step0Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestTrue(TEXT("Step 0 should succeed"), bSuccess);
		}

		// Step 1 failed
		const TSharedPtr<FJsonObject>* Step1Result = nullptr;
		if ((*ResultsArray)[1]->TryGetObject(Step1Result) && Step1Result != nullptr)
		{
			bool bSuccess = false;
			(*Step1Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestFalse(TEXT("Step 1 should fail"), bSuccess);
		}

		// Step 2 succeeded
		const TSharedPtr<FJsonObject>* Step2Result = nullptr;
		if ((*ResultsArray)[2]->TryGetObject(Step2Result) && Step2Result != nullptr)
		{
			bool bSuccess = false;
			(*Step2Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestTrue(TEXT("Step 2 should succeed"), bSuccess);
		}
	}

	return true;
}

// ── Empty Batch ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchEmptyTest,
	"Cortex.Core.Batch.EmptyBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchEmptyTest::RunTest(const FString& Parameters)
{
	// 0 commands
	// Expected: success, count=0
	FCortexCommandRouter Router;

	TArray<TSharedPtr<FJsonValue>> Commands;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Empty batch should succeed"), Result.bSuccess);

	// Verify 0 results
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		TestEqual(TEXT("Should have 0 results"), ResultsArray->Num(), 0);
	}

	// Verify count field
	int32 Count = -1;
	if (Result.Data.IsValid() && Result.Data->TryGetNumberField(TEXT("count"), Count))
	{
		TestEqual(TEXT("Count should be 0"), Count, 0);
	}

	return true;
}

// ── Backward Compatibility ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchBackwardCompatTest,
	"Cortex.Core.Batch.BackwardCompat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchBackwardCompatTest::RunTest(const FString& Parameters)
{
	// 1 ping command, no stop_on_error param
	// Expected: success (default behavior: stop_on_error=false)
	FCortexCommandRouter Router;

	TSharedPtr<FJsonObject> Step0 = MakeShared<FJsonObject>();
	Step0->SetStringField(TEXT("command"), TEXT("ping"));

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueObject>(Step0));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("commands"), Commands);
	// Note: NOT setting stop_on_error field

	FCortexCommandResult Result = Router.Execute(TEXT("batch"), Params);
	TestTrue(TEXT("Batch without stop_on_error should succeed"), Result.bSuccess);

	// Verify 1 result
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
	if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		TestEqual(TEXT("Should have 1 result"), ResultsArray->Num(), 1);

		// Step 0 succeeded
		const TSharedPtr<FJsonObject>* Step0Result = nullptr;
		if ((*ResultsArray)[0]->TryGetObject(Step0Result) && Step0Result != nullptr)
		{
			bool bSuccess = false;
			(*Step0Result)->TryGetBoolField(TEXT("success"), bSuccess);
			TestTrue(TEXT("Step 0 should succeed"), bSuccess);
		}
	}

	return true;
}

// ── IsInBatch: RAII Guard ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchIsInBatchTest,
	"Cortex.Core.Batch.IsInBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchIsInBatchTest::RunTest(const FString& Parameters)
{
	// Outside batch, IsInBatch should be false
	TestFalse(TEXT("IsInBatch should be false outside batch"),
		FCortexCommandRouter::IsInBatch());

	// We can't easily test during batch without a custom command,
	// but we can test the RAII guard directly
	{
		FCortexBatchScope Scope;
		TestTrue(TEXT("IsInBatch should be true inside scope"),
			FCortexCommandRouter::IsInBatch());
	}
	TestFalse(TEXT("IsInBatch should be false after scope exits"),
		FCortexCommandRouter::IsInBatch());

	return true;
}
