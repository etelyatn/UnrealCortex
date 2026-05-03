#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphTraceExecTest,
	"Cortex.Graph.Trace.ExecChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphTraceExecTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphTraceExecTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_TraceExecTest"),
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

	const auto AddPrintStringNode = [&](const TCHAR* Label) -> FString
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		AddParams->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
		TestTrue(FString::Printf(TEXT("add_node %s succeeds"), Label), AddResult.bSuccess);

		FString NodeId;
		if (AddResult.Data.IsValid())
		{
			AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}
		return NodeId;
	};

	const FString FirstPrintNodeId = AddPrintStringNode(TEXT("first"));
	const FString SecondPrintNodeId = AddPrintStringNode(TEXT("second"));
	TestFalse(TEXT("First print node has id"), FirstPrintNodeId.IsEmpty());
	TestFalse(TEXT("Second print node has id"), SecondPrintNodeId.IsEmpty());
	if (FirstPrintNodeId.IsEmpty() || SecondPrintNodeId.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), AssetPath);
	ConnectParams->SetStringField(TEXT("source_node"), FirstPrintNodeId);
	ConnectParams->SetStringField(TEXT("source_pin"), TEXT("then"));
	ConnectParams->SetStringField(TEXT("target_node"), SecondPrintNodeId);
	ConnectParams->SetStringField(TEXT("target_pin"), TEXT("execute"));

	const FCortexCommandResult ConnectResult = Router.Execute(TEXT("graph.connect"), ConnectParams);
	TestTrue(TEXT("connect succeeds"), ConnectResult.bSuccess);

	TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
	TraceParams->SetStringField(TEXT("asset_path"), AssetPath);
	TraceParams->SetStringField(TEXT("start_node_id"), FirstPrintNodeId);
	TraceParams->SetNumberField(TEXT("max_depth"), 10);
	TraceParams->SetStringField(TEXT("traverse_policy"), TEXT("Opaque"));
	TraceParams->SetBoolField(TEXT("include_edges"), true);

	const FCortexCommandResult TraceResult = Router.Execute(TEXT("graph.trace_exec"), TraceParams);
	TestTrue(TEXT("trace_exec succeeds"), TraceResult.bSuccess);
	if (TraceResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		TestTrue(TEXT("trace_exec returns nodes"), TraceResult.Data->TryGetArrayField(TEXT("nodes"), Nodes));
		if (Nodes)
		{
			TestTrue(TEXT("trace_exec returns at least the start and downstream node"), Nodes->Num() >= 2);
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
