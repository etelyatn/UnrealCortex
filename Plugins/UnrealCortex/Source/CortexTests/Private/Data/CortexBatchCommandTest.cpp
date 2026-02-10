
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUDBBatchCommandTest,
	"UDB.Commands.Batch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FUDBBatchCommandTest::RunTest(const FString& Parameters)
{
	FUDBCommandHandler Handler;

	// --- Test 1: Batch with multiple valid commands ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> Commands;

		// Command 0: ping
		TSharedRef<FJsonObject> Cmd0 = MakeShared<FJsonObject>();
		Cmd0->SetStringField(TEXT("command"), TEXT("ping"));
		Commands.Add(MakeShared<FJsonValueObject>(Cmd0));

		// Command 1: get_status
		TSharedRef<FJsonObject> Cmd1 = MakeShared<FJsonObject>();
		Cmd1->SetStringField(TEXT("command"), TEXT("get_status"));
		Commands.Add(MakeShared<FJsonValueObject>(Cmd1));

		Params->SetArrayField(TEXT("commands"), Commands);

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestTrue(TEXT("Batch should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			double Count = 0.0;
			Result.Data->TryGetNumberField(TEXT("count"), Count);
			TestEqual(TEXT("Batch count should be 2"), static_cast<int32>(Count), 2);

			double TotalTiming = 0.0;
			TestTrue(TEXT("Should have total_timing_ms"), Result.Data->TryGetNumberField(TEXT("total_timing_ms"), TotalTiming));
			TestTrue(TEXT("total_timing_ms should be >= 0"), TotalTiming >= 0.0);

			const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("results"), Results) && Results != nullptr)
			{
				TestEqual(TEXT("Should have 2 results"), Results->Num(), 2);

				// Verify result 0 (ping)
				if (Results->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* R0 = nullptr;
					if ((*Results)[0].IsValid() && (*Results)[0]->TryGetObject(R0) && R0 != nullptr)
					{
						bool bR0Success = false;
						(*R0)->TryGetBoolField(TEXT("success"), bR0Success);
						TestTrue(TEXT("Ping result should succeed"), bR0Success);

						FString R0Command;
						(*R0)->TryGetStringField(TEXT("command"), R0Command);
						TestEqual(TEXT("Result 0 command should be ping"), R0Command, FString(TEXT("ping")));

						double R0Index = -1.0;
						(*R0)->TryGetNumberField(TEXT("index"), R0Index);
						TestEqual(TEXT("Result 0 index should be 0"), static_cast<int32>(R0Index), 0);

						double R0Timing = -1.0;
						TestTrue(TEXT("Result 0 should have timing_ms"), (*R0)->TryGetNumberField(TEXT("timing_ms"), R0Timing));
						TestTrue(TEXT("Result 0 timing_ms should be >= 0"), R0Timing >= 0.0);
					}
				}

				// Verify result 1 (get_status)
				if (Results->Num() > 1)
				{
					const TSharedPtr<FJsonObject>* R1 = nullptr;
					if ((*Results)[1].IsValid() && (*Results)[1]->TryGetObject(R1) && R1 != nullptr)
					{
						bool bR1Success = false;
						(*R1)->TryGetBoolField(TEXT("success"), bR1Success);
						TestTrue(TEXT("get_status result should succeed"), bR1Success);

						FString R1Command;
						(*R1)->TryGetStringField(TEXT("command"), R1Command);
						TestEqual(TEXT("Result 1 command should be get_status"), R1Command, FString(TEXT("get_status")));
					}
				}
			}
			else
			{
				AddError(TEXT("Batch should have results array"));
			}
		}
	}

	// --- Test 2: Batch with one invalid command (partial success) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> Commands;

		// Command 0: ping (valid)
		TSharedRef<FJsonObject> Cmd0 = MakeShared<FJsonObject>();
		Cmd0->SetStringField(TEXT("command"), TEXT("ping"));
		Commands.Add(MakeShared<FJsonValueObject>(Cmd0));

		// Command 1: nonexistent_command (invalid)
		TSharedRef<FJsonObject> Cmd1 = MakeShared<FJsonObject>();
		Cmd1->SetStringField(TEXT("command"), TEXT("nonexistent_command"));
		Commands.Add(MakeShared<FJsonValueObject>(Cmd1));

		Params->SetArrayField(TEXT("commands"), Commands);

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestTrue(TEXT("Batch itself should succeed even with invalid sub-command"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("results"), Results) && Results != nullptr)
			{
				TestEqual(TEXT("Should have 2 results"), Results->Num(), 2);

				// Result 0 should succeed
				if (Results->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* R0 = nullptr;
					if ((*Results)[0].IsValid() && (*Results)[0]->TryGetObject(R0) && R0 != nullptr)
					{
						bool bR0Success = false;
						(*R0)->TryGetBoolField(TEXT("success"), bR0Success);
						TestTrue(TEXT("Ping should succeed"), bR0Success);
					}
				}

				// Result 1 should fail
				if (Results->Num() > 1)
				{
					const TSharedPtr<FJsonObject>* R1 = nullptr;
					if ((*Results)[1].IsValid() && (*Results)[1]->TryGetObject(R1) && R1 != nullptr)
					{
						bool bR1Success = true;
						(*R1)->TryGetBoolField(TEXT("success"), bR1Success);
						TestFalse(TEXT("Invalid command should fail"), bR1Success);

						FString ErrorCode;
						(*R1)->TryGetStringField(TEXT("error_code"), ErrorCode);
						TestEqual(TEXT("Error code should be UNKNOWN_COMMAND"), ErrorCode, FString(TEXT("UNKNOWN_COMMAND")));
					}
				}
			}
		}
	}

	// --- Test 3: Nested batch rejection ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> Commands;

		TSharedRef<FJsonObject> Cmd0 = MakeShared<FJsonObject>();
		Cmd0->SetStringField(TEXT("command"), TEXT("batch"));
		TSharedRef<FJsonObject> InnerParams = MakeShared<FJsonObject>();
		InnerParams->SetArrayField(TEXT("commands"), TArray<TSharedPtr<FJsonValue>>());
		Cmd0->SetObjectField(TEXT("params"), InnerParams);
		Commands.Add(MakeShared<FJsonValueObject>(Cmd0));

		Params->SetArrayField(TEXT("commands"), Commands);

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestTrue(TEXT("Batch with nested batch should still succeed as a batch"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("results"), Results) && Results != nullptr && Results->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* R0 = nullptr;
				if ((*Results)[0].IsValid() && (*Results)[0]->TryGetObject(R0) && R0 != nullptr)
				{
					bool bR0Success = true;
					(*R0)->TryGetBoolField(TEXT("success"), bR0Success);
					TestFalse(TEXT("Nested batch should fail"), bR0Success);

					FString ErrorCode;
					(*R0)->TryGetStringField(TEXT("error_code"), ErrorCode);
					TestEqual(TEXT("Error code should be BATCH_RECURSION_BLOCKED"), ErrorCode, FString(TEXT("BATCH_RECURSION_BLOCKED")));
				}
			}
		}
	}

	// --- Test 4: Empty batch ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("commands"), TArray<TSharedPtr<FJsonValue>>());

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestTrue(TEXT("Empty batch should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			double Count = 0.0;
			Result.Data->TryGetNumberField(TEXT("count"), Count);
			TestEqual(TEXT("Empty batch count should be 0"), static_cast<int32>(Count), 0);
		}
	}

	// --- Test 5: Batch limit exceeded ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> Commands;
		for (int32 i = 0; i < 21; ++i)
		{
			TSharedRef<FJsonObject> Cmd = MakeShared<FJsonObject>();
			Cmd->SetStringField(TEXT("command"), TEXT("ping"));
			Commands.Add(MakeShared<FJsonValueObject>(Cmd));
		}
		Params->SetArrayField(TEXT("commands"), Commands);

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestFalse(TEXT("Batch over limit should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be BATCH_LIMIT_EXCEEDED"), Result.ErrorCode, FString(TEXT("BATCH_LIMIT_EXCEEDED")));
	}

	// --- Test 6: Missing commands array ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FUDBCommandResult Result = Handler.Execute(TEXT("batch"), Params);
		TestFalse(TEXT("Batch without commands should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
	}

	return true;
}
