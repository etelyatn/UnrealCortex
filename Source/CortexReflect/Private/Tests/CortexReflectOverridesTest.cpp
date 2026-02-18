#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectOverridesBasicTest,
	"Cortex.Reflect.Overrides.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectOverridesBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_overrides"), Params);

	TestTrue(TEXT("find_overrides should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have class_name field"),
			Result.Data->HasField(TEXT("class_name")));
		TestTrue(TEXT("Should have children array"),
			Result.Data->HasField(TEXT("children")));
		TestTrue(TEXT("Should have total_overrides field"),
			Result.Data->HasField(TEXT("total_overrides")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectOverridesChildFieldsTest,
	"Cortex.Reflect.Overrides.ChildFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectOverridesChildFieldsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	Params->SetNumberField(TEXT("depth"), 1);
	Params->SetNumberField(TEXT("limit"), 5);

	FCortexCommandResult Result = Handler.Execute(TEXT("find_overrides"), Params);

	TestTrue(TEXT("find_overrides with depth/limit should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray;
		if (Result.Data->TryGetArrayField(TEXT("children"), ChildrenArray) &&
			ChildrenArray && ChildrenArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject>& FirstChild = (*ChildrenArray)[0]->AsObject();
			TestTrue(TEXT("First child should have 'name' field"),
				FirstChild->HasField(TEXT("name")));
			TestTrue(TEXT("First child should have 'overridden_functions' array"),
				FirstChild->HasField(TEXT("overridden_functions")));
			TestTrue(TEXT("First child should have 'overridden_events' array"),
				FirstChild->HasField(TEXT("overridden_events")));
			TestTrue(TEXT("First child should have 'custom_functions' array"),
				FirstChild->HasField(TEXT("custom_functions")));
			TestTrue(TEXT("First child should have 'custom_variables' array"),
				FirstChild->HasField(TEXT("custom_variables")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectOverridesDepthLimitTest,
	"Cortex.Reflect.Overrides.DepthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectOverridesDepthLimitTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	// depth=0 means ClassDepth > 0 fails for all direct children => empty result
	TSharedPtr<FJsonObject> ParamsDepth0 = MakeShared<FJsonObject>();
	ParamsDepth0->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	ParamsDepth0->SetNumberField(TEXT("depth"), 0);
	FCortexCommandResult ResultDepth0 = Handler.Execute(TEXT("find_overrides"), ParamsDepth0);
	TestTrue(TEXT("depth=0 should succeed"), ResultDepth0.bSuccess);
	if (ResultDepth0.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Children;
		if (ResultDepth0.Data->TryGetArrayField(TEXT("children"), Children))
		{
			TestEqual(TEXT("depth=0 should return no children"), Children->Num(), 0);
		}
	}

	// depth=1 should succeed and include immediate Blueprint children
	TSharedPtr<FJsonObject> ParamsDepth1 = MakeShared<FJsonObject>();
	ParamsDepth1->SetStringField(TEXT("class_name"), TEXT("ACharacter"));
	ParamsDepth1->SetNumberField(TEXT("depth"), 1);
	FCortexCommandResult ResultDepth1 = Handler.Execute(TEXT("find_overrides"), ParamsDepth1);
	TestTrue(TEXT("depth=1 should succeed"), ResultDepth1.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectOverridesClassNotFoundTest,
	"Cortex.Reflect.Overrides.ClassNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectOverridesClassNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ADoesNotExistFoo"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_overrides"), Params);

	TestFalse(TEXT("find_overrides with unknown class should fail"), Result.bSuccess);
	TestEqual(TEXT("Should be CLASS_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::ClassNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectOverridesMissingClassNameTest,
	"Cortex.Reflect.Overrides.MissingClassName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectOverridesMissingClassNameTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// class_name intentionally omitted

	FCortexCommandResult Result = Handler.Execute(TEXT("find_overrides"), Params);

	TestFalse(TEXT("find_overrides without class_name should fail"), Result.bSuccess);

	return true;
}
