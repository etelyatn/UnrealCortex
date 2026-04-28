#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Knot.h"
#include "UObject/Interface.h"

namespace CortexGraphInterfaceGraphTestUtils
{
	/**
	 * Build a transient Blueprint suitable for graph-resolution tests.
	 * Caller is responsible for MarkAsGarbage() at the end of the test.
	 */
	UBlueprint* CreateTestBlueprint(const TCHAR* Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			GetTransientPackage(),
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	/**
	 * Inject a synthetic interface implementation entry into Blueprint->ImplementedInterfaces
	 * with one stub UEdGraph named GraphName. Avoids the AddInterface helper because not all
	 * UInterface classes auto-populate the Graphs array, which would make the fixture fragile.
	 *
	 * Returns the created stub graph for further mutation in tests.
	 */
	UEdGraph* InjectInterfaceGraph(UBlueprint* Blueprint, UClass* InterfaceClass, const TCHAR* GraphName)
	{
		check(Blueprint && InterfaceClass);

		UEdGraph* StubGraph = NewObject<UEdGraph>(
			Blueprint,
			UEdGraph::StaticClass(),
			FName(GraphName),
			RF_Transactional);
		StubGraph->Schema = UEdGraphSchema_K2::StaticClass();

		FBPInterfaceDescription Desc;
		Desc.Interface = InterfaceClass;
		Desc.Graphs.Add(StubGraph);
		Blueprint->ImplementedInterfaces.Add(Desc);

		return StubGraph;
	}

	/**
	 * Inject a stub macro graph onto Blueprint->MacroGraphs. Stays in pure name-resolution
	 * territory — does not attempt to construct a valid macro signature.
	 */
	UEdGraph* InjectMacroGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* MacroGraph = NewObject<UEdGraph>(
			Blueprint,
			UEdGraph::StaticClass(),
			FName(GraphName),
			RF_Transactional);
		MacroGraph->Schema = UEdGraphSchema_K2::StaticClass();
		Blueprint->MacroGraphs.Add(MacroGraph);
		return MacroGraph;
	}

	/** Build a graph_cmd parameter object with the standard asset_path + graph_name fields. */
	TSharedPtr<FJsonObject> MakeGraphParams(const FString& AssetPath, const FString& GraphName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!GraphName.IsEmpty())
		{
			Params->SetStringField(TEXT("graph_name"), GraphName);
		}
		return Params;
	}

	/** Register graph domain on a router (consistent with existing graph tests). */
	void RegisterGraphDomain(FCortexCommandRouter& Router)
	{
		Router.RegisterDomain(
			TEXT("graph"),
			TEXT("Cortex Graph"),
			TEXT("1.0.0"),
			MakeShared<FCortexGraphCommandHandler>());
	}
}

using namespace CortexGraphInterfaceGraphTestUtils;

