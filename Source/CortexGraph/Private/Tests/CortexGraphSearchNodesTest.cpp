#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "K2Node_IfThenElse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesTest,
	"Cortex.Graph.SearchNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphSearchNodesTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_SearchNodesTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	const FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"), MakeShared<FCortexGraphCommandHandler>());

	for (int32 i = 0; i < 2; ++i)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NP);
		const FCortexCommandResult AddR = Router.Execute(TEXT("graph.add_node"), Params);
		if (!TestTrue(FString::Printf(TEXT("Setup add_node PrintString[%d] should succeed"), i), AddR.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		const FCortexCommandResult AddR = Router.Execute(TEXT("graph.add_node"), Params);
		if (!TestTrue(TEXT("Setup add_node IfThenElse should succeed"), AddR.bSuccess))
		{
			TestBP->MarkAsGarbage();
			return false;
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes by function_name should succeed"), R.bSuccess);

		if (R.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			R.Data->TryGetArrayField(TEXT("results"), ResultsArray);
			TestNotNull(TEXT("Should have results array"), ResultsArray);
			if (ResultsArray)
			{
				TestEqual(TEXT("Should find 2 PrintString nodes"), ResultsArray->Num(), 2);
			}
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes by node_class should succeed"), R.bSuccess);

		if (R.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			R.Data->TryGetArrayField(TEXT("results"), ResultsArray);
			if (ResultsArray)
			{
				TestEqual(TEXT("Should find 1 Branch node"), ResultsArray->Num(), 1);
			}
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("display_name"), TEXT("Print"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes by display_name should succeed"), R.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestFalse(TEXT("search_nodes with no filter should fail"), R.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), R.ErrorCode, CortexErrorCodes::InvalidField);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("function_name"), TEXT("DefinitelyNotARealFunction"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestTrue(TEXT("search_nodes no-match should succeed"), R.bSuccess);

		if (R.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
			R.Data->TryGetArrayField(TEXT("results"), ResultsArray);
			TestNotNull(TEXT("Should have results array"), ResultsArray);
			if (ResultsArray)
			{
				TestEqual(TEXT("Should return empty results"), ResultsArray->Num(), 0);
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesMacroGraphTest,
	"Cortex.Graph.SearchNodes.MacroGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesMacroGraphTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphSearchNodesMacroTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_SearchNodesMacroTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	// Create a macro graph and add a node directly to it
	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		TestBP,
		FName(TEXT("TestMacro")),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	TestNotNull(TEXT("Macro graph created"), MacroGraph);
	if (!MacroGraph)
	{
		TestBP->MarkAsGarbage();
		return false;
	}
	TestBP->MacroGraphs.Add(MacroGraph);

	UEdGraphNode* MacroNode = NewObject<UEdGraphNode>(MacroGraph, UK2Node_IfThenElse::StaticClass());
	MacroNode->CreateNewGuid();
	MacroGraph->AddNode(MacroNode, true, false);
	MacroNode->AllocateDefaultPins();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"), MakeShared<FCortexGraphCommandHandler>());

	const FString AssetPath = TestBP->GetPathName();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
	const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
	TestTrue(TEXT("search_nodes should succeed"), R.bSuccess);

	if (R.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
		R.Data->TryGetArrayField(TEXT("results"), ResultsArray);
		TestNotNull(TEXT("Should have results array"), ResultsArray);
		if (ResultsArray)
		{
			TestEqual(TEXT("Should find node in macro graph"), ResultsArray->Num(), 1);
			if (ResultsArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				(*ResultsArray)[0]->TryGetObject(Entry);
				if (Entry)
				{
					FString GraphName;
					(*Entry)->TryGetStringField(TEXT("graph_name"), GraphName);
					TestEqual(TEXT("graph_name should be the macro graph"), GraphName, FString(TEXT("TestMacro")));
				}
			}
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
