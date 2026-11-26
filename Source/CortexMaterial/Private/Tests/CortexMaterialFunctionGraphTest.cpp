#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

namespace
{
struct FFunctionGraphFixture
{
	FString Name;
	UPackage* Package = nullptr;
	UMaterialFunction* Function = nullptr;
	UMaterialExpressionScalarParameter* Scalar = nullptr;
	UMaterialExpressionFunctionOutput* Output = nullptr;

	explicit FFunctionGraphFixture(const TCHAR* Prefix = TEXT("Graph"))
	{
		Name = FString::Printf(TEXT("MF_%s_%s"), Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
		Package = CreatePackage(*(TEXT("/Game/Temp/CortexMaterialFunctionTest/") + Name));
		Function = NewObject<UMaterialFunction>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);

		Scalar = NewObject<UMaterialExpressionScalarParameter>(Function, NAME_None, RF_Transactional);
		Scalar->ParameterName = TEXT("Scalar");
		Scalar->DefaultValue = 1.0f;

		Output = NewObject<UMaterialExpressionFunctionOutput>(Function, NAME_None, RF_Transactional);
		Output->OutputName = TEXT("Result");
		Output->ConditionallyGenerateId(true);
		Output->A.Connect(0, Scalar);

		Function->GetExpressionCollection().AddExpression(Scalar);
		Function->GetExpressionCollection().AddExpression(Output);
		Package->SetDirtyFlag(false);
	}

	~FFunctionGraphFixture()
	{
		if (Function)
		{
			if (UMaterialFunctionEditorOnlyData* EditorData = Function->GetEditorOnlyData())
			{
				EditorData->MarkAsGarbage();
			}
			Function->MarkAsGarbage();
		}
		if (Scalar)
		{
			Scalar->MarkAsGarbage();
		}
		if (Output)
		{
			Output->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	TSharedPtr<FJsonObject> AssetParams() const
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Function->GetPathName());
		return Params;
	}

	TArray<TObjectPtr<UMaterialExpression>>& Expressions() const
	{
		return Function->GetExpressionCollection().Expressions;
	}
};

struct FFunctionInstanceGraphFixture
{
	FFunctionGraphFixture Base;
	UPackage* Package = nullptr;
	UMaterialFunctionInstance* Instance = nullptr;

	FFunctionInstanceGraphFixture()
		: Base(TEXT("InstanceBase"))
	{
		const FString InstanceName = TEXT("MFI_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
		Package = CreatePackage(*(TEXT("/Game/Temp/CortexMaterialFunctionTest/") + InstanceName));
		Instance = NewObject<UMaterialFunctionInstance>(Package, *InstanceName,
			RF_Public | RF_Standalone | RF_Transactional);
		Instance->SetParent(Base.Function);
		Instance->ScalarParameterValues.Add(
			FScalarParameterValue(FMaterialParameterInfo(FName(TEXT("Scalar"))), 4.5f));
		Package->SetDirtyFlag(false);
	}

	~FFunctionInstanceGraphFixture()
	{
		if (Instance)
		{
			Instance->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	TSharedPtr<FJsonObject> AssetParams() const
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Instance->GetPathName());
		return Params;
	}
};

bool IsFunctionNextUndo(const TCHAR* Title)
{
	return GEditor->Trans->GetUndoContext(true).Title.ToString().StartsWith(Title);
}

UMaterialExpression* FindFunctionExpression(UMaterialFunction* Function, const FString& Name)
{
	for (TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressionCollection().Expressions)
	{
		if (Expression && Expression->GetName() == Name)
		{
			return Expression;
		}
	}
	return nullptr;
}

const TSharedPtr<FJsonObject>* FindNode(const TArray<TSharedPtr<FJsonValue>>& Nodes, const FString& NodeId)
{
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry)->GetStringField(TEXT("node_id")) == NodeId)
		{
			return Entry;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionListNodesTest,
	"Cortex.Material.Graph.Function.ListNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionListNodesTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("ListNodes"));
	FCortexMaterialCommandHandler Handler;

	FCortexCommandResult Result = Handler.Execute(TEXT("list_nodes"), Fixture.AssetParams());
	if (!TestTrue(TEXT("list_nodes should accept a material function"), Result.bSuccess))
	{
		return false;
	}

	FString AssetKind;
	TestTrue(TEXT("asset_kind should be present"), Result.Data->TryGetStringField(TEXT("asset_kind"), AssetKind));
	TestEqual(TEXT("asset_kind should identify a material function"), AssetKind, TEXT("material_function"));

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("Should have nodes array"), Result.Data->TryGetArrayField(TEXT("nodes"), Nodes));
	if (!Nodes)
	{
		return false;
	}
	TestEqual(TEXT("Function expressions are the whole node list"), Nodes->Num(), Fixture.Expressions().Num());
	TestNull(TEXT("Functions have no virtual MaterialResult node"), FindNode(*Nodes, TEXT("MaterialResult")));