// ============================================================================
// 1. FindGraph resolves interface-implementation graphs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphFindInterfaceImplTest,
	"Cortex.Graph.InterfaceGraph.FindGraph_ResolvesInterfaceImplGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphFindInterfaceImplTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_FindInterfaceImpl"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("Sitting_Pose"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	FCortexCommandResult Result = Router.Execute(
		TEXT("graph.list_nodes"),
		MakeGraphParams(TestBP->GetPathName(), TEXT("Sitting_Pose")));

	TestTrue(TEXT("list_nodes resolves an interface-implementation graph"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		TestTrue(TEXT("Result has nodes array"),
			Result.Data->TryGetArrayField(TEXT("nodes"), NodesArray));
		// Empty stub graph — node count is zero, but array existence proves graph was found.
	}

	TestBP->MarkAsGarbage();
	return true;
}

// ============================================================================
// 2. FindGraph resolves macro graphs (regression for SearchNodes/FindGraph drift)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphFindMacroTest,
	"Cortex.Graph.InterfaceGraph.FindGraph_ResolvesMacroGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphFindMacroTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_FindMacroGraph"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectMacroGraph(TestBP, TEXT("MyMacro"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	FCortexCommandResult Result = Router.Execute(
		TEXT("graph.list_nodes"),
		MakeGraphParams(TestBP->GetPathName(), TEXT("MyMacro")));

	TestTrue(TEXT("list_nodes resolves a macro graph"), Result.bSuccess);

	TestBP->MarkAsGarbage();
	return true;
}

// ============================================================================
// 3. list_graphs reports the new kind field for every category
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphListGraphsKindTest,
	"Cortex.Graph.InterfaceGraph.ListGraphs_ReportsKindField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphListGraphsKindTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_ListGraphsKind"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectMacroGraph(TestBP, TEXT("MyMacro"));
	InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("MyInterfaceFunc"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_graphs"), Params);
	TestTrue(TEXT("list_graphs succeeded"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
	TestTrue(TEXT("Result has graphs array"),
		Result.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));

	bool bSawUbergraph = false;
	bool bSawFunction = false;
	bool bSawMacro = false;
	bool bSawInterfaceImpl = false;

	if (GraphsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *GraphsArray)
		{
			TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (!Entry.IsValid()) { continue; }

			FString Kind;
			TestTrue(TEXT("Each entry has a kind field"),
				Entry->TryGetStringField(TEXT("kind"), Kind));

			if      (Kind == TEXT("ubergraph"))      bSawUbergraph = true;
			else if (Kind == TEXT("function"))       bSawFunction = true;
			else if (Kind == TEXT("macro"))          bSawMacro = true;
			else if (Kind == TEXT("interface_impl")) bSawInterfaceImpl = true;
		}
	}

	TestTrue(TEXT("Saw ubergraph kind"), bSawUbergraph);
	TestTrue(TEXT("Saw function kind"), bSawFunction);
	TestTrue(TEXT("Saw macro kind"), bSawMacro);
	TestTrue(TEXT("Saw interface_impl kind"), bSawInterfaceImpl);

	TestBP->MarkAsGarbage();
	return true;
}

// ============================================================================
// 4. list_graphs reports owning_interface for interface_impl entries only
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphListGraphsOwningInterfaceTest,
	"Cortex.Graph.InterfaceGraph.ListGraphs_ReportsOwningInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphListGraphsOwningInterfaceTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_ListGraphsOwningInterface"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UClass* InterfaceClass = UInterface::StaticClass();
	InjectInterfaceGraph(TestBP, InterfaceClass, TEXT("OwnedFunc"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_graphs"), Params);
	TestTrue(TEXT("list_graphs succeeded"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
	if (!Result.Data->TryGetArrayField(TEXT("graphs"), GraphsArray) || !GraphsArray)
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	bool bFoundInterfaceImpl = false;
	for (const TSharedPtr<FJsonValue>& Value : *GraphsArray)
	{
		TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!Entry.IsValid()) { continue; }

		FString Kind;
		Entry->TryGetStringField(TEXT("kind"), Kind);

		FString OwningInterface;
		const bool bHasOwning = Entry->TryGetStringField(TEXT("owning_interface"), OwningInterface);

		if (Kind == TEXT("interface_impl"))
		{
			bFoundInterfaceImpl = true;
			TestTrue(TEXT("interface_impl entry has owning_interface field"), bHasOwning);
			TestEqual(TEXT("owning_interface matches injected interface name"),
				OwningInterface, InterfaceClass->GetName());
		}
		else
		{
			TestFalse(
				FString::Printf(TEXT("non-interface_impl entry must NOT have owning_interface (kind=%s)"), *Kind),
				bHasOwning);
		}
	}

	TestTrue(TEXT("Saw at least one interface_impl entry"), bFoundInterfaceImpl);

	TestBP->MarkAsGarbage();
	return true;
}

// ============================================================================
// 5. FindGraph first-match-wins on name collision (locks in current behavior so
//    any future interface=... disambiguator is a strict refinement, not a break)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphFirstMatchWinsTest,
	"Cortex.Graph.InterfaceGraph.FindGraph_FirstMatchWinsOnNameCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphFirstMatchWinsTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_FirstMatchWins"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	// Two interface impl entries with graphs that share the SAME GetName() but live under
	// DIFFERENT outers, so they are distinct UObjects. NewObject would otherwise refuse to
	// create a second same-named subobject under the same outer — putting one under TestBP
	// and one under the transient package gives us two genuinely separate FCortexGraphEntries
	// that collide on Graph->GetName().
	UEdGraph* FirstGraph  = NewObject<UEdGraph>(TestBP,                UEdGraph::StaticClass(), TEXT("DupName"), RF_Transactional);
	UEdGraph* SecondGraph = NewObject<UEdGraph>(GetTransientPackage(), UEdGraph::StaticClass(), TEXT("DupName"), RF_Transactional);
	TestNotNull(TEXT("First graph created"), FirstGraph);
	TestNotNull(TEXT("Second graph created"), SecondGraph);
	TestNotEqual(TEXT("Two distinct UEdGraph objects (different outers, same FName)"), FirstGraph, SecondGraph);
	TestEqual(TEXT("Both graphs share GetName() = 'DupName'"),
		FirstGraph->GetName(), SecondGraph->GetName());

	FirstGraph->Schema  = UEdGraphSchema_K2::StaticClass();
	SecondGraph->Schema = UEdGraphSchema_K2::StaticClass();

	FBPInterfaceDescription FirstDesc;
	FirstDesc.Interface = UInterface::StaticClass();
	FirstDesc.Graphs.Add(FirstGraph);
	TestBP->ImplementedInterfaces.Add(FirstDesc);

	FBPInterfaceDescription SecondDesc;
	SecondDesc.Interface = UInterface::StaticClass();
	SecondDesc.Graphs.Add(SecondGraph);
	TestBP->ImplementedInterfaces.Add(SecondDesc);

	FCortexCommandResult LookupErr;
	UEdGraph* Resolved = FCortexGraphNodeOps::FindGraph(TestBP, TEXT("DupName"), LookupErr);
	TestEqual(TEXT("FindGraph returns the FIRST inserted match (ImplementedInterfaces[0])"),
		Resolved, FirstGraph);

	TestBP->MarkAsGarbage();
	return true;
}

// ============================================================================
// 6. SearchNodes reach now matches FindGraph reach (regression coverage for the
//    drift between UbergraphPages+FunctionGraphs+MacroGraphs vs FindGraph's
//    UbergraphPages+FunctionGraphs only). Asserts SearchNodes can find a node
//    placed in an interface-implementation graph.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphSearchNodesReachTest,
	"Cortex.Graph.InterfaceGraph.SearchNodes_ReachesInterfaceImplGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphSearchNodesReachTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("BP_SearchNodesReach"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("FuncWithKnot"));

	// Drop a Knot (reroute) node into the interface graph. Knot has no schema dependencies,
	// so it serializes cleanly through the search path.
	UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(InterfaceGraph);
	KnotNode->CreateNewGuid();
	InterfaceGraph->Nodes.Add(KnotNode);

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("node_class"), TEXT("K2Node_Knot"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.search_nodes"), Params);
	TestTrue(TEXT("search_nodes succeeded"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		int32 Count = 0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("search_nodes finds the Knot in an interface-implementation graph"), Count >= 1);
	}

	TestBP->MarkAsGarbage();
	return true;
}
