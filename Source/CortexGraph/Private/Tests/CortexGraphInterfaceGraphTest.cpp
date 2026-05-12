#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Interface.h"

namespace CortexGraphInterfaceGraphTestUtils
{
	UBlueprint* CreateTestBlueprint(const TCHAR* PackageSuffix, const TCHAR* Name)
	{
		const FString PackagePath = FString::Printf(TEXT("/Game/Temp/%s"), PackageSuffix);
		UPackage* TestPackage = CreatePackage(*PackagePath);
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			TestPackage,
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	UEdGraph* InjectGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* Graph = NewObject<UEdGraph>(
			Blueprint,
			UEdGraph::StaticClass(),
			FName(GraphName),
			RF_Transactional);
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		return Graph;
	}

	UEdGraph* InjectInterfaceGraph(UBlueprint* Blueprint, UClass* InterfaceClass, const TCHAR* GraphName)
	{
		check(Blueprint && InterfaceClass);

		UEdGraph* Graph = InjectGraph(Blueprint, GraphName);

		FBPInterfaceDescription Desc;
		Desc.Interface = InterfaceClass;
		Desc.Graphs.Add(Graph);
		Blueprint->ImplementedInterfaces.Add(Desc);

		return Graph;
	}

	UEdGraph* InjectMacroGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* Graph = InjectGraph(Blueprint, GraphName);
		Blueprint->MacroGraphs.Add(Graph);
		return Graph;
	}

	UEdGraph* InjectDelegateGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* Graph = InjectGraph(Blueprint, GraphName);
		Blueprint->DelegateSignatureGraphs.Add(Graph);
		return Graph;
	}

	UK2Node_Knot* AddKnotNode(UEdGraph* Graph, int32 X = 0, int32 Y = 0)
	{
		UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
		KnotNode->CreateNewGuid();
		KnotNode->NodePosX = X;
		KnotNode->NodePosY = Y;
		Graph->Nodes.Add(KnotNode);
		return KnotNode;
	}

	UK2Node_CallFunction* AddPrintStringNode(UEdGraph* Graph)
	{
		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->CreateNewGuid();
		CallNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		Graph->Nodes.Add(CallNode);
		return CallNode;
	}

	TSharedPtr<FJsonObject> MakeAssetParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphListGraphsKindTest,
	"Cortex.Graph.InterfaceGraph.ListGraphs_ReportsKindField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphListGraphsKindTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphListKinds"), TEXT("BP_ListGraphsKind"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectMacroGraph(TestBP, TEXT("MyMacro"));
	InjectDelegateGraph(TestBP, TEXT("MyDelegate"));
	InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("MyInterfaceFunc"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	FCortexCommandResult Result = Router.Execute(TEXT("graph.list_graphs"), MakeAssetParams(TestBP->GetPathName()));
	TestTrue(TEXT("list_graphs succeeded"), Result.bSuccess);

	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
	TestTrue(TEXT("Result has graphs array"), Result.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));

	bool bSawUbergraph = false;
	bool bSawFunction = false;
	bool bSawMacro = false;
	bool bSawDelegate = false;
	bool bSawInterfaceImpl = false;
	bool bSawOwningInterface = false;

	if (GraphsArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *GraphsArray)
		{
			TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (!Entry.IsValid()) { continue; }

			FString Kind;
			TestTrue(TEXT("Each entry has a kind field"), Entry->TryGetStringField(TEXT("kind"), Kind));

			if (Kind == TEXT("ubergraph"))
			{
				bSawUbergraph = true;
			}
			else if (Kind == TEXT("function"))
			{
				bSawFunction = true;
			}
			else if (Kind == TEXT("macro"))
			{
				bSawMacro = true;
			}
			else if (Kind == TEXT("delegate"))
			{
				bSawDelegate = true;
			}
			else if (Kind == TEXT("interface_impl"))
			{
				bSawInterfaceImpl = true;
				FString OwningInterface;
				bSawOwningInterface |= Entry->TryGetStringField(TEXT("owning_interface"), OwningInterface)
					&& OwningInterface == UInterface::StaticClass()->GetName();
			}
		}
	}

	TestTrue(TEXT("Saw ubergraph kind"), bSawUbergraph);
	TestTrue(TEXT("Saw function kind"), bSawFunction);
	TestTrue(TEXT("Saw macro kind"), bSawMacro);
	TestTrue(TEXT("Saw delegate kind"), bSawDelegate);
	TestTrue(TEXT("Saw interface_impl kind"), bSawInterfaceImpl);
	TestTrue(TEXT("Saw owning_interface on interface_impl"), bSawOwningInterface);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphCapabilitiesDocTest,
	"Cortex.Graph.InterfaceGraph.Capabilities_DocumentGraphKindsAndDelegatePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphCapabilitiesDocTest::RunTest(const FString& Parameters)
{
	FCortexGraphCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	const auto FindCommand = [&Commands](const FString& CommandName) -> const FCortexCommandInfo*
	{
		for (const FCortexCommandInfo& Command : Commands)
		{
			if (Command.Name == CommandName)
			{
				return &Command;
			}
		}
		return nullptr;
	};

	const FCortexCommandInfo* ListGraphs = FindCommand(TEXT("list_graphs"));
	TestNotNull(TEXT("list_graphs capability exists"), ListGraphs);
	if (ListGraphs)
	{
		TestTrue(
			TEXT("list_graphs capability documents graph kind metadata"),
			ListGraphs->Description.Contains(TEXT("kind")));
		TestTrue(
			TEXT("list_graphs capability documents owning interface metadata"),
			ListGraphs->Description.Contains(TEXT("owning_interface")));
	}

	const TArray<FString> MutableCommands = {
		TEXT("add_node"),
		TEXT("remove_node"),
		TEXT("connect"),
		TEXT("disconnect"),
		TEXT("set_pin_value"),
		TEXT("auto_layout")
	};

	for (const FString& CommandName : MutableCommands)
	{
		const FCortexCommandInfo* Command = FindCommand(CommandName);
		TestNotNull(FString::Printf(TEXT("%s capability exists"), *CommandName), Command);
		if (Command)
		{
			TestTrue(
				FString::Printf(TEXT("%s capability documents delegate read-only policy"), *CommandName),
				Command->Description.Contains(TEXT("Delegate graphs are readable but not mutable")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphFindDirectTest,
	"Cortex.Graph.InterfaceGraph.FindGraph_ResolvesMacroAndInterfaceImpl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphFindDirectTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphFindUserGraphs"), TEXT("BP_FindUserGraphs"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* MacroGraph = InjectMacroGraph(TestBP, TEXT("MyMacro"));
	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("Sitting_Pose"));

	FCortexCommandResult Error;
	TestEqual(TEXT("FindGraph resolves macro graph"),
		FCortexGraphNodeOps::FindGraph(TestBP, TEXT("MyMacro"), Error), MacroGraph);
	TestEqual(TEXT("FindGraph resolves interface implementation graph"),
		FCortexGraphNodeOps::FindGraph(TestBP, TEXT("Sitting_Pose"), Error), InterfaceGraph);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphSearchNodesReachTest,
	"Cortex.Graph.InterfaceGraph.SearchNodes_ReachesInterfaceImplGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphSearchNodesReachTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphSearchInterface"), TEXT("BP_SearchNodesReach"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("FuncWithKnot"));
	AddKnotNode(InterfaceGraph);

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeAssetParams(TestBP->GetPathName());
	Params->SetStringField(TEXT("node_class"), TEXT("K2Node_Knot"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.search_nodes"), Params);
	TestTrue(TEXT("search_nodes succeeded"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		int32 Count = 0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("search_nodes finds the Knot in an interface implementation graph"), Count >= 1);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphAddNodePolicyTest,
	"Cortex.Graph.InterfaceGraph.AddNode_AllowsInterfaceImplRejectsDelegate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphAddNodePolicyTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphMutationPolicy"), TEXT("BP_MutationPolicy"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("EditableInterface"));
	InjectDelegateGraph(TestBP, TEXT("DelegateSignature"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> InterfaceParams = MakeAssetParams(TestBP->GetPathName());
	InterfaceParams->SetStringField(TEXT("graph_name"), TEXT("EditableInterface"));
	InterfaceParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_Knot"));
	FCortexCommandResult InterfaceResult = Router.Execute(TEXT("graph.add_node"), InterfaceParams);
	TestTrue(TEXT("add_node succeeds on interface implementation graph"), InterfaceResult.bSuccess);

	TSharedPtr<FJsonObject> DelegateParams = MakeAssetParams(TestBP->GetPathName());
	DelegateParams->SetStringField(TEXT("graph_name"), TEXT("DelegateSignature"));
	DelegateParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_Knot"));
	FCortexCommandResult DelegateResult = Router.Execute(TEXT("graph.add_node"), DelegateParams);
	TestFalse(TEXT("add_node rejects delegate signature graph"), DelegateResult.bSuccess);
	TestEqual(TEXT("delegate graph mutation error code"), DelegateResult.ErrorCode, CortexErrorCodes::InvalidOperation);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphConnectPolicyTest,
	"Cortex.Graph.InterfaceGraph.ConnectDisconnect_RejectDelegateGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphConnectPolicyTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphConnectionPolicy"), TEXT("BP_ConnectionPolicy"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	InjectDelegateGraph(TestBP, TEXT("DelegateSignature"));

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> ConnectParams = MakeAssetParams(TestBP->GetPathName());
	ConnectParams->SetStringField(TEXT("graph_name"), TEXT("DelegateSignature"));
	ConnectParams->SetStringField(TEXT("source_node"), TEXT("MissingA"));
	ConnectParams->SetStringField(TEXT("source_pin"), TEXT("then"));
	ConnectParams->SetStringField(TEXT("target_node"), TEXT("MissingB"));
	ConnectParams->SetStringField(TEXT("target_pin"), TEXT("execute"));
	FCortexCommandResult ConnectResult = Router.Execute(TEXT("graph.connect"), ConnectParams);
	TestFalse(TEXT("connect rejects delegate signature graph before node lookup"), ConnectResult.bSuccess);
	TestEqual(TEXT("delegate connect error code"), ConnectResult.ErrorCode, CortexErrorCodes::InvalidOperation);

	TSharedPtr<FJsonObject> DisconnectParams = MakeAssetParams(TestBP->GetPathName());
	DisconnectParams->SetStringField(TEXT("graph_name"), TEXT("DelegateSignature"));
	DisconnectParams->SetStringField(TEXT("node_id"), TEXT("MissingA"));
	DisconnectParams->SetStringField(TEXT("pin_name"), TEXT("then"));
	FCortexCommandResult DisconnectResult = Router.Execute(TEXT("graph.disconnect"), DisconnectParams);
	TestFalse(TEXT("disconnect rejects delegate signature graph before node lookup"), DisconnectResult.bSuccess);
	TestEqual(TEXT("delegate disconnect error code"), DisconnectResult.ErrorCode, CortexErrorCodes::InvalidOperation);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphGetSubgraphReachTest,
	"Cortex.Graph.InterfaceGraph.GetSubgraph_ReachesInterfaceImplGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphGetSubgraphReachTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphGetSubgraphInterface"), TEXT("BP_GetSubgraphInterface"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("InterfaceGraph"));
	AddKnotNode(InterfaceGraph);

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeAssetParams(TestBP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("InterfaceGraph"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
	TestTrue(TEXT("get_subgraph succeeds on interface implementation graph"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		int32 NodeCount = 0;
		Result.Data->TryGetNumberField(TEXT("node_count"), NodeCount);
		TestEqual(TEXT("get_subgraph returns interface graph node"), NodeCount, 1);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphFindFunctionCallsReachTest,
	"Cortex.Graph.InterfaceGraph.FindFunctionCalls_ReachesInterfaceImplGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphFindFunctionCallsReachTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphFindCallsInterface"), TEXT("BP_FindCallsInterface"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("InterfaceGraph"));
	AddPrintStringNode(InterfaceGraph);

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	TSharedPtr<FJsonObject> Params = MakeAssetParams(TestBP->GetPathName());
	Params->SetStringField(TEXT("function_name"), TEXT("PrintString"));

	FCortexCommandResult Result = Router.Execute(TEXT("graph.find_function_calls"), Params);
	TestTrue(TEXT("find_function_calls succeeds"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		int32 Count = 0;
		Result.Data->TryGetNumberField(TEXT("count"), Count);
		TestTrue(TEXT("find_function_calls finds call in interface implementation graph"), Count >= 1);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInterfaceGraphAutoLayoutReachTest,
	"Cortex.Graph.InterfaceGraph.AutoLayout_ReachesInterfaceImplGraphWhenNoGraphFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInterfaceGraphAutoLayoutReachTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = CreateTestBlueprint(TEXT("CortexGraphAutoLayoutInterface"), TEXT("BP_AutoLayoutInterface"));
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* InterfaceGraph = InjectInterfaceGraph(TestBP, UInterface::StaticClass(), TEXT("InterfaceGraph"));
	UK2Node_Knot* KnotNode = AddKnotNode(InterfaceGraph, 777, 555);

	FCortexCommandRouter Router;
	RegisterGraphDomain(Router);

	FCortexCommandResult Result = Router.Execute(TEXT("graph.auto_layout"), MakeAssetParams(TestBP->GetPathName()));
	TestTrue(TEXT("auto_layout succeeded"), Result.bSuccess);

	if (Result.bSuccess)
	{
		TestNotEqual(TEXT("auto_layout changed interface graph node X"), KnotNode->NodePosX, 777);
	}

	TestBP->MarkAsGarbage();
	return true;
}
