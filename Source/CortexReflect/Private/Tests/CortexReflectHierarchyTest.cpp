#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyBasicTest,
	"Cortex.Reflect.Hierarchy.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 1);
	Params->SetBoolField(TEXT("include_engine"), false);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);

	TestTrue(TEXT("class_hierarchy should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString Root;
		Result.Data->TryGetStringField(TEXT("root"), Root);
		TestEqual(TEXT("Root should be AActor"), Root, TEXT("AActor"));

		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray;
		TestTrue(TEXT("Should have children"),
			Result.Data->TryGetArrayField(TEXT("children"), ChildrenArray));
		TestTrue(TEXT("Should have total_classes"),
			Result.Data->HasField(TEXT("total_classes")));
		TestTrue(TEXT("Should have cpp_count"),
			Result.Data->HasField(TEXT("cpp_count")));
		TestTrue(TEXT("Should have blueprint_count"),
			Result.Data->HasField(TEXT("blueprint_count")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyDepthTest,
	"Cortex.Reflect.Hierarchy.DepthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyDepthTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	// Depth 0 = root only, no children
	TSharedPtr<FJsonObject> Params0 = MakeShared<FJsonObject>();
	Params0->SetStringField(TEXT("root"), TEXT("AActor"));
	Params0->SetNumberField(TEXT("depth"), 0);
	FCortexCommandResult Result0 = Handler.Execute(TEXT("class_hierarchy"), Params0);
	TestTrue(TEXT("Depth 0 should succeed"), Result0.bSuccess);
	if (Result0.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Children;
		Result0.Data->TryGetArrayField(TEXT("children"), Children);
		if (Children)
		{
			TestEqual(TEXT("Depth 0 should have no children"), Children->Num(), 0);
		}
	}

	// Depth 1 = direct children only, no grandchildren
	TSharedPtr<FJsonObject> Params1 = MakeShared<FJsonObject>();
	Params1->SetStringField(TEXT("root"), TEXT("APawn"));
	Params1->SetNumberField(TEXT("depth"), 1);
	Params1->SetBoolField(TEXT("include_engine"), true);
	FCortexCommandResult Result1 = Handler.Execute(TEXT("class_hierarchy"), Params1);
	TestTrue(TEXT("Depth 1 should succeed"), Result1.bSuccess);
	if (Result1.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Children;
		if (Result1.Data->TryGetArrayField(TEXT("children"), Children))
		{
			TestTrue(TEXT("APawn should have at least one child"), Children->Num() > 0);
			if (Children->Num() > 0)
			{
				const TSharedPtr<FJsonObject>& FirstChild = (*Children)[0]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* GrandChildren;
				if (FirstChild->TryGetArrayField(TEXT("children"), GrandChildren))
				{
					TestEqual(TEXT("Children at depth limit should have no children"),
						GrandChildren->Num(), 0);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyMaxResultsTest,
	"Cortex.Reflect.Hierarchy.MaxResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyMaxResultsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 5);
	Params->SetNumberField(TEXT("max_results"), 3);
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		int32 TotalClasses;
		if (Result.Data->TryGetNumberField(TEXT("total_classes"), TotalClasses))
		{
			TestTrue(TEXT("total_classes should not exceed max_results + 1 (root)"),
				TotalClasses <= 4);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyIncludeBlueprintTest,
	"Cortex.Reflect.Hierarchy.IncludeBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyIncludeBlueprintTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	TSharedPtr<FJsonObject> ParamsNoBP = MakeShared<FJsonObject>();
	ParamsNoBP->SetStringField(TEXT("root"), TEXT("AActor"));
	ParamsNoBP->SetNumberField(TEXT("depth"), 2);
	ParamsNoBP->SetBoolField(TEXT("include_blueprint"), false);
	ParamsNoBP->SetBoolField(TEXT("include_engine"), true);
	FCortexCommandResult ResultNoBP = Handler.Execute(TEXT("class_hierarchy"), ParamsNoBP);
	TestTrue(TEXT("Should succeed without BP"), ResultNoBP.bSuccess);
	if (ResultNoBP.Data.IsValid())
	{
		int32 BPCount;
		if (ResultNoBP.Data->TryGetNumberField(TEXT("blueprint_count"), BPCount))
		{
			TestEqual(TEXT("blueprint_count should be 0 when BPs excluded"), BPCount, 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyClassNotFoundTest,
	"Cortex.Reflect.Hierarchy.ClassNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyClassNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("ADoesNotExistActor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);

	TestFalse(TEXT("Should fail"), Result.bSuccess);
	TestEqual(TEXT("Should be CLASS_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::ClassNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyDefaultsTest,
	"Cortex.Reflect.Hierarchy.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyDefaultsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("Should succeed with defaults"), Result.bSuccess);

	return true;
}
