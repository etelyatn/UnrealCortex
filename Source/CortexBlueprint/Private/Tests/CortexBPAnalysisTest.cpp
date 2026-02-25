#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeForMigrationBasicTest,
	"Cortex.Blueprint.AnalyzeForMigration.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAnalyzeForMigrationBasicTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> AnalyzeParams = MakeShared<FJsonObject>();
	AnalyzeParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_ComplexActor"));

	FCortexCommandResult AnalyzeResult = Handler.Execute(TEXT("analyze_for_migration"), AnalyzeParams);

	if (!AnalyzeResult.bSuccess)
	{
		AddInfo(TEXT("BP_ComplexActor not available in this project — skipping content-dependent assertions"));
		return true;
	}

	TestTrue(TEXT("AnalyzeForMigration should return data"), AnalyzeResult.Data.IsValid());

	if (AnalyzeResult.Data.IsValid())
	{
		// Verify all expected top-level fields exist
		const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* DynComponentsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* TimelinesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* DispatchersArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* LatentNodesArray = nullptr;
		const TSharedPtr<FJsonObject>* ComplexityObj = nullptr;

		TestTrue(TEXT("variables field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("variables"), VariablesArray));
		TestTrue(TEXT("functions field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("functions"), FunctionsArray));
		TestTrue(TEXT("scs_components field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("scs_components"), ComponentsArray));
		TestTrue(TEXT("dynamic_components field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("dynamic_components"), DynComponentsArray));
		TestTrue(TEXT("graphs field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));
		TestTrue(TEXT("timelines field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("timelines"), TimelinesArray));
		TestTrue(TEXT("event_dispatchers field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("event_dispatchers"), DispatchersArray));
		TestTrue(TEXT("interfaces_implemented field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("interfaces_implemented"), InterfacesArray));
		TestTrue(TEXT("latent_nodes field should exist"),
			AnalyzeResult.Data->TryGetArrayField(TEXT("latent_nodes"), LatentNodesArray));
		TestTrue(TEXT("complexity_metrics field should exist"),
			AnalyzeResult.Data->TryGetObjectField(TEXT("complexity_metrics"), ComplexityObj));

		// Verify metadata fields
		FString Name;
		TestTrue(TEXT("name field should exist"), AnalyzeResult.Data->TryGetStringField(TEXT("name"), Name));
		TestEqual(TEXT("name should be BP_ComplexActor"), Name, TEXT("BP_ComplexActor"));

		FString Type;
		TestTrue(TEXT("type field should exist"), AnalyzeResult.Data->TryGetStringField(TEXT("type"), Type));
		TestFalse(TEXT("type should not be Unknown"), Type == TEXT("Unknown"));

		FString ParentClass;
		TestTrue(TEXT("parent_class should exist"), AnalyzeResult.Data->TryGetStringField(TEXT("parent_class"), ParentClass));

		bool bIsCompiled = false;
		TestTrue(TEXT("is_compiled should exist"), AnalyzeResult.Data->TryGetBoolField(TEXT("is_compiled"), bIsCompiled));

		// Verify complexity_metrics fields
		if (ComplexityObj && ComplexityObj->IsValid())
		{
			double TotalNodes = 0;
			TestTrue(TEXT("total_nodes should exist"),
				(*ComplexityObj)->TryGetNumberField(TEXT("total_nodes"), TotalNodes));
			TestTrue(TEXT("total_nodes should be > 0"), TotalNodes > 0);

			FString Confidence;
			TestTrue(TEXT("migration_confidence should exist"),
				(*ComplexityObj)->TryGetStringField(TEXT("migration_confidence"), Confidence));
			TestTrue(TEXT("migration_confidence should be high/medium/low"),
				Confidence == TEXT("high") || Confidence == TEXT("medium") || Confidence == TEXT("low"));
		}

		// Verify graphs have expected sub-fields
		if (GraphsArray && GraphsArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject> GraphObj = (*GraphsArray)[0]->AsObject();
			TestTrue(TEXT("graphs[0] should have name"), GraphObj->HasField(TEXT("name")));
			TestTrue(TEXT("graphs[0] should have node_count"), GraphObj->HasField(TEXT("node_count")));
			TestTrue(TEXT("graphs[0] should have has_tick"), GraphObj->HasField(TEXT("has_tick")));
			TestTrue(TEXT("graphs[0] should have custom_event_params"), GraphObj->HasField(TEXT("custom_event_params")));
		}

		if (ComponentsArray && ComponentsArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstComp = (*ComponentsArray)[0]->AsObject();
			TestTrue(TEXT("scs_component should have delegates field"),
				FirstComp->HasField(TEXT("delegates")));
			TestTrue(TEXT("scs_component should have bound_events_in_graph field"),
				FirstComp->HasField(TEXT("bound_events_in_graph")));
		}

		if (TimelinesArray && TimelinesArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstTimeline = (*TimelinesArray)[0]->AsObject();
			TestTrue(TEXT("timeline should have replicated field"),
				FirstTimeline->HasField(TEXT("replicated")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeForMigrationSelfContainedTest,
	"Cortex.Blueprint.AnalyzeForMigration.SelfContained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAnalyzeForMigrationSelfContainedTest::RunTest(const FString& Parameters)
{
	// Create a transient Blueprint programmatically
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AnalysisTest_Transient")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!TestBP)
	{
		AddError(TEXT("Failed to create test Blueprint"));
		return false;
	}

	// Add a variable to the Blueprint
	FEdGraphPinType FloatPinType;
	FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("TestHealth"), FloatPinType, TEXT("100.0"));

	// Compile so the generated class is available
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Run the analysis via the command handler
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());

	FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);

	TestTrue(TEXT("Analysis should succeed on transient BP"), Result.bSuccess);
	TestTrue(TEXT("Data should be valid"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		// Verify name
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("Name should match"), Name, TEXT("BP_AnalysisTest_Transient"));

		// Verify type
		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("Type should be Actor"), Type, TEXT("Actor"));

		// Verify parent class
		FString ParentClass;
		Result.Data->TryGetStringField(TEXT("parent_class"), ParentClass);
		TestEqual(TEXT("Parent class should be Actor"), ParentClass, TEXT("Actor"));

		// Verify compiled
		bool bIsCompiled = false;
		Result.Data->TryGetBoolField(TEXT("is_compiled"), bIsCompiled);
		TestTrue(TEXT("Should be compiled"), bIsCompiled);

		// Verify variables include our TestHealth with new schema fields
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("variables"), Variables) && Variables && Variables->Num() > 0)
		{
			bool bFoundTestHealth = false;
			for (const TSharedPtr<FJsonValue>& VarVal : *Variables)
			{
				const TSharedPtr<FJsonObject> VarObj = VarVal->AsObject();
				if (!VarObj.IsValid())
				{
					continue;
				}
				FString VarName;
				VarObj->TryGetStringField(TEXT("name"), VarName);
				if (VarName == TEXT("TestHealth"))
				{
					bFoundTestHealth = true;

					// Verify new schema fields exist
					TestTrue(TEXT("Variable should have is_replicated field"),
						VarObj->HasField(TEXT("is_replicated")));
					TestTrue(TEXT("Variable should have container_type field"),
						VarObj->HasField(TEXT("container_type")));

					FString ContainerType;
					VarObj->TryGetStringField(TEXT("container_type"), ContainerType);
					TestEqual(TEXT("container_type should be None for scalar"), ContainerType, TEXT("None"));

					bool bReplicated = true;
					VarObj->TryGetBoolField(TEXT("is_replicated"), bReplicated);
					TestFalse(TEXT("TestHealth should not be replicated"), bReplicated);
					break;
				}
			}
			TestTrue(TEXT("Should find TestHealth variable"), bFoundTestHealth);
		}
		else
		{
			AddError(TEXT("Variables array should not be empty"));
		}

		// Verify at least one graph exists (EventGraph)
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("graphs"), Graphs))
		{
			TestTrue(TEXT("Should have at least 1 graph"), Graphs->Num() >= 1);
		}

		// Verify complexity_metrics
		const TSharedPtr<FJsonObject>* Metrics = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics) && Metrics && Metrics->IsValid())
		{
			FString Confidence;
			(*Metrics)->TryGetStringField(TEXT("migration_confidence"), Confidence);
			TestEqual(TEXT("Simple BP should have high confidence"), Confidence, TEXT("high"));
		}

		TestTrue(TEXT("dynamic_components should exist"), Result.Data->HasField(TEXT("dynamic_components")));
		TestTrue(TEXT("instanced_subobjects should exist"), Result.Data->HasField(TEXT("instanced_subobjects")));
		TestTrue(TEXT("referenced_user_types should exist"), Result.Data->HasField(TEXT("referenced_user_types")));
		TestTrue(TEXT("cdo_overrides should exist"), Result.Data->HasField(TEXT("cdo_overrides")));
		TestTrue(TEXT("entity_summary should exist"), Result.Data->HasField(TEXT("entity_summary")));

		FString ParentClassPath;
		TestTrue(TEXT("parent_class_path should exist"),
			Result.Data->TryGetStringField(TEXT("parent_class_path"), ParentClassPath));
		TestTrue(TEXT("parent_class_path should contain /Script/"),
			ParentClassPath.Contains(TEXT("/Script/")));
	}

	// Cleanup
	TestBP->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalyzeForMigrationInvalidPathTest,
	"Cortex.Blueprint.AnalyzeForMigration.InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPAnalyzeForMigrationInvalidPathTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Test with non-existent path
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_DoesNotExist"));

	FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestFalse(TEXT("Should fail for non-existent BP"), Result.bSuccess);

	// Test with missing asset_path param
	TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	FCortexCommandResult EmptyResult = Handler.Execute(TEXT("analyze_for_migration"), EmptyParams);
	TestFalse(TEXT("Should fail for missing asset_path"), EmptyResult.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV3VariableFieldsTest,
	"Cortex.Blueprint.Analysis.V3VariableFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV3VariableFieldsTest::RunTest(const FString& Parameters)
{
	// Create transient Blueprint (same pattern as SelfContainedTest)
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_V3VarFieldsTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	// Add a variable with known flags
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("TestHealth"), PinType);
	TestTrue(TEXT("Variable added"), bAdded);
	if (!bAdded || TestBP->NewVariables.Num() == 0)
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	// Explicitly make it internal/transient to verify "None" access output.
	TestBP->NewVariables[0].PropertyFlags = CPF_Transient;

	// Compile
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Run analysis via command handler (same pattern as SelfContainedTest)
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	// Check variable has new V3 fields
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	TestTrue(TEXT("Has variables"), Result.Data->TryGetArrayField(TEXT("variables"), Variables));
	if (Variables && Variables->Num() > 0)
	{
		const TSharedPtr<FJsonObject>& VarObj = (*Variables)[0]->AsObject();
		TestTrue(TEXT("Has uproperty_specifier"), VarObj->HasField(TEXT("uproperty_specifier")));
		TestTrue(TEXT("Has blueprint_access"), VarObj->HasField(TEXT("blueprint_access")));
		TestTrue(TEXT("Has reference_type"), VarObj->HasField(TEXT("reference_type")));
		TestTrue(TEXT("Has replication object"), VarObj->HasField(TEXT("replication")));
		TestTrue(TEXT("Has is_save_game"), VarObj->HasField(TEXT("is_save_game")));
		TestTrue(TEXT("Has is_transient"), VarObj->HasField(TEXT("is_transient")));
		TestTrue(TEXT("Has is_gameplay_tag"), VarObj->HasField(TEXT("is_gameplay_tag")));
		TestTrue(TEXT("Has is_instanced"), VarObj->HasField(TEXT("is_instanced")));
		TestEqual(TEXT("uproperty_specifier should be None for internal var"),
			VarObj->GetStringField(TEXT("uproperty_specifier")), FString(TEXT("None")));
		TestEqual(TEXT("blueprint_access should be None for internal var"),
			VarObj->GetStringField(TEXT("blueprint_access")), FString(TEXT("None")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexBPAnalysisV3FunctionFieldsTest,
    "Cortex.Blueprint.Analysis.V3FunctionFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV3FunctionFieldsTest::RunTest(const FString& Parameters)
{
    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(TEXT("BP_V3FuncFieldsTest")),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("Test Blueprint created"), TestBP);
    if (!TestBP) { return false; }

    // Add a function graph with a unique name to avoid duplicate function conflicts
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        TestBP, FName(TEXT("V3UniqueFunc")), UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(TestBP, FuncGraph, true, static_cast<UClass*>(nullptr));
    FKismetEditorUtilities::CompileBlueprint(TestBP);

    FCortexBPCommandHandler Handler;
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
    FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
    TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* Functions;
    TestTrue(TEXT("Has functions"), Result.Data->TryGetArrayField(TEXT("functions"), Functions));
    if (Functions && Functions->Num() > 0)
    {
        const TSharedPtr<FJsonObject>& FuncObj = (*Functions)[0]->AsObject();
        TestTrue(TEXT("Has is_override"), FuncObj->HasField(TEXT("is_override")));
        TestTrue(TEXT("Has rpc_type"), FuncObj->HasField(TEXT("rpc_type")));
        TestTrue(TEXT("Has access"), FuncObj->HasField(TEXT("access")));
        TestTrue(TEXT("Has local_variables"), FuncObj->HasField(TEXT("local_variables")));
    }

    TestBP->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexBPAnalysisV3InputBindingsTest,
    "Cortex.Blueprint.Analysis.V3InputBindings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV3InputBindingsTest::RunTest(const FString& Parameters)
{
    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(TEXT("BP_V3InputTest")),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("Test Blueprint created"), TestBP);
    if (!TestBP) { return false; }

    FKismetEditorUtilities::CompileBlueprint(TestBP);

    FCortexBPCommandHandler Handler;
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
    FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
    TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);
    // Should have the field even if empty
    TestTrue(TEXT("Has input_bindings"), Result.Data->HasField(TEXT("input_bindings")));

    TestBP->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexBPAnalysisV3ConstructionScriptTest,
    "Cortex.Blueprint.Analysis.V3ConstructionScript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV3ConstructionScriptTest::RunTest(const FString& Parameters)
{
    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(TEXT("BP_V3ConstructionTest")),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("Test Blueprint created"), TestBP);
    if (!TestBP) { return false; }

    FKismetEditorUtilities::CompileBlueprint(TestBP);

    FCortexBPCommandHandler Handler;
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
    FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
    TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);
    TestTrue(TEXT("Has construction_script"), Result.Data->HasField(TEXT("construction_script")));

    TestBP->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexBPAnalysisV3ConfidenceTest,
    "Cortex.Blueprint.Analysis.V3Confidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV3ConfidenceTest::RunTest(const FString& Parameters)
{
    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(TEXT("BP_V3ConfidenceTest")),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("Test Blueprint created"), TestBP);
    if (!TestBP) { return false; }

    FKismetEditorUtilities::CompileBlueprint(TestBP);

    FCortexBPCommandHandler Handler;
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
    FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
    TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);

    const TSharedPtr<FJsonObject>* Metrics;
    TestTrue(TEXT("Has metrics"), Result.Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics));
    if (Metrics)
    {
        TestTrue(TEXT("Has macro_instance_count"), (*Metrics)->HasField(TEXT("macro_instance_count")));
        TestTrue(TEXT("Has parent_is_blueprint"), (*Metrics)->HasField(TEXT("parent_is_blueprint")));
        TestTrue(TEXT("Has unsupported_node_count"), (*Metrics)->HasField(TEXT("unsupported_node_count")));
        TestTrue(TEXT("Has user_defined_type_count"), (*Metrics)->HasField(TEXT("user_defined_type_count")));
        TestTrue(TEXT("Has interface_count"), (*Metrics)->HasField(TEXT("interface_count")));
    }

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4TopLevelSchemaTest,
	"Cortex.Blueprint.Analysis.V4TopLevelSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4TopLevelSchemaTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_V4SchemaTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test BP created"), TestBP);
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	TestTrue(TEXT("Has scs_components"), Result.Data->HasField(TEXT("scs_components")));
	TestTrue(TEXT("Has dynamic_components"), Result.Data->HasField(TEXT("dynamic_components")));
	TestTrue(TEXT("Has widgets"), Result.Data->HasField(TEXT("widgets")));
	TestTrue(TEXT("Has widget_animations"), Result.Data->HasField(TEXT("widget_animations")));
	TestTrue(TEXT("Has named_slots"), Result.Data->HasField(TEXT("named_slots")));
	TestTrue(TEXT("Has instanced_subobjects"), Result.Data->HasField(TEXT("instanced_subobjects")));
	TestTrue(TEXT("Has entity_summary"), Result.Data->HasField(TEXT("entity_summary")));
	TestTrue(TEXT("Has referenced_user_types"), Result.Data->HasField(TEXT("referenced_user_types")));
	TestTrue(TEXT("Has cdo_overrides"), Result.Data->HasField(TEXT("cdo_overrides")));
	TestFalse(TEXT("Legacy components field removed"), Result.Data->HasField(TEXT("components")));
	TestTrue(TEXT("Has widget_bindings"), Result.Data->HasField(TEXT("widget_bindings")));
	TestTrue(TEXT("Has widget_dependencies"), Result.Data->HasField(TEXT("widget_dependencies")));

	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	TestTrue(TEXT("Has complexity_metrics"),
		Result.Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics));
	if (Metrics && Metrics->IsValid())
	{
		TestTrue(TEXT("Has graph_logic_node_count"), (*Metrics)->HasField(TEXT("graph_logic_node_count")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4BoundDelegatesOnlyTest,
	"Cortex.Blueprint.Analysis.V4BoundDelegatesOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4BoundDelegatesOnlyTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_ComplexActor"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	if (!Result.bSuccess)
	{
		AddInfo(TEXT("BP_ComplexActor missing; skip content-dependent assertions"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Scs = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("scs_components"), Scs) && Scs && Scs->Num() > 0)
	{
		const TSharedPtr<FJsonObject> First = (*Scs)[0]->AsObject();
		TestTrue(TEXT("scs component has bound_events_in_graph"), First->HasField(TEXT("bound_events_in_graph")));
		if (First->HasField(TEXT("delegates")) && First->HasField(TEXT("bound_events_in_graph")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Delegates = First->GetArrayField(TEXT("delegates"));
			const TArray<TSharedPtr<FJsonValue>>& Bound = First->GetArrayField(TEXT("bound_events_in_graph"));
			TestTrue(TEXT("delegate list should not exceed bound event list"), Delegates.Num() <= Bound.Num());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4SupplementalSectionsTest,
	"Cortex.Blueprint.Analysis.V4SupplementalSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4SupplementalSectionsTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_V4Supplemental")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	TestTrue(TEXT("Has referenced_user_types"), Result.Data->HasField(TEXT("referenced_user_types")));
	TestTrue(TEXT("Has cdo_overrides"), Result.Data->HasField(TEXT("cdo_overrides")));
	TestTrue(TEXT("Has instanced_subobjects"), Result.Data->HasField(TEXT("instanced_subobjects")));

	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("functions"), Functions) && Functions && Functions->Num() > 0)
	{
		const TSharedPtr<FJsonObject> Fn = (*Functions)[0]->AsObject();
		TestTrue(TEXT("Function has access"), Fn->HasField(TEXT("access")));
		TestTrue(TEXT("Function has local_variables"), Fn->HasField(TEXT("local_variables")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisWidgetTypeDetectionTest,
	"Cortex.Blueprint.Analysis.V4WidgetTypeDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisWidgetTypeDetectionTest::RunTest(const FString& Parameters)
{
	UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
	UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (!UserWidgetClass || !WidgetBlueprintClass)
	{
		AddInfo(TEXT("UMG module not available - skipping Widget type detection test"));
		return true;
	}

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UserWidgetClass,
		GetTransientPackage(),
		FName(TEXT("WBP_V4TypeTest")),
		BPTYPE_Normal,
		WidgetBlueprintClass,
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Widget Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestTrue(TEXT("Analysis should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("Type should be WidgetBlueprint"), Type, TEXT("WidgetBlueprint"));
		TestTrue(TEXT("widgets should exist"), Result.Data->HasField(TEXT("widgets")));
		TestTrue(TEXT("widget_animations should exist"), Result.Data->HasField(TEXT("widget_animations")));
		TestTrue(TEXT("named_slots should exist"), Result.Data->HasField(TEXT("named_slots")));
		TestTrue(TEXT("widget_bindings should exist"), Result.Data->HasField(TEXT("widget_bindings")));
		TestTrue(TEXT("widget_dependencies should exist"), Result.Data->HasField(TEXT("widget_dependencies")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4WidgetEntitiesTest,
	"Cortex.Blueprint.Analysis.V4WidgetEntities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4WidgetEntitiesTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Inventory"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	if (!Result.bSuccess)
	{
		AddInfo(TEXT("WBP_Inventory missing; skip content-dependent assertions"));
		return true;
	}

	FString Type;
	Result.Data->TryGetStringField(TEXT("type"), Type);
	TestEqual(TEXT("type is WidgetBlueprint"), Type, TEXT("WidgetBlueprint"));

	const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* NamedSlots = nullptr;
	TestTrue(TEXT("Has widgets"), Result.Data->TryGetArrayField(TEXT("widgets"), Widgets));
	TestTrue(TEXT("Has named_slots"), Result.Data->TryGetArrayField(TEXT("named_slots"), NamedSlots));

	if (Widgets)
	{
		for (const TSharedPtr<FJsonValue>& WidgetVal : *Widgets)
		{
			const TSharedPtr<FJsonObject> WidgetObj = WidgetVal->AsObject();
			if (!WidgetObj.IsValid()) { continue; }
			const bool bIsVariable = WidgetObj->GetBoolField(TEXT("is_variable"));
			const int32 BoundCount = WidgetObj->GetArrayField(TEXT("bound_events_in_graph")).Num();
			TestTrue(TEXT("Widget passes filter"), bIsVariable || BoundCount > 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4WidgetMetadataTest,
	"Cortex.Blueprint.Analysis.V4WidgetMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4WidgetMetadataTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Inventory"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	if (!Result.bSuccess)
	{
		AddInfo(TEXT("WBP_Inventory missing; skip content-dependent assertions"));
		return true;
	}

	TestTrue(TEXT("Has widget_animations"), Result.Data->HasField(TEXT("widget_animations")));
	TestTrue(TEXT("Has widget_bindings"), Result.Data->HasField(TEXT("widget_bindings")));
	TestTrue(TEXT("Has widget_dependencies"), Result.Data->HasField(TEXT("widget_dependencies")));

	const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("widgets"), Widgets) && Widgets)
	{
		for (const TSharedPtr<FJsonValue>& WidgetVal : *Widgets)
		{
			const TSharedPtr<FJsonObject> WidgetObj = WidgetVal->AsObject();
			if (!WidgetObj.IsValid()) { continue; }
			if (WidgetObj->HasField(TEXT("list_view_type")))
			{
				TestTrue(TEXT("ListView has entry_widget_class"), WidgetObj->HasField(TEXT("entry_widget_class")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisV4EntitySummaryTest,
	"Cortex.Blueprint.Analysis.V4EntitySummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisV4EntitySummaryTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_V4EntitySummary")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestBP) { return false; }

	FKismetEditorUtilities::CompileBlueprint(TestBP);

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
	TestTrue(TEXT("Analysis succeeded"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Summary = nullptr;
	TestTrue(TEXT("entity_summary exists"), Result.Data->TryGetArrayField(TEXT("entity_summary"), Summary));
	if (Summary && Summary->Num() > 0)
	{
		const TSharedPtr<FJsonObject> Entry = (*Summary)[0]->AsObject();
		TestTrue(TEXT("Summary has name"), Entry->HasField(TEXT("name")));
		TestTrue(TEXT("Summary has entity_type"), Entry->HasField(TEXT("entity_type")));
	}

	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	TestTrue(TEXT("metrics exists"), Result.Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics));
	if (Metrics && Metrics->IsValid())
	{
		double LogicCount = -1;
		TestTrue(TEXT("graph_logic_node_count numeric"), (*Metrics)->TryGetNumberField(TEXT("graph_logic_node_count"), LogicCount));
		TestTrue(TEXT("graph_logic_node_count non-negative"), LogicCount >= 0);
	}

	TestBP->MarkAsGarbage();
	return true;
}
