#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Guid.h"
#include "Engine/Blueprint.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "CortexBPList119TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListTest,
	"Cortex.Blueprint.List.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListTest::RunTest(const FString& Parameters)
{
	// Setup - Create test Blueprint first
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestList_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_List_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), BlueprintName);
	CreateParams->SetStringField(TEXT("path"), BlueprintDir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	TestTrue(TEXT("Blueprint creation should succeed"), CreateResult.bSuccess);

	// Execute list command
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	FCortexCommandResult ListResult = Handler.Execute(TEXT("list"), ListParams);

	// Verify
	TestTrue(TEXT("List command should succeed"), ListResult.bSuccess);
	TestTrue(TEXT("Result data should be valid"), ListResult.Data.IsValid());

	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
		if (ListResult.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray))
		{
			TestTrue(TEXT("Blueprints array should not be empty"), BlueprintsArray->Num() > 0);

			// Find our test Blueprint
			bool bFoundTestBP = false;
			for (const TSharedPtr<FJsonValue>& BPValue : *BlueprintsArray)
			{
				const TSharedPtr<FJsonObject>& BPObj = BPValue->AsObject();
				FString AssetPath;
				if (BPObj->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					if (AssetPath.StartsWith(BlueprintDir) && AssetPath.Contains(BlueprintName))
					{
						bFoundTestBP = true;

						// Verify required fields
						FString Name;
						BPObj->TryGetStringField(TEXT("name"), Name);
						TestEqual(TEXT("Name should match"), Name, BlueprintName);

						FString Type;
						BPObj->TryGetStringField(TEXT("type"), Type);
						TestEqual(TEXT("Type should be Actor"), Type, TEXT("Actor"));

						// BP-002a: parent_class should be present in list
						FString ParentClass;
						BPObj->TryGetStringField(TEXT("parent_class"), ParentClass);
						TestEqual(TEXT("parent_class should be Actor"), ParentClass, TEXT("Actor"));

						break;
					}
				}
			}

			TestTrue(TEXT("Should find test Blueprint in list"), bFoundTestBP);
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListFilterPathTest,
	"Cortex.Blueprint.List.FilterPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListFilterPathTest::RunTest(const FString& Parameters)
{
	// Setup - Create test Blueprint in specific path
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestListFilter_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_ListFilter_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), BlueprintName);
	CreateParams->SetStringField(TEXT("path"), BlueprintDir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	TestTrue(TEXT("Blueprint creation should succeed"), CreateResult.bSuccess);

	// Execute list with path filter
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("path"), BlueprintDir);
	FCortexCommandResult ListResult = Handler.Execute(TEXT("list"), ListParams);

	// Verify
	TestTrue(TEXT("List command should succeed"), ListResult.bSuccess);

	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
		if (ListResult.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray))
		{
			// All returned Blueprints should be in the filter path
			for (const TSharedPtr<FJsonValue>& BPValue : *BlueprintsArray)
			{
				const TSharedPtr<FJsonObject>& BPObj = BPValue->AsObject();
				FString AssetPath;
				BPObj->TryGetStringField(TEXT("asset_path"), AssetPath);
				TestTrue(TEXT("Asset path should match filter"), AssetPath.StartsWith(BlueprintDir));
			}
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoTest,
	"Cortex.Blueprint.GetInfo.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetInfoTest::RunTest(const FString& Parameters)
{
	// Use pre-built BP_ComplexActor (has variables, functions, graphs)
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_ComplexActor"));
	FCortexCommandResult InfoResult = Handler.Execute(TEXT("get_info"), InfoParams);

	// Verify
	TestTrue(TEXT("GetInfo command should succeed"), InfoResult.bSuccess);
	TestTrue(TEXT("Result data should be valid"), InfoResult.Data.IsValid());

	if (InfoResult.Data.IsValid())
	{
		// Verify basic fields
		FString Name;
		InfoResult.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("Name should match"), Name, TEXT("BP_ComplexActor"));

		FString AssetPath;
		InfoResult.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("Asset path should match"), AssetPath, TEXT("/Game/Blueprints/BP_ComplexActor"));

		FString Type;
		InfoResult.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("Type should be Actor"), Type, TEXT("Actor"));

		FString ParentClass;
		InfoResult.Data->TryGetStringField(TEXT("parent_class"), ParentClass);
		TestEqual(TEXT("Parent class should be Actor"), ParentClass, TEXT("Actor"));

		// Verify variables array (BP_ComplexActor has Health, DisplayName, bIsActive)
		const TArray<TSharedPtr<FJsonValue>>* VariablesArray;
		if (TestTrue(TEXT("Variables array should exist"), InfoResult.Data->TryGetArrayField(TEXT("variables"), VariablesArray)))
		{
			TestTrue(TEXT("Should have at least 3 variables"), VariablesArray->Num() >= 3);

			// BP-002b: Verify friendly type names and category field
			for (const TSharedPtr<FJsonValue>& VarVal : *VariablesArray)
			{
				const TSharedPtr<FJsonObject>& VarObj = VarVal->AsObject();
				if (VarObj.IsValid())
				{
					FString VarName, VarType;
					VarObj->TryGetStringField(TEXT("name"), VarName);
					VarObj->TryGetStringField(TEXT("type"), VarType);

					// Type should be user-friendly ("float", "bool") not internal ("real", "boolean")
					TestFalse(TEXT("Variable type should not be 'real'"), VarType == TEXT("real"));
					TestFalse(TEXT("Variable type should not be 'boolean'"), VarType == TEXT("boolean"));
				}
			}
		}

		// Verify functions array (BP_ComplexActor has CalculateDamage)
		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray;
		if (TestTrue(TEXT("Functions array should exist"), InfoResult.Data->TryGetArrayField(TEXT("functions"), FunctionsArray)))
		{
			TestTrue(TEXT("Should have at least 1 function"), FunctionsArray->Num() >= 1);
		}

		// Verify graphs array
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray;
		if (TestTrue(TEXT("Graphs array should exist"), InfoResult.Data->TryGetArrayField(TEXT("graphs"), GraphsArray)))
		{
			TestTrue(TEXT("Should have at least one graph"), GraphsArray->Num() > 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoMissingAssetTest,
	"Cortex.Blueprint.GetInfo.MissingAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetInfoMissingAssetTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_DoesNotExist"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should return BLUEPRINT_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPInfoInheritedFunctionsTest,
	"Cortex.Blueprint.Info.InheritedCppFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPInfoInheritedFunctionsTest::RunTest(const FString& Parameters)
{
	// BP_ComplexActor inherits from AActor, which has many BlueprintCallable C++ functions
	const FString TestBPPath = TEXT("/Game/Blueprints/BP_ComplexActor");

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBPPath);
	Params->SetBoolField(TEXT("include_inherited"), true);
	Params->SetBoolField(TEXT("compact"), false);

	FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
	TestTrue(TEXT("get_info should succeed"), Result.bSuccess);

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result data is null"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* FunctionsArray;
	TestTrue(TEXT("functions array should exist"), Result.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));

	if (FunctionsArray)
	{
		bool bFoundInherited = false;
		for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
		{
			const TSharedPtr<FJsonObject>& FuncObj = FuncVal->AsObject();
			if (!FuncObj.IsValid())
			{
				continue;
			}
			FString Source;
			if (FuncObj->TryGetStringField(TEXT("source"), Source) && Source == TEXT("inherited"))
			{
				bFoundInherited = true;
				break;
			}
		}
		TestTrue(TEXT("At least one inherited function should appear with include_inherited=true"), bFoundInherited);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListFilterTypeTest,
	"Cortex.Blueprint.List.FilterType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListFilterTypeTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("FilterType"));
	TestTrue(TEXT("create actor BP"), CreateBlueprint(Handler, Dir, TEXT("BP_FT_Actor"), TEXT("Actor"), FString()));
	TestTrue(TEXT("create widget BP"), CreateBlueprint(Handler, Dir, TEXT("WBP_FT_Widget"), TEXT("Widget"), FString()));
	TestTrue(TEXT("create interface BP"), CreateBlueprint(Handler, Dir, TEXT("BPI_FT_Interface"), TEXT("Interface"), FString()));

	const FCortexCommandResult All = RunList(Handler, Dir, FString(), nullptr);
	TestEqual(TEXT("unfiltered list returns all three"), RowNames(All).Num(), 3);

	const FCortexCommandResult Widgets = RunList(Handler, Dir, TEXT("widget"), nullptr);
	TestTrue(TEXT("type=widget succeeds (case-insensitive)"), Widgets.bSuccess);
	TestEqual(TEXT("type=widget returns only the widget"), RowNames(Widgets), TArray<FString>{TEXT("WBP_FT_Widget")});
	double Count = -1.0;
	if (Widgets.Data.IsValid())
	{
		Widgets.Data->TryGetNumberField(TEXT("count"), Count);
	}
	TestEqual(TEXT("count equals returned rows"), static_cast<int32>(Count), 1);

	const FCortexCommandResult Actors = RunList(Handler, Dir, TEXT("Actor"), nullptr);
	TestEqual(TEXT("type=Actor returns only the actor"), RowNames(Actors), TArray<FString>{TEXT("BP_FT_Actor")});

	const FCortexCommandResult Interfaces = RunList(Handler, Dir, TEXT("INTERFACE"), nullptr);
	TestEqual(TEXT("type=INTERFACE returns only the interface"), RowNames(Interfaces), TArray<FString>{TEXT("BPI_FT_Interface")});

	const FCortexCommandResult Bogus = RunList(Handler, Dir, TEXT("Bogus"), nullptr);
	TestFalse(TEXT("unknown type fails"), Bogus.bSuccess);
	TestEqual(TEXT("unknown type error code"), Bogus.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("error lists the vocabulary"),
		Bogus.ErrorMessage.Contains(TEXT("Interface")) && Bogus.ErrorMessage.Contains(TEXT("EditorUtility"))
		&& Bogus.ErrorMessage.Contains(TEXT("MacroLibrary")) && Bogus.ErrorMessage.Contains(TEXT("Unknown")));

	DeleteBlueprint(Handler, Dir, TEXT("BPI_FT_Interface"));
	DeleteBlueprint(Handler, Dir, TEXT("WBP_FT_Widget"));
	DeleteBlueprint(Handler, Dir, TEXT("BP_FT_Actor"));
	RemoveTestDirectory(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListExcludeTypesTest,
	"Cortex.Blueprint.List.ExcludeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListExcludeTypesTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("ExcludeTypes"));
	TestTrue(TEXT("create actor BP"), CreateBlueprint(Handler, Dir, TEXT("BP_ET_Actor"), TEXT("Actor"), FString()));
	TestTrue(TEXT("create widget BP"), CreateBlueprint(Handler, Dir, TEXT("WBP_ET_Widget"), TEXT("Widget"), FString()));
	TestTrue(TEXT("create interface BP"), CreateBlueprint(Handler, Dir, TEXT("BPI_ET_Interface"), TEXT("Interface"), FString()));

	const TArray<TSharedPtr<FJsonValue>> WidgetAndInterface = MakeStringArray({TEXT("widget"), TEXT("INTERFACE")});
	const FCortexCommandResult Excluded = RunList(Handler, Dir, FString(), &WidgetAndInterface);
	TestTrue(TEXT("exclude_types succeeds"), Excluded.bSuccess);
	TestEqual(TEXT("exclude widget+interface leaves actor"), RowNames(Excluded), TArray<FString>{TEXT("BP_ET_Actor")});

	const TArray<TSharedPtr<FJsonValue>> WidgetOnly = MakeStringArray({TEXT("Widget")});
	const FCortexCommandResult Combined = RunList(Handler, Dir, TEXT("Actor"), &WidgetOnly);
	TestEqual(TEXT("type=Actor with exclude Widget keeps only the actor (no inversion)"),
		RowNames(Combined), TArray<FString>{TEXT("BP_ET_Actor")});

	const TArray<TSharedPtr<FJsonValue>> ActorOnly = MakeStringArray({TEXT("Actor")});
	const FCortexCommandResult Contradiction = RunList(Handler, Dir, TEXT("Actor"), &ActorOnly);
	TestTrue(TEXT("type=Actor with exclude Actor succeeds"), Contradiction.bSuccess);
	TestEqual(TEXT("type=Actor with exclude Actor is empty"), RowNames(Contradiction).Num(), 0);

	const TArray<TSharedPtr<FJsonValue>> BogusExclude = MakeStringArray({TEXT("Bogus")});
	const FCortexCommandResult BadExclude = RunList(Handler, Dir, FString(), &BogusExclude);
	TestFalse(TEXT("unknown exclude type fails"), BadExclude.bSuccess);
	TestEqual(TEXT("unknown exclude type error code"), BadExclude.ErrorCode, CortexErrorCodes::InvalidField);

	TArray<TSharedPtr<FJsonValue>> NonString;
	NonString.Add(MakeShared<FJsonValueNumber>(1.0));
	const FCortexCommandResult BadElement = RunList(Handler, Dir, FString(), &NonString);
	TestFalse(TEXT("non-string exclude entry fails"), BadElement.bSuccess);
	TestEqual(TEXT("non-string exclude entry error code"), BadElement.ErrorCode, CortexErrorCodes::InvalidField);

	DeleteBlueprint(Handler, Dir, TEXT("BPI_ET_Interface"));
	DeleteBlueprint(Handler, Dir, TEXT("WBP_ET_Widget"));
	DeleteBlueprint(Handler, Dir, TEXT("BP_ET_Actor"));
	RemoveTestDirectory(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListParentClassBlueprintTest,
	"Cortex.Blueprint.List.ParentClassBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListParentClassBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPList119Test;

	FCortexBPCommandHandler Handler;
	const FString Dir = MakeUniqueDir(TEXT("ParentChain"));
	const FString ParentName = TEXT("BP_PC_Parent");
	const FString ChildName = TEXT("BP_PC_Child");
	const FString ParentGeneratedClass = FString::Printf(TEXT("%s/%s.%s_C"), *Dir, *ParentName, *ParentName);
	TestTrue(TEXT("create parent BP"), CreateBlueprint(Handler, Dir, ParentName, TEXT("Actor"), FString()));
	TestTrue(TEXT("create child BP"), CreateBlueprint(Handler, Dir, ChildName, FString(), ParentGeneratedClass));

	const FCortexCommandResult Result = RunList(Handler, Dir, FString(), nullptr);
	TestTrue(TEXT("list succeeds"), Result.bSuccess);

	const TSharedPtr<FJsonObject> Child = FindRow(Result, ChildName);
	if (TestTrue(TEXT("child row exists"), Child.IsValid()))
	{
		TestEqual(TEXT("parent_class is the parent Blueprint asset name"), RowString(Child, TEXT("parent_class")), ParentName);
		TestEqual(TEXT("native_parent_class is Actor"), RowString(Child, TEXT("native_parent_class")), TEXT("Actor"));
		TestTrue(TEXT("parent_class_path is the parent generated class path"),
			RowString(Child, TEXT("parent_class_path")).EndsWith(FString::Printf(TEXT("%s.%s_C"), *ParentName, *ParentName)));
		TestEqual(TEXT("asset_class is Blueprint"), RowString(Child, TEXT("asset_class")), TEXT("Blueprint"));
		TestEqual(TEXT("child classified by native parent"), RowString(Child, TEXT("type")), TEXT("Actor"));
	}

	const TSharedPtr<FJsonObject> Parent = FindRow(Result, ParentName);
	if (TestTrue(TEXT("parent row exists"), Parent.IsValid()))
	{
		TestEqual(TEXT("native parent row parent_class"), RowString(Parent, TEXT("parent_class")), TEXT("Actor"));
		TestEqual(TEXT("native parent row native_parent_class"), RowString(Parent, TEXT("native_parent_class")), TEXT("Actor"));
		TestEqual(TEXT("native parent row parent_class_path"), RowString(Parent, TEXT("parent_class_path")), TEXT("/Script/Engine.Actor"));
	}

	DeleteBlueprint(Handler, Dir, ChildName);
	DeleteBlueprint(Handler, Dir, ParentName);
	RemoveTestDirectory(Dir);
	return true;
}
