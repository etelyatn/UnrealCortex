// CortexBPCompactSerializationTest.cpp
#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoCompactTest,
	"Cortex.Blueprint.CompactSerialization.GetInfoCompact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetInfoCompactTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_Compact/BP_CompactTest");

	// Setup: create a Blueprint parented to AActor.
	// AActor has many BlueprintCallable inherited functions (ReceiveBeginPlay, ReceiveTick, etc.)
	// with empty inputs/outputs — ideal for verifying compact stripping.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_CompactTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Compact"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult R = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create BP"), R.bSuccess);
		if (!R.bSuccess)
		{
			return false;
		}
	}

	// Setup: add a function with inputs so non-empty arrays are preserved in compact mode.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("MyTestFunc"));

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TSharedPtr<FJsonObject> InputParam = MakeShared<FJsonObject>();
		InputParam->SetStringField(TEXT("name"), TEXT("MyParam"));
		InputParam->SetStringField(TEXT("type"), TEXT("float"));
		Inputs.Add(MakeShared<FJsonValueObject>(InputParam));
		Params->SetArrayField(TEXT("inputs"), Inputs);

		Handler.Execute(TEXT("add_function"), Params);
		// Not asserting here — even if it fails we still test inherited function compaction
	}

	// -----------------------------------------------------------------------
	// Test 1: compact=true (default) — source absent, empty arrays stripped
	// -----------------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetBoolField(TEXT("include_inherited"), true);
		// Note: compact defaults to true, do NOT set it here

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		TestTrue(TEXT("[Compact] get_info should succeed"), Result.bSuccess);

		if (!Result.Data.IsValid())
		{
			AddError(TEXT("[Compact] Result data is null"));
			// Cleanup
			UBlueprint* TestBP = Cast<UBlueprint>(StaticFindObject(UBlueprint::StaticClass(), nullptr, *TestBPPath));
			if (TestBP)
			{
				TestBP->MarkAsGarbage();
			}
			TSharedPtr<FJsonObject> DelParams = MakeShared<FJsonObject>();
			DelParams->SetStringField(TEXT("asset_path"), TestBPPath);
			Handler.Execute(TEXT("delete"), DelParams);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
		TestTrue(TEXT("[Compact] functions array should exist"),
			Result.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));

		if (FunctionsArray)
		{
			bool bFoundAnySource = false;
			bool bFoundEmptyInputsArray = false;
			bool bFoundEmptyOutputsArray = false;
			bool bFoundNonEmptyInputs = false;

			for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
			{
				const TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
				if (!FuncObj.IsValid())
				{
					continue;
				}

				FString Source;
				if (FuncObj->TryGetStringField(TEXT("source"), Source))
				{
					bFoundAnySource = true;
				}

				const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
				if (FuncObj->TryGetArrayField(TEXT("inputs"), InputsArr))
				{
					if (InputsArr->Num() == 0)
					{
						bFoundEmptyInputsArray = true;
					}
					else
					{
						bFoundNonEmptyInputs = true;
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
				if (FuncObj->TryGetArrayField(TEXT("outputs"), OutputsArr))
				{
					if (OutputsArr->Num() == 0)
					{
						bFoundEmptyOutputsArray = true;
					}
				}
			}

			TestFalse(TEXT("[Compact] source field should be absent on all functions"), bFoundAnySource);
			TestFalse(TEXT("[Compact] empty inputs arrays should be stripped"), bFoundEmptyInputsArray);
			TestFalse(TEXT("[Compact] empty outputs arrays should be stripped"), bFoundEmptyOutputsArray);
			TestTrue(TEXT("[Compact] non-empty inputs array should be present for MyTestFunc"), bFoundNonEmptyInputs);
		}
	}

	// -----------------------------------------------------------------------
	// Test 2: compact=false — source present, empty arrays present
	// -----------------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetBoolField(TEXT("include_inherited"), true);
		Params->SetBoolField(TEXT("compact"), false);

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		TestTrue(TEXT("[Verbose] get_info should succeed"), Result.bSuccess);

		if (!Result.Data.IsValid())
		{
			AddError(TEXT("[Verbose] Result data is null"));
			// Cleanup
			UBlueprint* TestBP = Cast<UBlueprint>(StaticFindObject(UBlueprint::StaticClass(), nullptr, *TestBPPath));
			if (TestBP)
			{
				TestBP->MarkAsGarbage();
			}
			TSharedPtr<FJsonObject> DelParams = MakeShared<FJsonObject>();
			DelParams->SetStringField(TEXT("asset_path"), TestBPPath);
			Handler.Execute(TEXT("delete"), DelParams);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
		TestTrue(TEXT("[Verbose] functions array should exist"),
			Result.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));

		if (FunctionsArray)
		{
			bool bFoundSource = false;
			bool bFoundEmptyInputsArray = false;
			bool bFoundEmptyOutputsArray = false;

			for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
			{
				const TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
				if (!FuncObj.IsValid())
				{
					continue;
				}

				FString Source;
				if (FuncObj->TryGetStringField(TEXT("source"), Source))
				{
					bFoundSource = true;
				}

				const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
				if (FuncObj->TryGetArrayField(TEXT("inputs"), InputsArr))
				{
					if (InputsArr->Num() == 0)
					{
						bFoundEmptyInputsArray = true;
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
				if (FuncObj->TryGetArrayField(TEXT("outputs"), OutputsArr))
				{
					if (OutputsArr->Num() == 0)
					{
						bFoundEmptyOutputsArray = true;
					}
				}
			}

			TestTrue(TEXT("[Verbose] source field should be present on functions"), bFoundSource);
			TestTrue(TEXT("[Verbose] empty inputs arrays should be present"), bFoundEmptyInputsArray);
			TestTrue(TEXT("[Verbose] empty outputs arrays should be present"), bFoundEmptyOutputsArray);
		}
	}

	// Cleanup
	{
		UBlueprint* TestBP = Cast<UBlueprint>(StaticFindObject(UBlueprint::StaticClass(), nullptr, *TestBPPath));
		if (TestBP)
		{
			TestBP->MarkAsGarbage();
		}
		TSharedPtr<FJsonObject> DelParams = MakeShared<FJsonObject>();
		DelParams->SetStringField(TEXT("asset_path"), TestBPPath);
		Handler.Execute(TEXT("delete"), DelParams);
	}

	return true;
}
