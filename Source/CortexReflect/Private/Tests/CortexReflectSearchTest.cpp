#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchBasicTest,
	"Cortex.Reflect.Search.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Character"));
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
		TestTrue(TEXT("Should have results array"),
			Result.Data->TryGetArrayField(TEXT("results"), ResultsArray));

		TestTrue(TEXT("Should have total_results field"),
			Result.Data->HasField(TEXT("total_results")));

		if (ResultsArray && ResultsArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject>& FirstEntry = (*ResultsArray)[0]->AsObject();
			TestTrue(TEXT("First result should have 'name' field"),
				FirstEntry->HasField(TEXT("name")));
			TestTrue(TEXT("First result should have 'type' field"),
				FirstEntry->HasField(TEXT("type")));
			TestTrue(TEXT("First result should have 'parent' field"),
				FirstEntry->HasField(TEXT("parent")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchTypeFilterTest,
	"Cortex.Reflect.Search.TypeFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchTypeFilterTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Character"));
	Params->SetStringField(TEXT("type_filter"), TEXT("ACharacter"));
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with type_filter should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
		TestTrue(TEXT("Should have results array"),
			Result.Data->TryGetArrayField(TEXT("results"), ResultsArray));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchLimitTest,
	"Cortex.Reflect.Search.Limit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchLimitTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Actor"));
	Params->SetBoolField(TEXT("include_engine"), true);
	Params->SetNumberField(TEXT("limit"), 2);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with limit should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		int32 TotalResults;
		if (Result.Data->TryGetNumberField(TEXT("total_results"), TotalResults))
		{
			TestTrue(TEXT("total_results should not exceed limit"),
				TotalResults <= 2);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchNoEngineTest,
	"Cortex.Reflect.Search.NoEngine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchNoEngineTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Actor"));
	// include_engine not set — defaults to false

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with no engine classes should succeed (may return 0 results)"),
		Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchMissingPatternTest,
	"Cortex.Reflect.Search.MissingPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchMissingPatternTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// pattern field intentionally omitted

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestFalse(TEXT("search without pattern should fail"), Result.bSuccess);

	return true;
}
