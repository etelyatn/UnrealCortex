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
		Result.Data->TryGetStringField(TEXT("name"), Root);
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
			TestTrue(TEXT("total_classes should not exceed max_results"),
				TotalClasses <= 3);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyChildNameFieldTest,
	"Cortex.Reflect.Hierarchy.ChildNameField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyChildNameFieldTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("APawn"));
	Params->SetNumberField(TEXT("depth"), 1);
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("class_hierarchy should succeed"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result data is invalid"));
		return true;
	}

	// Root node must have "name" field
	FString RootName;
	TestTrue(TEXT("Root should have 'name' field"),
		Result.Data->TryGetStringField(TEXT("name"), RootName));
	TestFalse(TEXT("Root 'name' should not be empty"), RootName.IsEmpty());
	TestEqual(TEXT("Root name should be APawn"), RootName, TEXT("APawn"));

	// Each child must have a non-empty "name" field
	const TArray<TSharedPtr<FJsonValue>>* Children;
	if (Result.Data->TryGetArrayField(TEXT("children"), Children))
	{
		TestTrue(TEXT("APawn should have at least one child with include_engine"),
			Children->Num() > 0);

		for (int32 i = 0; i < Children->Num(); i++)
		{
			const TSharedPtr<FJsonObject>& Child = (*Children)[i]->AsObject();
			TestTrue(FString::Printf(TEXT("Child[%d] should be a valid object"), i),
				Child.IsValid());
			if (Child.IsValid())
			{
				FString ChildName;
				TestTrue(FString::Printf(TEXT("Child[%d] should have 'name' field"), i),
					Child->TryGetStringField(TEXT("name"), ChildName));
				TestFalse(FString::Printf(TEXT("Child[%d] 'name' should not be empty"), i),
					ChildName.IsEmpty());
				TestNotEqual(FString::Printf(TEXT("Child[%d] 'name' should not be 'Unknown'"), i),
					ChildName, FString(TEXT("Unknown")));
			}
		}
	}
	else
	{
		AddError(TEXT("Result should have children array"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyCountBreakdownTest,
	"Cortex.Reflect.Hierarchy.CountBreakdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyCountBreakdownTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("root"), TEXT("AActor"));
	ParamsObj->SetNumberField(TEXT("depth"), 2);
	ParamsObj->SetNumberField(TEXT("max_results"), 500);
	ParamsObj->SetBoolField(TEXT("include_engine"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), ParamsObj);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have project_cpp_count"), Result.Data->HasField(TEXT("project_cpp_count")));
		TestTrue(TEXT("Should have engine_cpp_count"), Result.Data->HasField(TEXT("engine_cpp_count")));
		TestTrue(
			TEXT("Should have project_blueprint_count"),
			Result.Data->HasField(TEXT("project_blueprint_count"))
		);

		const int32 ProjectCpp = Result.Data->GetIntegerField(TEXT("project_cpp_count"));
		const int32 EngineCpp = Result.Data->GetIntegerField(TEXT("engine_cpp_count"));
		const int32 TotalCpp = Result.Data->GetIntegerField(TEXT("cpp_count"));

		TestEqual(TEXT("Project + Engine CPP should equal total CPP"), ProjectCpp + EngineCpp, TotalCpp);
		TestTrue(TEXT("Should have some engine classes with include_engine=true"), EngineCpp > 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyProjectOnlyCountsTest,
	"Cortex.Reflect.Hierarchy.ProjectOnlyCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyProjectOnlyCountsTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;

	TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("root"), TEXT("AActor"));
	ParamsObj->SetNumberField(TEXT("depth"), 2);
	ParamsObj->SetNumberField(TEXT("max_results"), 500);
	ParamsObj->SetBoolField(TEXT("include_engine"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), ParamsObj);
	TestTrue(TEXT("Should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const int32 EngineCpp = Result.Data->GetIntegerField(TEXT("engine_cpp_count"));
		TestEqual(TEXT("Project-only mode should report zero engine classes"), EngineCpp, 0);
	}

	return true;
}

namespace CortexReflectHierarchyFlatClassesTest
{
	/**
	 * Collects every node name of the nested hierarchy tree in visit order. The flat class list is
	 * a duplicate of exactly these names, so the tree walk is the authority for "navigable without
	 * duplicate flat classes" as well as for the explicit opt-in recovery.
	 */
	void CollectFlatClassNames(const TSharedPtr<FJsonObject>& Node, TArray<FString>& OutNames)
	{
		if (!Node.IsValid())
		{
			return;
		}

		FString Name;
		if (Node->TryGetStringField(TEXT("name"), Name))
		{
			OutNames.Add(Name);
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (!Node->TryGetArrayField(TEXT("children"), Children) || !Children)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Child : *Children)
		{
			CollectFlatClassNames(Child->AsObject(), OutNames);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyFlatClassesDefaultOffTest,
	"Cortex.Reflect.Hierarchy.FlatClassesDefaultOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyFlatClassesDefaultOffTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 1);
	Params->SetBoolField(TEXT("include_engine"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("class_hierarchy should succeed"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result data is invalid"));
		return true;
	}

	// The flat duplicate of every node name is opt-in: a default response must not repeat the
	// whole navigable tree as a second flat array.
	TestFalse(TEXT("Default class_hierarchy must not duplicate flat classes"),
		Result.Data->HasField(TEXT("classes")));

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	TestTrue(TEXT("Default hierarchy must remain navigable through children"),
		Result.Data->TryGetArrayField(TEXT("children"), Children));
	TestTrue(TEXT("Engine-inclusive AActor hierarchy must expose direct children"),
		Children != nullptr && Children->Num() > 0);

	// The aggregate counts must still describe the nested tree the caller can walk.
	int32 TotalClasses = 0;
	TestTrue(TEXT("total_classes must remain present"),
		Result.Data->TryGetNumberField(TEXT("total_classes"), TotalClasses));
	TestEqual(TEXT("total_classes must count the navigable tree (root plus direct children)"),
		TotalClasses, 1 + (Children ? Children->Num() : 0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyFlatClassesOptInTest,
	"Cortex.Reflect.Hierarchy.FlatClassesOptIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyFlatClassesOptInTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 1);
	Params->SetBoolField(TEXT("include_engine"), true);
	Params->SetBoolField(TEXT("include_flat_classes"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("class_hierarchy should succeed with include_flat_classes=true"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result data is invalid"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* FlatClasses = nullptr;
	TestTrue(TEXT("Explicit include_flat_classes=true must recover the flat class list"),
		Result.Data->TryGetArrayField(TEXT("classes"), FlatClasses));
	if (!FlatClasses)
	{
		return true;
	}

	TArray<FString> TreeNames;
	CortexReflectHierarchyFlatClassesTest::CollectFlatClassNames(Result.Data, TreeNames);
	TestTrue(TEXT("Opt-in hierarchy must still carry a navigable tree"), TreeNames.Num() > 1);
	TestEqual(TEXT("Flat classes must list every navigable tree node exactly once"),
		FlatClasses->Num(), TreeNames.Num());

	for (const FString& TreeName : TreeNames)
	{
		bool bFound = false;
		for (const TSharedPtr<FJsonValue>& Value : *FlatClasses)
		{
			FString FlatName;
			if (Value->TryGetString(FlatName) && FlatName == TreeName)
			{
				bFound = true;
				break;
			}
		}
		TestTrue(FString::Printf(TEXT("Flat classes must include tree node %s"), *TreeName), bFound);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectHierarchyBoundedTraversalSignalTest,
	"Cortex.Reflect.Hierarchy.BoundedTraversalDeclaresTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectHierarchyBoundedTraversalSignalTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 2);
	Params->SetBoolField(TEXT("include_engine"), true);
	// A node budget far below the size of the AActor hierarchy, so the walk is bounded early.
	Params->SetNumberField(TEXT("max_results"), 3);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("class_hierarchy should succeed"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result data is invalid"));
		return true;
	}

	// The node budget is spent on the root and its descendants, so the walk stops early. The
	// emitted top-level child list is shorter than the budget, and the truncation must come from
	// the traversal itself rather than from the emitted child count.
	bool bTruncated = false;
	TestTrue(TEXT("A bounded traversal must declare truncation"),
		Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));
	TestTrue(TEXT("A bounded traversal must report truncated=true"), bTruncated);

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	TestTrue(TEXT("A bounded hierarchy must still be navigable through children"),
		Result.Data->TryGetArrayField(TEXT("children"), Children));
	if (Children)
	{
		TestTrue(TEXT("Emitted children must be shorter than the node budget"),
			Children->Num() < 3);
	}

	int32 TotalClasses = 0;
	TestTrue(TEXT("total_classes must remain present"),
		Result.Data->TryGetNumberField(TEXT("total_classes"), TotalClasses));
	TestTrue(TEXT("total_classes must not exceed the node budget"), TotalClasses <= 3);

	return true;
}