	const TSharedPtr<FJsonObject>* ScalarNode = FindNode(*Nodes, Fixture.Scalar->GetName());
	if (TestNotNull(TEXT("Scalar parameter node is listed"), ScalarNode))
	{
		FString ExpressionClass;
		(*ScalarNode)->TryGetStringField(TEXT("expression_class"), ExpressionClass);
		TestEqual(TEXT("Scalar parameter node class"), ExpressionClass, TEXT("MaterialExpressionScalarParameter"));
	}
	TestNotNull(TEXT("Function output node is listed"), FindNode(*Nodes, Fixture.Output->GetName()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionPinsAndConnectionsTest,
	"Cortex.Material.Graph.Function.GetNodePinsAndConnections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionPinsAndConnectionsTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Pins"));
	FCortexMaterialCommandHandler Handler;

	// Existing Scalar -> Output edge reports the reflected pin name A.
	FCortexCommandResult Connections = Handler.Execute(TEXT("list_connections"), Fixture.AssetParams());
	if (!TestTrue(TEXT("list_connections should accept a material function"), Connections.bSuccess))
	{
		return false;
	}
	FString AssetKind;
	Connections.Data->TryGetStringField(TEXT("asset_kind"), AssetKind);
	TestEqual(TEXT("list_connections asset_kind"), AssetKind, TEXT("material_function"));

