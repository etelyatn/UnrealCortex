#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAutoLayoutTest,
	"Cortex.Graph.AutoLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAutoLayoutTest::RunTest(const FString& Parameters)
{
	// Create transient Blueprint with nodes
	UPackage* TestPackage = CreatePackage(TEXT("/Game/Temp/CortexGraphAutoLayoutTest"));
	TestPackage->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), TestPackage, TEXT("BP_GraphAutoLayoutTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), TestBP);
	if (!TestBP) return false;

	FString AssetPath = TestBP->GetPathName();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	// Add two PrintString nodes at (0,0)
	for (int32 i = 0; i < 2; ++i)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		P->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		P->SetObjectField(TEXT("params"), NP);
		FCortexCommandResult R = Router.Execute(TEXT("graph.add_node"), P);
		TestTrue(TEXT("add_node succeeded"), R.bSuccess);
	}

	// Call graph.auto_layout
	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	LayoutParams->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult LayoutResult = Router.Execute(TEXT("graph.auto_layout"), LayoutParams);

	TestTrue(TEXT("auto_layout succeeded"), LayoutResult.bSuccess);
	if (LayoutResult.bSuccess && LayoutResult.Data.IsValid())
	{
		double NodeCount = 0;
		LayoutResult.Data->TryGetNumberField(TEXT("node_count"), NodeCount);
		TestTrue(TEXT("node_count > 0"), NodeCount > 0);

		double GraphsProcessed = 0;
		LayoutResult.Data->TryGetNumberField(TEXT("graphs_processed"), GraphsProcessed);
		TestTrue(TEXT("graphs_processed > 0"), GraphsProcessed > 0);
	}

	// Test empty graph (only default event nodes)
	UPackage* EmptyPkg = CreatePackage(TEXT("/Game/Temp/CortexGraphAutoLayoutEmptyTest"));
	EmptyPkg->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* EmptyBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), EmptyPkg, TEXT("BP_GraphAutoLayoutEmpty"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()
	);
	if (EmptyBP)
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		EmptyParams->SetStringField(TEXT("asset_path"), EmptyBP->GetPathName());
		FCortexCommandResult EmptyResult = Router.Execute(TEXT("graph.auto_layout"), EmptyParams);
		TestTrue(TEXT("auto_layout on empty graph succeeds"), EmptyResult.bSuccess);
		EmptyBP->MarkAsGarbage();
	}

	// Test with graph_name filter
	{
		TSharedPtr<FJsonObject> FilterParams = MakeShared<FJsonObject>();
		FilterParams->SetStringField(TEXT("asset_path"), AssetPath);
		FilterParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		FCortexCommandResult FilterResult = Router.Execute(TEXT("graph.auto_layout"), FilterParams);
		TestTrue(TEXT("auto_layout with graph_name filter succeeded"), FilterResult.bSuccess);
		if (FilterResult.bSuccess && FilterResult.Data.IsValid())
		{
			double GraphsProcessed = 0;
			FilterResult.Data->TryGetNumberField(TEXT("graphs_processed"), GraphsProcessed);
			TestEqual(TEXT("Should process exactly 1 graph"), static_cast<int32>(GraphsProcessed), 1);
		}
	}

	// Test missing asset_path returns error
	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		FCortexCommandResult BadResult = Router.Execute(TEXT("graph.auto_layout"), BadParams);
		TestFalse(TEXT("auto_layout without asset_path should fail"), BadResult.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), BadResult.ErrorCode, CortexErrorCodes::InvalidField);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAutoLayoutBodySpacingTest,
	"Cortex.Graph.AutoLayout.BodySpacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAutoLayoutBodySpacingTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Game/Temp/CortexLayoutBody_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, TEXT("BP_LayoutBody"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Blueprint fixture"), Blueprint))
	{
		Package->MarkAsGarbage();
		return false;
	}
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	TArray<UK2Node_CallFunction*> Calls;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->SetFromFunction(Index == 0
			? UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("GetAllActorsOfClassWithTag"))
			: UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		Graph->AddNode(Node, false, false);
		Calls.Add(Node);
	}
	UEdGraphPin* Out = Calls[0]->FindPinChecked(TEXT("then"));
	UEdGraphPin* In = Calls[1]->FindPinChecked(TEXT("execute"));
	Out->MakeLinkTo(In);
	const FGuid SourceGuid = Calls[0]->NodeGuid;
	const FGuid TargetGuid = Calls[1]->NodeGuid;
	UEdGraphPin* DefaultPin = Calls[1]->FindPinChecked(TEXT("InString"));
	const FString OriginalDefault = DefaultPin->DefaultValue;
	FCortexGraphCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("graph_name"), Graph->GetName());
	Params->SetNumberField(TEXT("horizontal_spacing"), 80);
	const FCortexCommandResult Result = Handler.Execute(TEXT("auto_layout"), Params);
	TestTrue(TEXT("Full layout succeeds"), Result.bSuccess);
	const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Calls[0]);
	Widget->SlatePrepass(1.0f);
	const int32 BodyWidth = FMath::CeilToInt(Widget->GetDesiredSize().X);
	TestTrue(TEXT("Rendered call bodies have requested horizontal clearance"),
		Calls[1]->NodePosX >= Calls[0]->NodePosX + BodyWidth + 80);
	TestEqual(TEXT("Source identity preserved"), Calls[0]->NodeGuid, SourceGuid);
	TestEqual(TEXT("Target identity preserved"), Calls[1]->NodeGuid, TargetGuid);
	TestTrue(TEXT("Execution edge preserved"), Out->LinkedTo.Contains(In) && In->LinkedTo.Contains(Out));
	TestEqual(TEXT("Input default preserved"), DefaultPin->DefaultValue, OriginalDefault);
	const FIntPoint SourcePosition(Calls[0]->NodePosX, Calls[0]->NodePosY);
	const FIntPoint TargetPosition(Calls[1]->NodePosX, Calls[1]->NodePosY);
	Package->SetDirtyFlag(false);
	const FCortexCommandResult Repeat = Handler.Execute(TEXT("auto_layout"), Params);
	TestTrue(TEXT("Repeat succeeds"), Repeat.bSuccess);
	TestEqual(TEXT("Source stable"), FIntPoint(Calls[0]->NodePosX, Calls[0]->NodePosY), SourcePosition);
	TestEqual(TEXT("Target stable"), FIntPoint(Calls[1]->NodePosX, Calls[1]->NodePosY), TargetPosition);
	TestFalse(TEXT("Unchanged formatting does not dirty package"), Package->IsDirty());
	TSharedPtr<FJsonObject> Unbound = MakeShared<FJsonObject>();
	Unbound->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Unbound->SetStringField(TEXT("subgraph_path"), TEXT("UnboundChild"));
	const FCortexCommandResult Refused = Handler.Execute(TEXT("auto_layout"), Unbound);
	TestFalse(TEXT("Composite path without root graph is refused"), Refused.bSuccess);
	TestEqual(TEXT("Unbound target reports invalid field"), Refused.ErrorCode, FString(TEXT("INVALID_FIELD")));
	TestFalse(TEXT("Refused targeting leaves package clean"), Package->IsDirty());
	Blueprint->MarkAsGarbage();
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAutoLayoutPersistedPresentationTest,
	"Cortex.Graph.AutoLayout.PersistedPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAutoLayoutPersistedPresentationTest::RunTest(const FString& Parameters)
{
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Game/Temp/CortexLayoutPresentation_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, TEXT("BP_Presentation"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Presentation fixture"), Blueprint))
	{
		Package->MarkAsGarbage();
		return false;
	}
	FCortexGraphCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> Construction = MakeShared<FJsonObject>();
	Construction->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	Params->SetObjectField(TEXT("params"), Construction);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestTrue(TEXT("Author development-only call"), Handler.Execute(TEXT("add_node"), Params).bSuccess);
	}
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	TestTrue(TEXT("Initial presentation layout"), Handler.Execute(TEXT("auto_layout"), Params).bSuccess);
	TMap<FGuid, FIntPoint> Positions;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Positions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
		if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			TestTrue(TEXT("Authored call already has its persisted development-only presentation"),
				Call->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly);
			// Exercise the normal loading hook that reconciles function metadata.
			// Disk persistence is also exercised through core.save_asset/reload_asset.
			TArray<uint8> Bytes;
			FObjectWriter Writer(Bytes);
			Call->Serialize(Writer);
			FObjectReader Reader(Bytes);
			Reader.SetCustomVersions(Writer.GetCustomVersions());
			Call->Serialize(Reader);
		}
	}
	Package->SetDirtyFlag(false);
	const FCortexCommandResult Repeat = Handler.Execute(TEXT("auto_layout"), Params);
	TestTrue(TEXT("Loaded presentation layout succeeds"), Repeat.bSuccess);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		TestEqual(TEXT("Loading unchanged calls preserves formatted positions"),
			FIntPoint(Node->NodePosX, Node->NodePosY), Positions[Node->NodeGuid]);
	}
	TestFalse(TEXT("Loaded presentation no-op leaves package clean"), Package->IsDirty());
	Blueprint->MarkAsGarbage();
	Package->MarkAsGarbage();
	return true;
}
