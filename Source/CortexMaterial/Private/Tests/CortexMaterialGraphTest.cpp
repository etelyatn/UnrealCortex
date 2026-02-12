#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialAddNodeTest,
	"Cortex.Material.Graph.AddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialAddNodeTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestGraph_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Graph_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_material"), CreateParams);
	TestTrue(TEXT("Material creation should succeed"), CreateResult.bSuccess);

	// Add a scalar parameter node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	TestTrue(TEXT("add_node should succeed"), AddResult.bSuccess);

	if (AddResult.Data.IsValid())
	{
		FString NodeId;
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
		TestFalse(TEXT("node_id should be populated"), NodeId.IsEmpty());

		FString ExprClass;
		AddResult.Data->TryGetStringField(TEXT("expression_class"), ExprClass);
		TestEqual(TEXT("expression_class should match"), ExprClass, TEXT("MaterialExpressionScalarParameter"));
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListNodesTest,
	"Cortex.Material.Graph.ListNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListNodesTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestList_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_List_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Add a node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	Handler.Execute(TEXT("add_node"), AddParams);

	// List nodes
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("list_nodes"), ListParams);

	TestTrue(TEXT("list_nodes should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		TestTrue(TEXT("Should have nodes array"),
			Result.Data->TryGetArrayField(TEXT("nodes"), NodesArray));

		if (NodesArray)
		{
			TestTrue(TEXT("Should have at least 1 node"), NodesArray->Num() >= 1);
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetNodeTest,
	"Cortex.Material.Graph.GetNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetNodeTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestGetNode_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_GetNode_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material and add node
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	// Get node
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), MatPath);
	GetParams->SetStringField(TEXT("node_id"), NodeId);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_node"), GetParams);

	TestTrue(TEXT("get_node should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString ResultNodeId;
		Result.Data->TryGetStringField(TEXT("node_id"), ResultNodeId);
		TestEqual(TEXT("node_id should match"), ResultNodeId, NodeId);

		FString ExprClass;
		Result.Data->TryGetStringField(TEXT("expression_class"), ExprClass);
		TestFalse(TEXT("expression_class should be populated"), ExprClass.IsEmpty());
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialRemoveNodeTest,
	"Cortex.Material.Graph.RemoveNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialRemoveNodeTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestRemove_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Remove_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material and add node
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	// List nodes before removal
	TSharedPtr<FJsonObject> ListParams1 = MakeShared<FJsonObject>();
	ListParams1->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult ListResult1 = Handler.Execute(TEXT("list_nodes"), ListParams1);

	int32 CountBefore = 0;
	if (ListResult1.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (ListResult1.Data->TryGetArrayField(TEXT("nodes"), NodesArray))
		{
			CountBefore = NodesArray->Num();
		}
	}

	// Remove node
	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), MatPath);
	RemoveParams->SetStringField(TEXT("node_id"), NodeId);
	FCortexCommandResult RemoveResult = Handler.Execute(TEXT("remove_node"), RemoveParams);

	TestTrue(TEXT("remove_node should succeed"), RemoveResult.bSuccess);

	// List nodes after removal
	TSharedPtr<FJsonObject> ListParams2 = MakeShared<FJsonObject>();
	ListParams2->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult ListResult2 = Handler.Execute(TEXT("list_nodes"), ListParams2);

	int32 CountAfter = 0;
	if (ListResult2.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (ListResult2.Data->TryGetArrayField(TEXT("nodes"), NodesArray))
		{
			CountAfter = NodesArray->Num();
		}
	}

	TestTrue(TEXT("Node count should decrease"), CountAfter < CountBefore);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetNodeNotFoundTest,
	"Cortex.Material.Graph.GetNode.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetNodeNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestNotFound_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_NotFound_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Try to get non-existent node
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), MatPath);
	GetParams->SetStringField(TEXT("node_id"), TEXT("NonExistentNode_12345"));
	FCortexCommandResult Result = Handler.Execute(TEXT("get_node"), GetParams);

	TestFalse(TEXT("Should fail for non-existent node"), Result.bSuccess);
	TestEqual(TEXT("Error code should be NODE_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::NodeNotFound);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialConnectTest,
	"Cortex.Material.Graph.Connect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialConnectTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestConnect_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Connect_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Add a scalar parameter node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	// Connect to MaterialResult Roughness input
	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), MatPath);
	ConnectParams->SetStringField(TEXT("source_node"), NodeId);
	ConnectParams->SetNumberField(TEXT("source_output"), 0);
	ConnectParams->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
	ConnectParams->SetStringField(TEXT("target_input"), TEXT("Roughness"));
	FCortexCommandResult Result = Handler.Execute(TEXT("connect"), ConnectParams);

	TestTrue(TEXT("connect should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		bool bConnected = false;
		Result.Data->TryGetBoolField(TEXT("connected"), bConnected);
		TestTrue(TEXT("connected should be true"), bConnected);
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListConnectionsTest,
	"Cortex.Material.Graph.ListConnections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListConnectionsTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestListConn_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_ListConn_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material and add node
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	// Connect to MaterialResult
	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), MatPath);
	ConnectParams->SetStringField(TEXT("source_node"), NodeId);
	ConnectParams->SetNumberField(TEXT("source_output"), 0);
	ConnectParams->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
	ConnectParams->SetStringField(TEXT("target_input"), TEXT("Roughness"));
	Handler.Execute(TEXT("connect"), ConnectParams);

	// List connections
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("list_connections"), ListParams);

	TestTrue(TEXT("list_connections should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
		TestTrue(TEXT("Should have connections array"),
			Result.Data->TryGetArrayField(TEXT("connections"), ConnectionsArray));

		if (ConnectionsArray)
		{
			TestTrue(TEXT("Should have at least 1 connection"), ConnectionsArray->Num() >= 1);
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDisconnectTest,
	"Cortex.Material.Graph.Disconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDisconnectTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestDisconnect_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Disconnect_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material, add node, and connect
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionScalarParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("asset_path"), MatPath);
	ConnectParams->SetStringField(TEXT("source_node"), NodeId);
	ConnectParams->SetNumberField(TEXT("source_output"), 0);
	ConnectParams->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
	ConnectParams->SetStringField(TEXT("target_input"), TEXT("Roughness"));
	Handler.Execute(TEXT("connect"), ConnectParams);

	// Disconnect
	TSharedPtr<FJsonObject> DisconnectParams = MakeShared<FJsonObject>();
	DisconnectParams->SetStringField(TEXT("asset_path"), MatPath);
	DisconnectParams->SetStringField(TEXT("target_node"), TEXT("MaterialResult"));
	DisconnectParams->SetStringField(TEXT("target_input"), TEXT("Roughness"));
	FCortexCommandResult Result = Handler.Execute(TEXT("disconnect"), DisconnectParams);

	TestTrue(TEXT("disconnect should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		bool bDisconnected = false;
		Result.Data->TryGetBoolField(TEXT("disconnected"), bDisconnected);
		TestTrue(TEXT("disconnected should be true"), bDisconnected);
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}