	const TArray<TSharedPtr<FJsonValue>>* ConnectionArray = nullptr;
	TestTrue(TEXT("Should have connections array"), Connections.Data->TryGetArrayField(TEXT("connections"), ConnectionArray));
	if (!ConnectionArray)
	{
		return false;
	}
	bool bFoundOutputEdge = false;
	for (const TSharedPtr<FJsonValue>& Value : *ConnectionArray)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry)
		{
			continue;
		}
		FString SourceNode, TargetNode, TargetInput;
		(*Entry)->TryGetStringField(TEXT("source_node"), SourceNode);
		(*Entry)->TryGetStringField(TEXT("target_node"), TargetNode);
		(*Entry)->TryGetStringField(TEXT("target_input"), TargetInput);
		if (TargetNode == Fixture.Output->GetName())
		{
			bFoundOutputEdge = true;
			TestEqual(TEXT("Function output input is named A"), TargetInput, TEXT("A"));
			TestEqual(TEXT("Function output edge source"), SourceNode, Fixture.Scalar->GetName());
		}
	}
	TestTrue(TEXT("Scalar -> FunctionOutput edge is listed"), bFoundOutputEdge);

	FCortexCommandResult Pins = Handler.Execute(TEXT("get_node_pins"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), Fixture.Output->GetName());
		return Params;
	}());
	if (!TestTrue(TEXT("get_node_pins should accept a material function"), Pins.bSuccess))
	{
		return false;
	}
	Pins.Data->TryGetStringField(TEXT("asset_kind"), AssetKind);
	TestEqual(TEXT("get_node_pins asset_kind"), AssetKind, TEXT("material_function"));

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	TestTrue(TEXT("Function output has an inputs array"), Pins.Data->TryGetArrayField(TEXT("inputs"), Inputs));
	if (!Inputs || !TestEqual(TEXT("Function output has one input"), Inputs->Num(), 1))
	{
		return false;
	}
	FString InputName, ConnectedTo;
	(*Inputs)[0]->AsObject()->TryGetStringField(TEXT("name"), InputName);
	(*Inputs)[0]->AsObject()->TryGetStringField(TEXT("connected_to"), ConnectedTo);
	TestEqual(TEXT("Function output pin name is A"), InputName, TEXT("A"));
	TestEqual(TEXT("Function output connected source"), ConnectedTo, Fixture.Scalar->GetName());

	// get_node reads the function expression's properties and asset kind.
	FCortexCommandResult Node = Handler.Execute(TEXT("get_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), Fixture.Scalar->GetName());
		return Params;
	}());
	if (!TestTrue(TEXT("get_node should accept a material function"), Node.bSuccess))
	{
		return false;
	}
	Node.Data->TryGetStringField(TEXT("asset_kind"), AssetKind);
	TestEqual(TEXT("get_node asset_kind"), AssetKind, TEXT("material_function"));
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	TestTrue(TEXT("Node properties exist"), Node.Data->TryGetObjectField(TEXT("properties"), Properties));
	if (Properties)
	{
		FString ParameterName;
		(*Properties)->TryGetStringField(TEXT("ParameterName"), ParameterName);
		TestEqual(TEXT("Scalar node parameter name"), ParameterName, TEXT("Scalar"));
	}

	// connect resolves the reflected A pin on a function output.
	FCortexCommandResult Added = Handler.Execute(TEXT("add_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionConstant"));
		return Params;
	}());
	if (!TestTrue(TEXT("add_node should accept a material function"), Added.bSuccess))
	{
		return false;
	}
	FString ConstantNode;
	Added.Data->TryGetStringField(TEXT("node_id"), ConstantNode);

	FCortexCommandResult Connected = Handler.Execute(TEXT("connect"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("source_node"), ConstantNode);
		Params->SetNumberField(TEXT("source_output"), 0);
		Params->SetStringField(TEXT("target_node"), Fixture.Output->GetName());
		Params->SetStringField(TEXT("target_input"), TEXT("A"));
		return Params;
	}());
	TestTrue(TEXT("connect should reach the function output pin A"), Connected.bSuccess);
	TestTrue(TEXT("Function output input now points at the constant"),
		Fixture.Output->A.Expression && Fixture.Output->A.Expression->GetName() == ConstantNode);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionWriteAndRecompileTest,
	"Cortex.Material.Graph.Function.WriteAndRecompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionWriteAndRecompileTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Write"));
	FCortexMaterialCommandHandler Handler;
	const FGuid StateBefore = Fixture.Function->StateId;

	FCortexCommandResult Added = Handler.Execute(TEXT("add_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("expression_class"), TEXT("Constant"));
		return Params;
	}());
	if (!TestTrue(TEXT("add_node should accept a material function"), Added.bSuccess))
	{
		return false;
	}
	FString ConstantNode;
	Added.Data->TryGetStringField(TEXT("node_id"), ConstantNode);
	TestEqual(TEXT("add_node adds one expression"), Fixture.Expressions().Num(), 3);
	TestNotEqual(TEXT("add_node recompiles the function"), Fixture.Function->StateId, StateBefore);

	UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(
		FindFunctionExpression(Fixture.Function, ConstantNode));
	if (!TestNotNull(TEXT("Added constant is in the function collection"), Constant))
	{
		return false;
	}

	FCortexCommandResult Property = Handler.Execute(TEXT("set_node_property"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), ConstantNode);
		Params->SetStringField(TEXT("property_name"), TEXT("R"));
		Params->SetNumberField(TEXT("value"), 0.25);
		return Params;
	}());
	TestTrue(TEXT("set_node_property should accept a material function"), Property.bSuccess);
	TestEqual(TEXT("Constant.R is written"), Constant->R, 0.25f);

	FCortexCommandResult InputAdded = Handler.Execute(TEXT("add_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionFunctionInput"));
		return Params;
	}());
	if (!TestTrue(TEXT("Function input node can be added"), InputAdded.bSuccess))
	{
		return false;
	}
	FString InputNode;
	InputAdded.Data->TryGetStringField(TEXT("node_id"), InputNode);
	UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(
		FindFunctionExpression(Fixture.Function, InputNode));
	if (TestNotNull(TEXT("Added function input is in the collection"), FunctionInput))
	{
		TestTrue(TEXT("New function input has a generated id"), FunctionInput->Id.IsValid());
	}

	const FGuid StateAfterWrites = Fixture.Function->StateId;
	FCortexCommandResult Removed = Handler.Execute(TEXT("remove_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), ConstantNode);
		return Params;
	}());
	TestTrue(TEXT("remove_node should accept a material function"), Removed.bSuccess);
	TestEqual(TEXT("remove_node drops one expression"), Fixture.Expressions().Num(), 3);
	TestNotEqual(TEXT("remove_node recompiles the function"), Fixture.Function->StateId, StateAfterWrites);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionDisconnectTest,
	"Cortex.Material.Graph.Function.Disconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionDisconnectTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Disconnect"));
	FCortexMaterialCommandHandler Handler;
	TestTrue(TEXT("Fixture output starts connected"), Fixture.Output->A.Expression == Fixture.Scalar);

	FCortexCommandResult Result = Handler.Execute(TEXT("disconnect"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("target_node"), Fixture.Output->GetName());
		Params->SetStringField(TEXT("target_input"), TEXT("A"));
		return Params;
	}());
	if (!TestTrue(TEXT("disconnect should accept expression inputs on a function"), Result.bSuccess))
	{
		return false;
	}
	TestNull(TEXT("Function output input is cleared"), Fixture.Output->A.Expression);

	FCortexCommandResult Connections = Handler.Execute(TEXT("list_connections"), Fixture.AssetParams());
	const TArray<TSharedPtr<FJsonValue>>* ConnectionArray = nullptr;
	Connections.Data->TryGetArrayField(TEXT("connections"), ConnectionArray);
	TestTrue(TEXT("No expression edges remain"),
		ConnectionArray && ConnectionArray->Num() == 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionAutoLayoutTest,
	"Cortex.Material.Graph.Function.AutoLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionAutoLayoutTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Layout"));
	FCortexMaterialCommandHandler Handler;
	Fixture.Scalar->MaterialExpressionEditorX = 0;
	Fixture.Scalar->MaterialExpressionEditorY = 0;
	Fixture.Output->MaterialExpressionEditorX = 0;
	Fixture.Output->MaterialExpressionEditorY = 0;

	FCortexCommandResult Result = Handler.Execute(TEXT("auto_layout"), Fixture.AssetParams());
	if (!TestTrue(TEXT("auto_layout should accept a material function"), Result.bSuccess))
	{
		return false;
	}

	double NodeCount = 0;
	Result.Data->TryGetNumberField(TEXT("node_count"), NodeCount);
	TestEqual(TEXT("auto_layout reports the function expressions"), static_cast<int32>(NodeCount),
		Fixture.Expressions().Num());
	TestTrue(TEXT("Layout places the source in the first column"), Fixture.Scalar->MaterialExpressionEditorX >= 0);
	TestTrue(TEXT("Sink is right of its source"),
		Fixture.Output->MaterialExpressionEditorX > Fixture.Scalar->MaterialExpressionEditorX);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionInstanceReadOnlyTest,
	"Cortex.Material.Graph.Function.Instance.ReadOnlyAndOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionInstanceReadOnlyTest::RunTest(const FString& Parameters)
{
	FFunctionInstanceGraphFixture Fixture;
	FCortexMaterialCommandHandler Handler;

	FCortexCommandResult Nodes = Handler.Execute(TEXT("list_nodes"), Fixture.AssetParams());
	if (!TestTrue(TEXT("list_nodes should accept a material function instance"), Nodes.bSuccess))
	{
		return false;
	}
	FString AssetKind;
	Nodes.Data->TryGetStringField(TEXT("asset_kind"), AssetKind);
	TestEqual(TEXT("list_nodes asset_kind"), AssetKind, TEXT("material_function_instance"));
	const TArray<TSharedPtr<FJsonValue>>* NodeArray = nullptr;
	Nodes.Data->TryGetArrayField(TEXT("nodes"), NodeArray);
	TestEqual(TEXT("Instance reads the base function graph"),
		NodeArray ? NodeArray->Num() : 0, Fixture.Base.Expressions().Num());

	FCortexCommandResult Node = Handler.Execute(TEXT("get_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), Fixture.Base.Scalar->GetName());
		return Params;
	}());
	if (!TestTrue(TEXT("get_node should accept a material function instance"), Node.bSuccess))
	{
		return false;
	}
	Node.Data->TryGetStringField(TEXT("asset_kind"), AssetKind);
	TestEqual(TEXT("get_node asset_kind"), AssetKind, TEXT("material_function_instance"));
	const TSharedPtr<FJsonObject>* Override = nullptr;
	if (!TestTrue(TEXT("get_node reports the scalar override"),
		Node.Data->TryGetObjectField(TEXT("parameter_override"), Override)))
	{
		return false;
	}
	FString OverrideName, OverrideType;
	double OverrideValue = 0;
	(*Override)->TryGetStringField(TEXT("name"), OverrideName);
	(*Override)->TryGetStringField(TEXT("type"), OverrideType);
	(*Override)->TryGetNumberField(TEXT("value"), OverrideValue);
	TestEqual(TEXT("Override name"), OverrideName, TEXT("Scalar"));
	TestEqual(TEXT("Override type"), OverrideType, TEXT("scalar"));
	TestEqual(TEXT("Override value"), OverrideValue, 4.5);

	const int32 ExpressionCount = Fixture.Base.Expressions().Num();
	FCortexCommandResult Add = Handler.Execute(TEXT("add_node"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("expression_class"), TEXT("Constant"));
		return Params;
	}());
	TestFalse(TEXT("add_node refuses a function instance"), Add.bSuccess);
	TestEqual(TEXT("Instance write refusal code"), Add.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("Refused write leaves the base graph untouched"), Fixture.Base.Expressions().Num(), ExpressionCount);

	FCortexCommandResult Disconnect = Handler.Execute(TEXT("disconnect"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("target_node"), Fixture.Base.Output->GetName());
		Params->SetStringField(TEXT("target_input"), TEXT("A"));
		return Params;
	}());
	TestFalse(TEXT("disconnect refuses a function instance"), Disconnect.bSuccess);
	TestEqual(TEXT("Instance disconnect refusal code"), Disconnect.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("Refused disconnect leaves the base link"), Fixture.Base.Output->A.Expression == Fixture.Base.Scalar);

	FCortexCommandResult Property = Handler.Execute(TEXT("set_node_property"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), Fixture.Base.Scalar->GetName());
		Params->SetStringField(TEXT("property_name"), TEXT("DefaultValue"));
		Params->SetNumberField(TEXT("value"), 2.0);
		return Params;
	}());
	TestFalse(TEXT("set_node_property refuses a function instance"), Property.bSuccess);
	TestEqual(TEXT("Instance property refusal code"), Property.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("Refused property leaves the base value"), Fixture.Base.Scalar->DefaultValue, 1.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionMaterialOnlyOpsTest,
	"Cortex.Material.Graph.Function.MaterialOnlyOperationsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionMaterialOnlyOpsTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("MaterialOnly"));
	FCortexMaterialCommandHandler Handler;

	FCortexCommandResult SetInput = Handler.Execute(TEXT("set_material_input"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("input"), TEXT("BaseColor"));
		Params->SetNumberField(TEXT("mask"), 1);
		return Params;
	}());
	TestFalse(TEXT("set_material_input refuses a material function"), SetInput.bSuccess);
	TestEqual(TEXT("set_material_input refusal code"), SetInput.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("Function graph remains unchanged"), Fixture.Expressions().Num(), 2);

	FCortexCommandResult InputState = Handler.Execute(TEXT("list_connections"), [&]
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetBoolField(TEXT("include_input_state"), true);
		return Params;
	}());
	TestFalse(TEXT("include_input_state refuses a material function"), InputState.bSuccess);
	TestEqual(TEXT("include_input_state refusal code"), InputState.ErrorCode, CortexErrorCodes::InvalidOperation);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionUndoAddNodeTest,
	"Cortex.Material.Graph.Function.UndoAddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionUndoAddNodeTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Undo"));
	FCortexMaterialCommandHandler Handler;
	const int32 Before = Fixture.Expressions().Num();

	TSharedPtr<FJsonObject> AddParams = Fixture.AssetParams();
	AddParams->SetStringField(TEXT("expression_class"), TEXT("Constant"));
	TestTrue(TEXT("add_node succeeds"), Handler.Execute(TEXT("add_node"), AddParams).bSuccess);
	TestEqual(TEXT("add_node adds one expression"), Fixture.Expressions().Num(), Before + 1);
	if (!TestTrue(TEXT("add_node is the next undo step"),
		IsFunctionNextUndo(TEXT("Cortex: Add Material Expression Node"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo add_node"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo removes the added expression"), Fixture.Expressions().Num(), Before);
	TestTrue(TEXT("Redo add_node"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restores the added expression"), Fixture.Expressions().Num(), Before + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionRemoveNodeBreaksLinksTest,
	"Cortex.Material.Graph.Function.RemoveNodeBreaksLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionRemoveNodeBreaksLinksTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Remove"));
	FCortexMaterialCommandHandler Handler;
	TestTrue(TEXT("Fixture output starts connected"), Fixture.Output->A.Expression == Fixture.Scalar);

	TSharedPtr<FJsonObject> RemoveParams = Fixture.AssetParams();
	RemoveParams->SetStringField(TEXT("node_id"), Fixture.Scalar->GetName());
	FCortexCommandResult Result = Handler.Execute(TEXT("remove_node"), RemoveParams);
	if (!TestTrue(TEXT("remove_node should accept a material function"), Result.bSuccess))
	{
		return false;
	}
	TestNull(TEXT("Removing the source clears the function output input"), Fixture.Output->A.Expression);
	TestEqual(TEXT("Only the function output remains"), Fixture.Expressions().Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialFunctionUniqueSignatureNamesTest,
	"Cortex.Material.Graph.Function.UniqueSignatureNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialFunctionUniqueSignatureNamesTest::RunTest(const FString& Parameters)
{
	FFunctionGraphFixture Fixture(TEXT("Names"));
	FCortexMaterialCommandHandler Handler;

	auto AddNode = [&Fixture, &Handler](const TCHAR* ClassName) -> FString
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("expression_class"), ClassName);
		FCortexCommandResult Result = Handler.Execute(TEXT("add_node"), Params);
		FString NodeId;
		if (Result.bSuccess && Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}
		return NodeId;
	};
	auto Rename = [&Fixture, &Handler](const FString& NodeId, const TCHAR* Property, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Params = Fixture.AssetParams();
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetStringField(TEXT("property_name"), Property);
		Params->SetStringField(TEXT("value"), Value);
		return Handler.Execute(TEXT("set_node_property"), Params);
	};

	const FString Output2 = AddNode(TEXT("FunctionOutput"));
	auto* OutputExpression = Cast<UMaterialExpressionFunctionOutput>(
		FindFunctionExpression(Fixture.Function, Output2));
	if (!TestNotNull(TEXT("Second function output exists"), OutputExpression))
	{
		return false;
	}
	TestEqual(TEXT("Second output gets the editor suffix"),
		OutputExpression->OutputName.ToString(), TEXT("Result_1"));

	const FString Input1 = AddNode(TEXT("FunctionInput"));
	const FString Input2 = AddNode(TEXT("FunctionInput"));
	auto* InputExpression1 = Cast<UMaterialExpressionFunctionInput>(
		FindFunctionExpression(Fixture.Function, Input1));
	auto* InputExpression2 = Cast<UMaterialExpressionFunctionInput>(
		FindFunctionExpression(Fixture.Function, Input2));
	if (!TestNotNull(TEXT("First function input exists"), InputExpression1)
		|| !TestNotNull(TEXT("Second function input exists"), InputExpression2))
	{
		return false;
	}
	TestEqual(TEXT("First input keeps the default name"),
		InputExpression1->InputName.ToString(), TEXT("In"));
	TestEqual(TEXT("Second input gets the editor suffix"),
		InputExpression2->InputName.ToString(), TEXT("In_1"));

	FCortexCommandResult DuplicateOutput = Rename(Output2, TEXT("OutputName"), TEXT("Result"));
	TestFalse(TEXT("Duplicate output rename is refused"), DuplicateOutput.bSuccess);
	TestEqual(TEXT("Duplicate output rename code"), DuplicateOutput.ErrorCode, CortexErrorCodes::InvalidValue);
	TestEqual(TEXT("Refused output rename leaves the name"), OutputExpression->OutputName.ToString(), TEXT("Result_1"));

	FCortexCommandResult DuplicateInput = Rename(Input2, TEXT("InputName"), TEXT("In"));
	TestFalse(TEXT("Duplicate input rename is refused"), DuplicateInput.bSuccess);
	TestEqual(TEXT("Duplicate input rename code"), DuplicateInput.ErrorCode, CortexErrorCodes::InvalidValue);
	TestEqual(TEXT("Refused input rename leaves the name"), InputExpression2->InputName.ToString(), TEXT("In_1"));

	return true;
}
