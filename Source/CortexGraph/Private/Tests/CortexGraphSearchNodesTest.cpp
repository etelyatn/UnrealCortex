#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
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
	"Cortex.Graph.SearchNodes.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = CreatePackage(TEXT("/Game/Temp/CortexGraphSearchNodesTest"));
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
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

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
	UPackage* TestPackage = CreatePackage(TEXT("/Game/Temp/CortexGraphSearchNodesMacroTest"));
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
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

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

namespace
{
	UBlueprint* CreateInventoryBlueprint(const TCHAR* PackageName, const TCHAR* AssetName)
	{
		UPackage* Package = CreatePackage(PackageName);
		Package->SetPackageFlags(PKG_PlayInEditor);
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	UEdGraph* AddFunctionGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Blueprint->FunctionGraphs.Add(Graph);
		return Graph;
	}

	UK2Node_CustomEvent* AddCustomEvent(UEdGraph* Graph, const TCHAR* EventName)
	{
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = FName(EventName);
		Event->CreateNewGuid();
		Event->PostPlacedNewNode();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, false, false);
		return Event;
	}

	bool AddNodeByClass(FCortexCommandRouter& Router, const FString& AssetPath, const FString& GraphName, const TCHAR* NodeClass)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), NodeClass);
		if (!GraphName.IsEmpty())
		{
			Params->SetStringField(TEXT("graph_name"), GraphName);
		}
		return Router.Execute(TEXT("graph.add_node"), Params).bSuccess;
	}

	using FResultRows = TSharedPtr<const TArray<TSharedPtr<FJsonValue>>>;

	// Copies the row array so callers never hold a pointer into a temporary command result.
	FResultRows GetResults(const FCortexCommandResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
		if (Result.bSuccess && Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("results"), ResultsArray) && ResultsArray != nullptr)
		{
			return MakeShared<const TArray<TSharedPtr<FJsonValue>>>(*ResultsArray);
		}
		return nullptr;
	}

	FString RowString(const TSharedPtr<FJsonValue>& Row, const TCHAR* Field)
	{
		FString Value;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Row.IsValid() && Row->TryGetObject(Object) && Object != nullptr)
		{
			(*Object)->TryGetStringField(Field, Value);
		}
		return Value;
	}

	int32 CountAllNodes(const UBlueprint* Blueprint)
	{
		int32 Total = 0;
		for (const TArray<TObjectPtr<UEdGraph>>* Pages : { &Blueprint->UbergraphPages, &Blueprint->FunctionGraphs, &Blueprint->MacroGraphs, &Blueprint->DelegateSignatureGraphs })
		{
			for (const UEdGraph* Graph : *Pages)
			{
				Total += Graph->Nodes.Num();
			}
		}
		return Total;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesGraphNameScopeTest,
	"Cortex.Graph.SearchNodes.GraphNameScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesGraphNameScopeTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateInventoryBlueprint(TEXT("/Game/Temp/CortexSearchNodesScopeTest"), TEXT("BP_SearchNodesScope"));
	if (!TestNotNull(TEXT("Test Blueprint created"), TestBP))
	{
		return false;
	}

	AddFunctionGraph(TestBP, TEXT("ScopedFunction"));
	const FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

	TestTrue(TEXT("Setup IfThenElse in ScopedFunction"), AddNodeByClass(Router, AssetPath, TEXT("ScopedFunction"), TEXT("UK2Node_IfThenElse")));
	TestTrue(TEXT("Setup IfThenElse in EventGraph"), AddNodeByClass(Router, AssetPath, TEXT("EventGraph"), TEXT("UK2Node_IfThenElse")));
	TestTrue(TEXT("Setup second IfThenElse in EventGraph"), AddNodeByClass(Router, AssetPath, TEXT("EventGraph"), TEXT("UK2Node_IfThenElse")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), TEXT("ScopedFunction"));
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		const FResultRows Rows = GetResults(Router.Execute(TEXT("graph.search_nodes"), Params));
		if (TestNotNull(TEXT("Scoped results present"), Rows.Get()))
		{
			TestEqual(TEXT("graph_name restricts to the named graph"), Rows->Num(), 1);
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				TestEqual(TEXT("Row graph_name"), RowString(Row, TEXT("graph_name")), FString(TEXT("ScopedFunction")));
			}
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		const FResultRows Rows = GetResults(Router.Execute(TEXT("graph.search_nodes"), Params));
		if (TestNotNull(TEXT("Unscoped results present"), Rows.Get()))
		{
			TestEqual(TEXT("No graph_name searches every graph"), Rows->Num(), 3);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraph"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
		TestFalse(TEXT("Unknown graph_name should fail"), R.bSuccess);
		TestEqual(TEXT("Error should be GRAPH_NOT_FOUND"), R.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesNoFilterInventoryTest,
	"Cortex.Graph.SearchNodes.NoFilterInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesNoFilterInventoryTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateInventoryBlueprint(TEXT("/Game/Temp/CortexSearchNodesInventoryTest"), TEXT("BP_SearchNodesInventory"));
	if (!TestNotNull(TEXT("Test Blueprint created"), TestBP))
	{
		return false;
	}

	AddFunctionGraph(TestBP, TEXT("InventoryFunction"));
	const FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

	TestTrue(TEXT("Setup IfThenElse"), AddNodeByClass(Router, AssetPath, TEXT("InventoryFunction"), TEXT("UK2Node_IfThenElse")));
	TestTrue(TEXT("Setup Sequence"), AddNodeByClass(Router, AssetPath, TEXT("EventGraph"), TEXT("UK2Node_ExecutionSequence")));
	AddCustomEvent(TestBP->UbergraphPages[0], TEXT("InventoryEvent"));

	const int32 ExpectedTotal = CountAllNodes(TestBP);
	TestTrue(TEXT("Fixture has nodes"), ExpectedTotal >= 3);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult R = Router.Execute(TEXT("graph.search_nodes"), Params);
	TestTrue(TEXT("search_nodes without filters should succeed"), R.bSuccess);

	if (const FResultRows Rows = GetResults(R); Rows.IsValid())
	{
		TestEqual(TEXT("No filter returns every node in scope"), Rows->Num(), ExpectedTotal);
		if (Rows->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if ((*Rows)[0]->TryGetObject(Object) && Object != nullptr)
			{
				TestFalse(TEXT("Compact rows omit node_class"), (*Object)->HasField(TEXT("node_class")));
			}
		}
	}
	else
	{
		AddError(TEXT("Missing results array"));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSearchNodesSubclassMatchTest,
	"Cortex.Graph.SearchNodes.SubclassMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSearchNodesSubclassMatchTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateInventoryBlueprint(TEXT("/Game/Temp/CortexSearchNodesSubclassTest"), TEXT("BP_SearchNodesSubclass"));
	if (!TestNotNull(TEXT("Test Blueprint created"), TestBP))
	{
		return false;
	}

	const FString AssetPath = TestBP->GetPathName();
	UEdGraph* EventGraph = TestBP->UbergraphPages[0];

	int32 PosY = 0;
	FKismetEditorUtilities::AddDefaultEventNode(TestBP, EventGraph, FName(TEXT("ReceiveBeginPlay")), AActor::StaticClass(), PosY);
	AddCustomEvent(EventGraph, TEXT("SubclassEvent"));

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());

	{
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		Params->SetObjectField(TEXT("params"), NP);
		TestTrue(TEXT("Setup PrintString"), Router.Execute(TEXT("graph.add_node"), Params).bSuccess);
	}

	const int32 ExpectedTotal = CountAllNodes(TestBP);

	auto Search = [&Router, &AssetPath](const TCHAR* NodeClass)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), NodeClass);
		return Router.Execute(TEXT("graph.search_nodes"), Params);
	};

	auto Classes = [](const TArray<TSharedPtr<FJsonValue>>& Rows)
	{
		TSet<FString> Names;
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			Names.Add(RowString(Row, TEXT("class")));
		}
		return Names;
	};

	if (const FResultRows Rows = GetResults(Search(TEXT("K2Node"))); Rows.IsValid())
	{
		TestEqual(TEXT("K2Node base class matches every K2 node in scope"), Rows->Num(), ExpectedTotal);
	}
	else
	{
		AddError(TEXT("K2Node search failed"));
	}

	if (const FResultRows Rows = GetResults(Search(TEXT("K2Node_Call*"))); Rows.IsValid())
	{
		const TSet<FString> Names = Classes(*Rows);
		TestTrue(TEXT("Prefix matches K2Node_CallFunction"), Names.Contains(TEXT("K2Node_CallFunction")));
		TestFalse(TEXT("Prefix excludes K2Node_Event"), Names.Contains(TEXT("K2Node_Event")));
		TestFalse(TEXT("Prefix excludes K2Node_CustomEvent"), Names.Contains(TEXT("K2Node_CustomEvent")));
		for (const FString& Name : Names)
		{
			TestTrue(FString::Printf(TEXT("Prefix result %s starts with K2Node_Call"), *Name), Name.StartsWith(TEXT("K2Node_Call")));
		}
	}
	else
	{
		AddError(TEXT("K2Node_Call* search failed"));
	}

	// A '*' prefix is used verbatim on the runtime class name: "U*" is the prefix "U", not a stripped empty prefix
	// (which would match every node), and "UK2Node*" is not rewritten to "K2Node*".
	for (const TCHAR* Filter : { TEXT("U*"), TEXT("UK2Node*") })
	{
		if (const FResultRows Rows = GetResults(Search(Filter)); Rows.IsValid())
		{
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				TestTrue(FString::Printf(TEXT("%s only matches classes whose runtime name starts with it"), Filter),
					RowString(Row, TEXT("class")).StartsWith(FString(Filter).LeftChop(1), ESearchCase::IgnoreCase));
			}
		}
		else
		{
			AddError(FString::Printf(TEXT("%s search failed"), Filter));
		}
	}

	for (const TCHAR* Filter : { TEXT("K2Node_Event"), TEXT("uk2node_event") })
	{
		if (const FResultRows Rows = GetResults(Search(Filter)); Rows.IsValid())
		{
			const TSet<FString> Names = Classes(*Rows);
			TestTrue(FString::Printf(TEXT("%s matches the event node"), Filter), Names.Contains(TEXT("K2Node_Event")));
			TestTrue(FString::Printf(TEXT("%s matches custom events (subclass)"), Filter), Names.Contains(TEXT("K2Node_CustomEvent")));
			TestFalse(FString::Printf(TEXT("%s excludes call nodes"), Filter), Names.Contains(TEXT("K2Node_CallFunction")));
		}
		else
		{
			AddError(FString::Printf(TEXT("%s search failed"), Filter));
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
