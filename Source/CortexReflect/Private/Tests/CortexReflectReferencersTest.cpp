#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectRefsBasicTest,
	"Cortex.Reflect.Referencers.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectRefsBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_referencers"), Params);

	TestTrue(TEXT("get_referencers should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have asset_path field"),
			Result.Data->HasField(TEXT("asset_path")));
		TestTrue(TEXT("Should have referencers array"),
			Result.Data->HasField(TEXT("referencers")));
		TestTrue(TEXT("Should have total field"),
			Result.Data->HasField(TEXT("total")));

		double Total = Result.Data->GetNumberField(TEXT("total"));
		TestTrue(TEXT("/Script/Engine should have at least one referencer"), Total > 0.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectRefsMissingPathTest,
	"Cortex.Reflect.Referencers.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectRefsMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("get_referencers"), Params);

	TestFalse(TEXT("get_referencers without asset_path should fail"), Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectRefsCategoryTest,
	"Cortex.Reflect.Referencers.Category",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectRefsCategoryTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));
	Params->SetStringField(TEXT("category"), TEXT("package"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_referencers"), Params);

	TestTrue(TEXT("get_referencers with category should succeed"), Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectRefsLimitTest,
	"Cortex.Reflect.Referencers.Limit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectRefsLimitTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));
	Params->SetNumberField(TEXT("limit"), 3);

	FCortexCommandResult Result = Handler.Execute(TEXT("get_referencers"), Params);

	TestTrue(TEXT("get_referencers with limit should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Referencers;
		if (Result.Data->TryGetArrayField(TEXT("referencers"), Referencers))
		{
			TestTrue(TEXT("Should respect limit"), Referencers->Num() <= 3);
		}
		TestTrue(TEXT("Should have total_unfiltered field"),
			Result.Data->HasField(TEXT("total_unfiltered")));
	}

	return true;
}
