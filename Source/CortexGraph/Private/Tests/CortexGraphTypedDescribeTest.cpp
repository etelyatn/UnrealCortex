#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Curves/CurveFloat.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if WITH_EDITOR

namespace
{
void CleanupDescribeTestPackage(UPackage* Pkg)
{
	if (!Pkg) return;
	Pkg->SetDirtyFlag(false);
	if (Pkg->IsRooted())
	{
		Pkg->RemoveFromRoot();
	}
	Pkg->ClearFlags(RF_Standalone);
	Pkg->MarkAsGarbage();
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("TestCleanup")));
	}
}

const TSharedPtr<FJsonObject>* FindPinByName(const TArray<TSharedPtr<FJsonValue>>* PinsArray, const FString& PinName)
{
	if (!PinsArray) return nullptr;
	for (const TSharedPtr<FJsonValue>& Val : *PinsArray)
	{
		const TSharedPtr<FJsonObject>* PinObj = nullptr;
		if (Val.IsValid() && Val->TryGetObject(PinObj) && PinObj && (*PinObj).IsValid())
		{
			FString Name;
			if ((*PinObj)->TryGetStringField(TEXT("name"), Name) && Name == PinName)
			{
				return PinObj;
			}
		}
	}
	return nullptr;
}
}

// ---------------------------------------------------------------------------
// Test 1: Widget Blueprint Self type in real context
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeWidgetSelfTest,
	"Cortex.Graph.Authoring.Describe.WidgetSelf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeWidgetSelfTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/WBP_DescribeWidgetSelfTest"));
	Pkg->SetPackageFlags(PKG_PlayInEditor);

	UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
		Pkg, UWidgetBlueprint::StaticClass(), TEXT("WBP_DescribeWidgetSelfTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	WBP->ParentClass = UUserWidget::StaticClass();
	WBP->WidgetTree = NewObject<UWidgetTree>(WBP, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
	UCanvasPanel* RootCanvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WBP->WidgetTree->RootWidget = RootCanvas;

	UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
		WBP, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(WBP, EventGraph);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_class"), TEXT("Self"));
	Params->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	Params->SetObjectField(TEXT("target"), TargetObj);

	const bool bWasDirty = WBP->GetOutermost()->IsDirty();
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);

	TestTrue(TEXT("contextual Self description succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		CleanupDescribeTestPackage(Pkg);
		return false;
	}

	TestEqual(TEXT("validated not family-only"),
		Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));
	TestEqual(TEXT("describe leaves dirty state unchanged"),
		WBP->GetOutermost()->IsDirty(), bWasDirty);

	// Assert the actual self subtype path equals this fixture's authoritative class, not /Script/Engine.Actor
	const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
	TestTrue(TEXT("expected_pins array present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
	const TSharedPtr<FJsonObject>* SelfPin = FindPinByName(PinsArray, TEXT("self"));
	TestNotNull(TEXT("self pin found"), SelfPin);
	if (SelfPin)
	{
		FString SubObjPath;
		(*SelfPin)->TryGetStringField(TEXT("sub_category_object"), SubObjPath);
		TestEqual(TEXT("self subtype path equals Widget Blueprint generated class, not Actor"),
			SubObjPath, WBP->GeneratedClass->GetPathName());
		TestFalse(TEXT("self subtype path is not Actor"),
			SubObjPath.Contains(TEXT("Actor")));
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Designer member types in Widget Blueprint context
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeDesignerMembersTest,
	"Cortex.Graph.Authoring.Describe.DesignerMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeDesignerMembersTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/WBP_DescribeDesignerMembersTest"));
	Pkg->SetPackageFlags(PKG_PlayInEditor);

	UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
		Pkg, UWidgetBlueprint::StaticClass(), TEXT("WBP_DescribeDesignerMembersTest"),
		RF_Public | RF_Standalone | RF_Transactional);
	WBP->ParentClass = UUserWidget::StaticClass();
	WBP->WidgetTree = NewObject<UWidgetTree>(WBP, UWidgetTree::StaticClass(), TEXT("WidgetTree"));
	UCanvasPanel* RootCanvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	WBP->WidgetTree->RootWidget = RootCanvas;
	UTextBlock* DesignWidget = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MyTextBlock"));
	DesignWidget->bIsVariable = true;
	RootCanvas->AddChild(DesignWidget);

	UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
		WBP, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(WBP, EventGraph);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	// 1. VariableGet on is_variable=true designer widget succeeds with validated status and UTextBlock type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
		Params->SetStringField(TEXT("asset_path"), WBP->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("MyTextBlock"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("VariableGet describe succeeds for designer widget"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("validation_status is validated"),
				Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray));
			const TSharedPtr<FJsonObject>* VarPin = FindPinByName(PinsArray, TEXT("MyTextBlock"));
			TestNotNull(TEXT("MyTextBlock output pin found"), VarPin);
			if (VarPin)
			{
				TestEqual(TEXT("pin category is object"),
					(*VarPin)->GetStringField(TEXT("category")), FString(TEXT("object")));
				TestEqual(TEXT("pin sub_category_object is UTextBlock"),
					(*VarPin)->GetStringField(TEXT("sub_category_object")), UTextBlock::StaticClass()->GetPathName());
			}
		}
	}

	// 2. Designer widget with is_variable=false fails validation with actionable prerequisite guidance
	{
		DesignWidget->bIsVariable = false;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
		Params->SetStringField(TEXT("asset_path"), WBP->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("MyTextBlock"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestFalse(TEXT("VariableGet describe fails when is_variable is false"), Result.bSuccess);
		TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("Error message mentions umg.set_widget_variable"),
			Result.ErrorMessage.Contains(TEXT("umg.set_widget_variable")));
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Pure function pins have no exec pins
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribePureFunctionTest,
	"Cortex.Graph.Authoring.Describe.PureFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribePureFunctionTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribePureFuncTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribePureFuncTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	TestNotNull(TEXT("EventGraph exists"), EventGraph);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetObjectField(TEXT("target"), TargetObj);
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("IsValid"));
	Params->SetObjectField(TEXT("params"), NodeParams);

	const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
	TestTrue(TEXT("Pure function describe succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("validation_status is validated"),
			Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

		// Resolved symbol shows is_pure: true
		const TSharedPtr<FJsonObject>* SymObj = nullptr;
		TestTrue(TEXT("resolved_symbol present"), Result.Data->TryGetObjectField(TEXT("resolved_symbol"), SymObj) && SymObj != nullptr);
		if (SymObj && (*SymObj).IsValid())
		{
			TestTrue(TEXT("is_pure is true"), (*SymObj)->GetBoolField(TEXT("is_pure")));
		}

		// Expected pins must NOT contain any exec pins
		const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
		TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
		if (PinsArray)
		{
			for (const TSharedPtr<FJsonValue>& Val : *PinsArray)
			{
				const TSharedPtr<FJsonObject>* PinObj = nullptr;
				if (Val.IsValid() && Val->TryGetObject(PinObj) && PinObj && (*PinObj).IsValid())
				{
					TestNotEqual(TEXT("No exec pin on pure function"),
						(*PinObj)->GetStringField(TEXT("type")), FString(TEXT("exec")));
					TestNotEqual(TEXT("No exec category on pure function"),
						(*PinObj)->GetStringField(TEXT("category")), FString(TEXT("exec")));
				}
			}

			// Must have ReturnValue pin
			const TSharedPtr<FJsonObject>* RetPin = FindPinByName(PinsArray, TEXT("ReturnValue"));
			TestNotNull(TEXT("ReturnValue pin found"), RetPin);
			if (RetPin)
			{
				TestEqual(TEXT("ReturnValue category is bool"),
					(*RetPin)->GetStringField(TEXT("category")), FString(TEXT("bool")));
				TestEqual(TEXT("ReturnValue direction is output"),
					(*RetPin)->GetStringField(TEXT("direction")), FString(TEXT("output")));
			}
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Inherited declaration identity
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeInheritedDeclarationTest,
	"Cortex.Graph.Authoring.Describe.InheritedDeclaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeInheritedDeclarationTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeInheritedTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		APawn::StaticClass(), Pkg, FName("BP_DescribeInheritedTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	TestNotNull(TEXT("EventGraph exists"), EventGraph);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetObjectField(TEXT("target"), TargetObj);
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Pawn"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("K2_DestroyActor"));
	Params->SetObjectField(TEXT("params"), NodeParams);

	const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
	TestTrue(TEXT("Inherited function describe succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("validation_status is validated"),
			Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

		const TSharedPtr<FJsonObject>* SymObj = nullptr;
		TestTrue(TEXT("resolved_symbol present"), Result.Data->TryGetObjectField(TEXT("resolved_symbol"), SymObj) && SymObj != nullptr);
		if (SymObj && (*SymObj).IsValid())
		{
			TestEqual(TEXT("declaring_class is Actor"),
				(*SymObj)->GetStringField(TEXT("declaring_class")), FString(TEXT("/Script/Engine.Actor")));
			TestEqual(TEXT("context_class is Pawn"),
				(*SymObj)->GetStringField(TEXT("context_class")), FString(TEXT("/Script/Engine.Pawn")));
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: Class-dependent pins for DynamicCast and ConstructObject
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeClassDependentPinsTest,
	"Cortex.Graph.Authoring.Describe.ClassDependentPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeClassDependentPinsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeClassDepPinsTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeClassDepPinsTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	// 1. DynamicCast to Pawn
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("DynamicCast"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.Pawn"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("DynamicCast describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("validation_status is validated"),
				Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
			const TSharedPtr<FJsonObject>* CastPin = FindPinByName(PinsArray, TEXT("AsPawn"));
			TestNotNull(TEXT("AsPawn pin found"), CastPin);
			if (CastPin)
			{
				TestEqual(TEXT("AsPawn category is object"),
					(*CastPin)->GetStringField(TEXT("category")), FString(TEXT("object")));
				TestEqual(TEXT("AsPawn sub_category_object is Pawn"),
					(*CastPin)->GetStringField(TEXT("sub_category_object")), APawn::StaticClass()->GetPathName());
			}
		}
	}

	// 2. ConstructObject
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("ConstructObject"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.CurveFloat"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("ConstructObject describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("validation_status is validated"),
				Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
			const TSharedPtr<FJsonObject>* RetPin = FindPinByName(PinsArray, TEXT("ReturnValue"));
			TestNotNull(TEXT("ReturnValue pin found"), RetPin);
			if (RetPin)
			{
				TestEqual(TEXT("ReturnValue category is object"),
					(*RetPin)->GetStringField(TEXT("category")), FString(TEXT("object")));
				TestEqual(TEXT("ReturnValue sub_category_object is CurveFloat"),
					(*RetPin)->GetStringField(TEXT("sub_category_object")), UCurveFloat::StaticClass()->GetPathName());
			}
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: Map terminal type readback
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeMapTerminalTypeTest,
	"Cortex.Graph.Authoring.Describe.MapTerminalType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeMapTerminalTypeTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeMapTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeMapTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	// Add a Map<String, int32> member variable
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	PinType.ContainerType = EPinContainerType::Map;
	PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName("StringToIntMap"), PinType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetObjectField(TEXT("target"), TargetObj);
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("variable_name"), TEXT("StringToIntMap"));
	Params->SetObjectField(TEXT("params"), NodeParams);

	const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
	TestTrue(TEXT("Map VariableGet describe succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("validation_status is validated"),
			Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("validated")));

		const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
		TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
		const TSharedPtr<FJsonObject>* MapPin = FindPinByName(PinsArray, TEXT("StringToIntMap"));
		TestNotNull(TEXT("StringToIntMap pin found"), MapPin);
		if (MapPin)
		{
			TestEqual(TEXT("container_type is map"),
				(*MapPin)->GetStringField(TEXT("container_type")), FString(TEXT("map")));
			TestEqual(TEXT("category is string"),
				(*MapPin)->GetStringField(TEXT("category")), FString(TEXT("string")));

			const TSharedPtr<FJsonObject>* TermType = nullptr;
			TestTrue(TEXT("map_terminal_type present"),
				(*MapPin)->TryGetObjectField(TEXT("map_terminal_type"), TermType) && TermType != nullptr);
			if (TermType && (*TermType).IsValid())
			{
				TestEqual(TEXT("terminal category is int"),
					(*TermType)->GetStringField(TEXT("category")), FString(TEXT("int")));
			}
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: Advanced and hidden flags on pins
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeAdvancedHiddenFlagsTest,
	"Cortex.Graph.Authoring.Describe.AdvancedHiddenFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeAdvancedHiddenFlagsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeAdvHiddenTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeAdvHiddenTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetObjectField(TEXT("target"), TargetObj);
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Params->SetObjectField(TEXT("params"), NodeParams);

	const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
	TestTrue(TEXT("PrintString describe succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
		TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);

		// Self / WorldContextObject pin should have is_hidden: true
		const TSharedPtr<FJsonObject>* SelfPin = FindPinByName(PinsArray, TEXT("self"));
		if (!SelfPin)
		{
			SelfPin = FindPinByName(PinsArray, TEXT("WorldContextObject"));
		}
		TestNotNull(TEXT("WorldContextObject / self pin found"), SelfPin);
		if (SelfPin)
		{
			TestTrue(TEXT("WorldContextObject pin is_hidden is true"),
				(*SelfPin)->GetBoolField(TEXT("is_hidden")));
		}

		// InString is neither hidden nor advanced
		const TSharedPtr<FJsonObject>* InStringPin = FindPinByName(PinsArray, TEXT("InString"));
		TestNotNull(TEXT("InString pin found"), InStringPin);
		if (InStringPin)
		{
			TestFalse(TEXT("InString is_hidden is false"), (*InStringPin)->GetBoolField(TEXT("is_hidden")));
			TestFalse(TEXT("InString is_advanced is false"), (*InStringPin)->GetBoolField(TEXT("is_advanced")));
		}

		// TextColor or Duration has is_advanced: true in PrintString
		const TSharedPtr<FJsonObject>* TextColorPin = FindPinByName(PinsArray, TEXT("TextColor"));
		TestNotNull(TEXT("TextColor pin found"), TextColorPin);
		if (TextColorPin)
		{
			TestTrue(TEXT("TextColor is_advanced is true"), (*TextColorPin)->GetBoolField(TEXT("is_advanced")));
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: Ref / const outputs
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeRefConstOutputsTest,
	"Cortex.Graph.Authoring.Describe.RefConstOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeRefConstOutputsTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeRefConstTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeRefConstTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	// LineTraceSingle has OutHit (FHitResult out param) and ActorsToIgnore (const array input)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("LineTraceSingle"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("LineTraceSingle describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
			const TSharedPtr<FJsonObject>* OutHitPin = FindPinByName(PinsArray, TEXT("OutHit"));
			TestNotNull(TEXT("OutHit pin found"), OutHitPin);
			if (OutHitPin)
			{
				TestEqual(TEXT("OutHit direction is output"),
					(*OutHitPin)->GetStringField(TEXT("direction")), FString(TEXT("output")));
				TestEqual(TEXT("OutHit category is struct"),
					(*OutHitPin)->GetStringField(TEXT("category")), FString(TEXT("struct")));
			}

			const TSharedPtr<FJsonObject>* ActorsPin = FindPinByName(PinsArray, TEXT("ActorsToIgnore"));
			TestNotNull(TEXT("ActorsToIgnore pin found"), ActorsPin);
			if (ActorsPin)
			{
				TestEqual(TEXT("ActorsToIgnore direction is input"),
					(*ActorsPin)->GetStringField(TEXT("direction")), FString(TEXT("input")));
				TestEqual(TEXT("ActorsToIgnore container_type is array"),
					(*ActorsPin)->GetStringField(TEXT("container_type")), FString(TEXT("array")));
				TestTrue(TEXT("ActorsToIgnore is_const is true"),
					(*ActorsPin)->GetBoolField(TEXT("is_const")));
			}
		}
	}

	// K2_InvalidateTimerHandle has UPARAM(ref) Handle parameter where is_reference is true
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("K2_InvalidateTimerHandle"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("K2_InvalidateTimerHandle describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			TestTrue(TEXT("expected_pins present"), Result.Data->TryGetArrayField(TEXT("expected_pins"), PinsArray) && PinsArray != nullptr);
			const TSharedPtr<FJsonObject>* HandlePin = FindPinByName(PinsArray, TEXT("Handle"));
			TestNotNull(TEXT("Handle pin found"), HandlePin);
			if (HandlePin)
			{
				TestTrue(TEXT("Handle is_reference is true"),
					(*HandlePin)->GetBoolField(TEXT("is_reference")));
			}
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: Invalid supplied selector rejection
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeInvalidSelectorRejectionTest,
	"Cortex.Graph.Authoring.Describe.InvalidSelectorRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeInvalidSelectorRejectionTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_DescribeInvalidSelTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeInvalidSelTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	// 1. Invalid function name rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		NodeParams->SetStringField(TEXT("function_name"), TEXT("ThisFunctionDoesNotExist_12345"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestFalse(TEXT("Invalid function selector rejected"), Result.bSuccess);
		TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// 2. Invalid variable name rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), TargetObj);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("variable_name"), TEXT("ThisVariableDoesNotExist_12345"));
		Params->SetObjectField(TEXT("params"), NodeParams);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestFalse(TEXT("Invalid variable selector rejected"), Result.bSuccess);
		TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// 3. Invalid target graph GUID rejected
	{
		TSharedPtr<FJsonObject> BadTarget = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> BadGraphRef = MakeShared<FJsonObject>();
		BadGraphRef->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		BadTarget->SetObjectField(TEXT("graph_ref"), BadGraphRef);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("Self"));
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetObjectField(TEXT("target"), BadTarget);

		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestFalse(TEXT("Invalid target graph GUID rejected"), Result.bSuccess);
		TestEqual(TEXT("Error code is GRAPH_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::GraphNotFound);
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: Retain generic family-only describe without asset context
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeFamilyOnlyTest,
	"Cortex.Graph.Authoring.Describe.FamilyOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeFamilyOnlyTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	// Generic describe without asset context returns family_only validation_status
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_VariableGet"));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("Generic VariableGet describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("validation_status is family_only"),
				Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("family_only")));
			TestTrue(TEXT("supported is true"), Result.Data->GetBoolField(TEXT("supported")));
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("Self"));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("Generic Self describe succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestEqual(TEXT("validation_status is family_only"),
				Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("family_only")));
			TestTrue(TEXT("supported is true"), Result.Data->GetBoolField(TEXT("supported")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 11: Compare validated describe output to actual added node
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeEquivalenceWithAddNodeTest,
	"Cortex.Graph.Authoring.Describe.EquivalenceWithAddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphDescribeEquivalenceWithAddNodeTest::RunTest(const FString& Parameters)
{
	UPackage* Pkg = CreatePackage(TEXT("/Game/Temp/BP_DescribeEquivTest"));
	Pkg->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Pkg, FName("BP_DescribeEquivTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	TestNotNull(TEXT("EventGraph exists"), EventGraph);

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : TEXT(""));
	TargetObj->SetObjectField(TEXT("graph_ref"), GraphRef);

	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.KismetSystemLibrary"));
	NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));

	// 1. Describe node in context
	TSharedPtr<FJsonObject> DescParams = MakeShared<FJsonObject>();
	DescParams->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	DescParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	DescParams->SetObjectField(TEXT("target"), TargetObj);
	DescParams->SetObjectField(TEXT("params"), NodeParams);

	const bool bWasDirty = Blueprint->GetOutermost()->IsDirty();
	const FCortexCommandResult DescResult = Router.Execute(TEXT("graph.describe_node"), DescParams);
	TestTrue(TEXT("Describe succeeds"), DescResult.bSuccess);
	TestEqual(TEXT("Describe leaves dirty state unchanged"), Blueprint->GetOutermost()->IsDirty(), bWasDirty);

	if (!DescResult.bSuccess || !DescResult.Data.IsValid())
	{
		CleanupDescribeTestPackage(Pkg);
		return false;
	}

	// 2. Add node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	AddParams->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	AddParams->SetStringField(TEXT("graph_name"), EventGraph->GetName());
	AddParams->SetObjectField(TEXT("params"), NodeParams);

	const FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	TestTrue(TEXT("AddNode succeeds"), AddResult.bSuccess);
	if (!AddResult.bSuccess || !AddResult.Data.IsValid())
	{
		CleanupDescribeTestPackage(Pkg);
		return false;
	}

	// 3. Compare expected_pins from describe with pins from add_node
	const TArray<TSharedPtr<FJsonValue>>* DescPins = nullptr;
	TestTrue(TEXT("expected_pins present"), DescResult.Data->TryGetArrayField(TEXT("expected_pins"), DescPins) && DescPins != nullptr);

	const TArray<TSharedPtr<FJsonValue>>* AddPins = nullptr;
	TestTrue(TEXT("add_node pins present"), AddResult.Data->TryGetArrayField(TEXT("pins"), AddPins) && AddPins != nullptr);

	if (DescPins && AddPins)
	{
		TestEqual(TEXT("Same number of pins in describe and add_node"), DescPins->Num(), AddPins->Num());

		for (const TSharedPtr<FJsonValue>& AddVal : *AddPins)
		{
			const TSharedPtr<FJsonObject>* AddPinObj = nullptr;
			if (!AddVal.IsValid() || !AddVal->TryGetObject(AddPinObj) || !AddPinObj || !(*AddPinObj).IsValid())
			{
				continue;
			}
			FString PinName = (*AddPinObj)->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject>* DescPinObj = FindPinByName(DescPins, PinName);
			TestNotNull(FString::Printf(TEXT("Pin '%s' found in describe expected_pins"), *PinName), DescPinObj);
			if (DescPinObj)
			{
				TestEqual(FString::Printf(TEXT("Pin '%s' direction matches"), *PinName),
					(*DescPinObj)->GetStringField(TEXT("direction")), (*AddPinObj)->GetStringField(TEXT("direction")));
				TestEqual(FString::Printf(TEXT("Pin '%s' category matches"), *PinName),
					(*DescPinObj)->GetStringField(TEXT("category")), (*AddPinObj)->GetStringField(TEXT("category")));
				TestEqual(FString::Printf(TEXT("Pin '%s' subcategory matches"), *PinName),
					(*DescPinObj)->GetStringField(TEXT("subcategory")), (*AddPinObj)->GetStringField(TEXT("subcategory")));
				TestEqual(FString::Printf(TEXT("Pin '%s' sub_category_object matches"), *PinName),
					(*DescPinObj)->GetStringField(TEXT("sub_category_object")), (*AddPinObj)->GetStringField(TEXT("sub_category_object")));
				TestEqual(FString::Printf(TEXT("Pin '%s' container_type matches"), *PinName),
					(*DescPinObj)->GetStringField(TEXT("container_type")), (*AddPinObj)->GetStringField(TEXT("container_type")));
			}
		}
	}

	CleanupDescribeTestPackage(Pkg);
	return true;
}

#endif // WITH_EDITOR
