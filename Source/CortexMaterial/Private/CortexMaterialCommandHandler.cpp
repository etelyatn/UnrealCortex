#include "CortexMaterialCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "Operations/CortexMaterialParamOps.h"
#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialCollectionOps.h"

FCortexCommandResult FCortexMaterialCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	// Asset operations
	if (Command == TEXT("list_materials"))
		return FCortexMaterialAssetOps::ListMaterials(Params);
	if (Command == TEXT("get_material"))
		return FCortexMaterialAssetOps::GetMaterial(Params);
	if (Command == TEXT("create_material"))
		return FCortexMaterialAssetOps::CreateMaterial(Params);
	if (Command == TEXT("delete_material"))
		return FCortexMaterialAssetOps::DeleteMaterial(Params);
	if (Command == TEXT("list_instances"))
		return FCortexMaterialAssetOps::ListInstances(Params);
	if (Command == TEXT("get_instance"))
		return FCortexMaterialAssetOps::GetInstance(Params);
	if (Command == TEXT("create_instance"))
		return FCortexMaterialAssetOps::CreateInstance(Params);
	if (Command == TEXT("delete_instance"))
		return FCortexMaterialAssetOps::DeleteInstance(Params);

	// Parameter operations
	if (Command == TEXT("list_parameters"))
		return FCortexMaterialParamOps::ListParameters(Params);
	if (Command == TEXT("get_parameter"))
		return FCortexMaterialParamOps::GetParameter(Params);
	if (Command == TEXT("set_parameter"))
		return FCortexMaterialParamOps::SetParameter(Params);
	if (Command == TEXT("set_parameters"))
		return FCortexMaterialParamOps::SetParameters(Params);
	if (Command == TEXT("reset_parameter"))
		return FCortexMaterialParamOps::ResetParameter(Params);

	// Graph operations
	if (Command == TEXT("list_nodes"))
		return FCortexMaterialGraphOps::ListNodes(Params);
	if (Command == TEXT("get_node"))
		return FCortexMaterialGraphOps::GetNode(Params);
	if (Command == TEXT("add_node"))
		return FCortexMaterialGraphOps::AddNode(Params);
	if (Command == TEXT("remove_node"))
		return FCortexMaterialGraphOps::RemoveNode(Params);
	if (Command == TEXT("list_connections"))
		return FCortexMaterialGraphOps::ListConnections(Params);
	if (Command == TEXT("connect"))
		return FCortexMaterialGraphOps::Connect(Params);
	if (Command == TEXT("disconnect"))
		return FCortexMaterialGraphOps::Disconnect(Params);
	if (Command == TEXT("auto_layout"))
		return FCortexMaterialGraphOps::AutoLayout(Params);
	if (Command == TEXT("set_node_property"))
		return FCortexMaterialGraphOps::SetNodeProperty(Params);
	if (Command == TEXT("get_node_pins"))
		return FCortexMaterialGraphOps::GetNodePins(Params);

	// Collection operations
	if (Command == TEXT("list_collections"))
		return FCortexMaterialCollectionOps::ListCollections(Params);
	if (Command == TEXT("get_collection"))
		return FCortexMaterialCollectionOps::GetCollection(Params);
	if (Command == TEXT("create_collection"))
		return FCortexMaterialCollectionOps::CreateCollection(Params);
	if (Command == TEXT("delete_collection"))
		return FCortexMaterialCollectionOps::DeleteCollection(Params);
	if (Command == TEXT("add_collection_parameter"))
		return FCortexMaterialCollectionOps::AddCollectionParameter(Params);
	if (Command == TEXT("remove_collection_parameter"))
		return FCortexMaterialCollectionOps::RemoveCollectionParameter(Params);
	if (Command == TEXT("set_collection_parameter"))
		return FCortexMaterialCollectionOps::SetCollectionParameter(Params);

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown material command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexMaterialCommandHandler::GetSupportedCommands() const
{
	return {
		{ TEXT("list_materials"), TEXT("List all materials in a path") },
		{ TEXT("get_material"), TEXT("Get material details") },
		{ TEXT("create_material"), TEXT("Create a new UMaterial") },
		{ TEXT("delete_material"), TEXT("Delete a material asset") },
		{ TEXT("list_instances"), TEXT("List material instances") },
		{ TEXT("get_instance"), TEXT("Get instance details with overrides") },
		{ TEXT("create_instance"), TEXT("Create a UMaterialInstanceConstant") },
		{ TEXT("delete_instance"), TEXT("Delete a material instance") },
		{ TEXT("list_parameters"), TEXT("List all parameters on a material or instance") },
		{ TEXT("get_parameter"), TEXT("Get parameter value and metadata") },
		{ TEXT("set_parameter"), TEXT("Set parameter value on an instance") },
		{ TEXT("set_parameters"), TEXT("Batch set multiple parameters") },
		{ TEXT("reset_parameter"), TEXT("Reset instance override to parent value") },
		{ TEXT("list_nodes"), TEXT("List material expression nodes") },
		{ TEXT("get_node"), TEXT("Get node details by ID") },
		{ TEXT("add_node"), TEXT("Add expression node to material graph") },
		{ TEXT("remove_node"), TEXT("Remove expression node from material") },
		{ TEXT("list_connections"), TEXT("List all node connections in material") },
		{ TEXT("connect"), TEXT("Connect nodes in material graph") },
		{ TEXT("disconnect"), TEXT("Disconnect nodes in material graph") },
		{ TEXT("auto_layout"), TEXT("Auto-layout material graph nodes by connection topology") },
		{ TEXT("set_node_property"), TEXT("Set property value on material expression node") },
		{ TEXT("get_node_pins"), TEXT("Get input and output pin names for a material expression node") },
		{ TEXT("list_collections"), TEXT("List material parameter collections") },
		{ TEXT("get_collection"), TEXT("Get collection with parameters") },
		{ TEXT("create_collection"), TEXT("Create a material parameter collection") },
		{ TEXT("delete_collection"), TEXT("Delete a material parameter collection") },
		{ TEXT("add_collection_parameter"), TEXT("Add parameter to collection") },
		{ TEXT("remove_collection_parameter"), TEXT("Remove parameter from collection") },
		{ TEXT("set_collection_parameter"), TEXT("Set collection parameter value") },
	};
}
