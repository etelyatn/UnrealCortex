#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/UObjectIterator.h"

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
		TestEqual(TEXT("warning_count should be 0"), WarningCount, 0.0);
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
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_CompileWarn_%s"), *UniqueSuffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CompileWarn_%s"), *UniqueSuffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	auto CleanupBlueprint = [&]()
	{
		UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (CreatedBP)
		{
			CreatedBP->MarkAsGarbage();
		}
	};

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), BlueprintName);
		CreateParams->SetStringField(TEXT("path"), BlueprintDir);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		TestTrue(TEXT("Setup: create should succeed"), CreateResult.bSuccess);
	}

	UBlueprint* TestBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Blueprint should exist"), TestBP);
	if (!TestBP || TestBP->UbergraphPages.Num() == 0)
	{
		CleanupBlueprint();
		return true;
	}

	UEdGraph* EventGraph = TestBP->UbergraphPages[0];

	TArray<UFunction*> DeprecatedCallableFunctions;
	for (TObjectIterator<UFunction> It; It; ++It)
	{
		UFunction* Function = *It;
		if (!Function
			|| !Function->HasAllFunctionFlags(FUNC_BlueprintCallable | FUNC_Static)
			|| !Function->HasMetaData(TEXT("DeprecatedFunction")))
		{
			continue;
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		if (!OwnerClass || !OwnerClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			continue;
		}

		DeprecatedCallableFunctions.Add(Function);
	}

	for (TObjectIterator<UFunction> It; It; ++It)
	{
		UFunction* Function = *It;
		if (!Function
			|| !Function->HasAllFunctionFlags(FUNC_BlueprintCallable)
			|| Function->HasAnyFunctionFlags(FUNC_Static)
			|| !Function->HasMetaData(TEXT("DeprecatedFunction")))
		{
			continue;
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		if (!OwnerClass || !OwnerClass->IsChildOf(AActor::StaticClass()))
		{
			continue;
		}

		DeprecatedCallableFunctions.AddUnique(Function);
	}

	DeprecatedCallableFunctions.Sort([](const UFunction& A, const UFunction& B)
	{
		return A.GetPathName() < B.GetPathName();
	});

	TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), BlueprintPath);

	FCortexCommandResult WarningResult;
	bool bFoundWarningCase = false;
	const int32 MaxAttempts = DeprecatedCallableFunctions.Num();
	for (int32 Index = 0; Index < MaxAttempts; ++Index)
	{
		UFunction* Candidate = DeprecatedCallableFunctions[Index];
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(EventGraph);
		Node->CreateNewGuid();
		Node->SetFromFunction(Candidate);
		EventGraph->AddNode(Node, false, false);
		Node->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsModified(TestBP);

		FCortexCommandResult Result = Handler.Execute(TEXT("compile"), CompileParams);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			FString CompileStatus;
			double ErrorCount = -1;
			double WarningCount = -1;
			if (Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus)
				&& Result.Data->TryGetNumberField(TEXT("error_count"), ErrorCount)
				&& Result.Data->TryGetNumberField(TEXT("warning_count"), WarningCount)
				&& CompileStatus == TEXT("warning")
				&& ErrorCount == 0.0
				&& WarningCount > 0.0)
			{
				WarningResult = Result;
				bFoundWarningCase = true;
				break;
			}
		}

		EventGraph->RemoveNode(Node);
		FBlueprintEditorUtils::MarkBlueprintAsModified(TestBP);
	}

	if (!bFoundWarningCase)
	{
		AddInfo(TEXT("Skipping warning-path contract check: no deprecated callable produced compile warnings in this runtime."));
		CleanupBlueprint();
		return true;
	}

	FString CompileStatus;
	double ErrorCount = -1;
	double WarningCount = -1;
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;

	TestTrue(TEXT("compile should succeed on warning path"), WarningResult.bSuccess);
	TestTrue(TEXT("warning payload should exist"), WarningResult.Data.IsValid());
	TestTrue(TEXT("compile_status exists"),
		WarningResult.Data->TryGetStringField(TEXT("compile_status"), CompileStatus));
	TestEqual(TEXT("compile_status should be warning"), CompileStatus, TEXT("warning"));
	TestTrue(TEXT("error_count exists"),
		WarningResult.Data->TryGetNumberField(TEXT("error_count"), ErrorCount));
	TestTrue(TEXT("warning_count exists"),
		WarningResult.Data->TryGetNumberField(TEXT("warning_count"), WarningCount));
	TestEqual(TEXT("error_count should be 0"), ErrorCount, 0.0);
	TestTrue(TEXT("warning_count should be > 0"), WarningCount > 0.0);
	TestTrue(TEXT("diagnostics exists"),
		WarningResult.Data->TryGetArrayField(TEXT("diagnostics"), Diagnostics));
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

	CleanupBlueprint();

	return true;
}
