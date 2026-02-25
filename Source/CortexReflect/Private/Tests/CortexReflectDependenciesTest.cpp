#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDepsBasicTest,
	"Cortex.Reflect.Dependencies.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectDepsBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_dependencies"), Params);

	TestTrue(TEXT("get_dependencies should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have asset_path field"),
			Result.Data->HasField(TEXT("asset_path")));
		TestTrue(TEXT("Should have dependencies array"),
			Result.Data->HasField(TEXT("dependencies")));
		TestTrue(TEXT("Should have total field"),
			Result.Data->HasField(TEXT("total")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDepsMissingPathTest,
	"Cortex.Reflect.Dependencies.MissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectDepsMissingPathTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("get_dependencies"), Params);

	TestFalse(TEXT("get_dependencies without asset_path should fail"), Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDepsCategoryTest,
	"Cortex.Reflect.Dependencies.Category",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectDepsCategoryTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));
	Params->SetStringField(TEXT("category"), TEXT("package"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_dependencies"), Params);

	TestTrue(TEXT("get_dependencies with category should succeed"), Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDepsLimitTest,
	"Cortex.Reflect.Dependencies.Limit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectDepsLimitTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine"));
	Params->SetNumberField(TEXT("limit"), 5);

	FCortexCommandResult Result = Handler.Execute(TEXT("get_dependencies"), Params);

	TestTrue(TEXT("get_dependencies with limit should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Dependencies;
		if (Result.Data->TryGetArrayField(TEXT("dependencies"), Dependencies))
		{
			TestTrue(TEXT("Should respect limit"), Dependencies->Num() <= 5);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectDepsObjectPathTest,
	"Cortex.Reflect.Dependencies.ObjectPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectDepsObjectPathTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine.Engine"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_dependencies"), Params);

	TestTrue(TEXT("get_dependencies should handle object path suffix"), Result.bSuccess);

	return true;
}
