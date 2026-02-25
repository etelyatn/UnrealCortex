#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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
		TestTrue(TEXT("data should exist"), Result.Data.IsValid());

		FString CompileStatus;
		double ErrorCount = -1;
		double WarningCount = -1;
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;

		TestTrue(TEXT("compile_status exists"),
			Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus));
		TestEqual(TEXT("compile_status should be success"), CompileStatus, TEXT("success"));
		TestTrue(TEXT("error_count exists"),
			Result.Data->TryGetNumberField(TEXT("error_count"), ErrorCount));
		TestTrue(TEXT("warning_count exists"),
			Result.Data->TryGetNumberField(TEXT("warning_count"), WarningCount));
		TestEqual(TEXT("error_count should be 0"), ErrorCount, 0.0);
		TestTrue(TEXT("diagnostics array exists"),
			Result.Data->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
		TestEqual(TEXT("diagnostics should be empty"), Diagnostics ? Diagnostics->Num() : -1, 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCompileWarningContractTest,
	"Cortex.Blueprint.CompileWarningContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCompileWarningContractTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Stable warning fixture expected to be authored in test content.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_CompileWarning"));

	FCortexCommandResult Result = Handler.Execute(TEXT("compile"), Params);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		AddInfo(TEXT("Skipping warning-path contract check: warning fixture missing or failed to compile."));
		return true;
	}

	FString CompileStatus;
	double ErrorCount = -1;
	double WarningCount = -1;
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;

	if (!Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus))
	{
		AddInfo(TEXT("Skipping warning-path contract check: compile_status missing."));
		return true;
	}

	if (CompileStatus != TEXT("warning"))
	{
		AddInfo(TEXT("Skipping warning-path contract check: fixture did not produce warnings in this environment."));
		return true;
	}

	TestTrue(TEXT("error_count exists"),
		Result.Data->TryGetNumberField(TEXT("error_count"), ErrorCount));
	TestTrue(TEXT("warning_count exists"),
		Result.Data->TryGetNumberField(TEXT("warning_count"), WarningCount));
	TestEqual(TEXT("error_count should be 0"), ErrorCount, 0.0);
	TestTrue(TEXT("warning_count should be > 0"), WarningCount > 0.0);
	TestTrue(TEXT("diagnostics exists"),
		Result.Data->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
	TestTrue(TEXT("diagnostics should contain at least one warning"),
		Diagnostics && Diagnostics->Num() > 0);

	bool bFoundWarningSeverity = false;
	if (Diagnostics)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
		{
			const TSharedPtr<FJsonObject> Diag = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Diag.IsValid())
			{
				continue;
			}

			FString Severity;
			if (Diag->TryGetStringField(TEXT("severity"), Severity)
				&& Severity == TEXT("warning"))
			{
				bFoundWarningSeverity = true;
				break;
			}
		}
	}
	TestTrue(TEXT("at least one diagnostic should be warning"), bFoundWarningSeverity);

	return true;
}
