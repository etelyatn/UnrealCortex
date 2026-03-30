#include "CortexMaterialCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "Operations/CortexMaterialParamOps.h"
#include "Operations/CortexMaterialGraphOps.h"
#include "Operations/CortexMaterialCollectionOps.h"
#include "Operations/CortexMaterialDynamicOps.h"

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
	if (Command == TEXT("set_material_property"))
		return FCortexMaterialAssetOps::SetMaterialProperty(Params);
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

	// Dynamic material instance operations (PIE required)
	if (Command == TEXT("list_dynamic_instances"))
		return FCortexMaterialDynamicOps::ListDynamicInstances(Params);
	if (Command == TEXT("get_dynamic_instance"))
		return FCortexMaterialDynamicOps::GetDynamicInstance(Params);
	if (Command == TEXT("create_dynamic_instance"))
		return FCortexMaterialDynamicOps::CreateDynamicInstance(Params);
	if (Command == TEXT("destroy_dynamic_instance"))
		return FCortexMaterialDynamicOps::DestroyDynamicInstance(Params);
	if (Command == TEXT("set_dynamic_parameter"))
		return FCortexMaterialDynamicOps::SetDynamicParameter(Params);
	if (Command == TEXT("get_dynamic_parameter"))
		return FCortexMaterialDynamicOps::GetDynamicParameter(Params);
	if (Command == TEXT("list_dynamic_parameters"))
		return FCortexMaterialDynamicOps::ListDynamicParameters(Params);
	if (Command == TEXT("set_dynamic_parameters"))
		return FCortexMaterialDynamicOps::SetDynamicParameters(Params);
	if (Command == TEXT("reset_dynamic_parameter"))
		return FCortexMaterialDynamicOps::ResetDynamicParameter(Params);

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown material command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexMaterialCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("list_materials"), TEXT("List all materials in a path") }
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to search"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Search subdirectories recursively")),
		FCortexCommandInfo{ TEXT("get_material"), TEXT("Get material details") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
		FCortexCommandInfo{ TEXT("create_material"), TEXT("Create a new UMaterial") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Destination content path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Material asset name")),
		FCortexCommandInfo{ TEXT("delete_material"), TEXT("Delete a material asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
		FCortexCommandInfo{ TEXT("set_material_property"), TEXT("Set a property on a UMaterial asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("UMaterial property name"))
			.Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
		FCortexCommandInfo{ TEXT("list_instances"), TEXT("List material instances") }
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to search"))
			.Optional(TEXT("parent_material"), TEXT("string"), TEXT("Optional parent material filter")),
		FCortexCommandInfo{ TEXT("get_instance"), TEXT("Get instance details with overrides") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path")),
		FCortexCommandInfo{ TEXT("create_instance"), TEXT("Create a UMaterialInstanceConstant") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Destination content path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Material instance asset name"))
			.Required(TEXT("parent_material"), TEXT("string"), TEXT("Parent material asset path (alias: parent)")),
		FCortexCommandInfo{ TEXT("delete_instance"), TEXT("Delete a material instance") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path")),
		FCortexCommandInfo{ TEXT("list_parameters"), TEXT("List all parameters on a material or instance") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material or instance asset path")),
		FCortexCommandInfo{ TEXT("get_parameter"), TEXT("Get parameter value and metadata") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name")),
		FCortexCommandInfo{ TEXT("set_parameter"), TEXT("Set parameter value on an instance") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name (alias: name)"))
			.Optional(TEXT("parameter_type"), TEXT("string"), TEXT("Parameter type (auto-detected if omitted)"))
			.Required(TEXT("value"), TEXT("object"), TEXT("Parameter value")),
		FCortexCommandInfo{ TEXT("set_parameters"), TEXT("Batch set multiple parameters") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameters"), TEXT("array"), TEXT("Parameter updates")),
		FCortexCommandInfo{ TEXT("reset_parameter"), TEXT("Reset instance override to parent value") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name")),
		FCortexCommandInfo{ TEXT("list_nodes"), TEXT("List material expression nodes") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
		FCortexCommandInfo{ TEXT("get_node"), TEXT("Get node details by ID") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node identifier")),
		FCortexCommandInfo{ TEXT("add_node"), TEXT("Add expression node to material graph") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_class"), TEXT("string"), TEXT("Expression class name (short names like VectorParameter accepted)"))
			.Optional(TEXT("position"), TEXT("object"), TEXT("Optional node position")),
		FCortexCommandInfo{ TEXT("remove_node"), TEXT("Remove expression node from material") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node identifier")),
		FCortexCommandInfo{ TEXT("list_connections"), TEXT("List all node connections in material") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
		FCortexCommandInfo{ TEXT("connect"), TEXT("Connect nodes in material graph") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("source_node"), TEXT("string"), TEXT("Source node identifier"))
			.Required(TEXT("source_output"), TEXT("number"), TEXT("Source output index"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Target node identifier"))
			.Required(TEXT("target_input"), TEXT("string"), TEXT("Target input pin name (e.g. 'BaseColor', 'A', 'Input')")),
		FCortexCommandInfo{ TEXT("disconnect"), TEXT("Disconnect nodes in material graph") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Target node identifier"))
			.Required(TEXT("target_input"), TEXT("string"), TEXT("Target input pin name (e.g. 'BaseColor', 'A', 'Input')")),
		FCortexCommandInfo{ TEXT("auto_layout"), TEXT("Auto-layout material graph nodes by connection topology") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path")),
		FCortexCommandInfo{ TEXT("set_node_property"), TEXT("Set property value on material expression node") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node identifier"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Expression property name (alias: property)"))
			.Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
		FCortexCommandInfo{ TEXT("get_node_pins"), TEXT("Get input and output pin names for a material expression node") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node identifier")),
		FCortexCommandInfo{ TEXT("list_collections"), TEXT("List material parameter collections") }
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to search"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Search subdirectories recursively")),
		FCortexCommandInfo{ TEXT("get_collection"), TEXT("Get collection with parameters") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Collection asset path")),
		FCortexCommandInfo{ TEXT("create_collection"), TEXT("Create a material parameter collection") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Destination content path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Collection asset name")),
		FCortexCommandInfo{ TEXT("delete_collection"), TEXT("Delete a material parameter collection") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Collection asset path")),
		FCortexCommandInfo{ TEXT("add_collection_parameter"), TEXT("Add parameter to collection") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Collection asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("parameter_type"), TEXT("string"), TEXT("Parameter type"))
			.Required(TEXT("default_value"), TEXT("object"), TEXT("Default parameter value")),
		FCortexCommandInfo{ TEXT("remove_collection_parameter"), TEXT("Remove parameter from collection") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Collection asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name")),
		FCortexCommandInfo{ TEXT("set_collection_parameter"), TEXT("Set collection parameter value") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Collection asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("value"), TEXT("object"), TEXT("Parameter value")),
		FCortexCommandInfo{ TEXT("list_dynamic_instances"), TEXT("List material slots and DMI status on a PIE actor") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path")),
		FCortexCommandInfo{ TEXT("get_dynamic_instance"), TEXT("Get DMI details with all parameters") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("create_dynamic_instance"), TEXT("Create a Dynamic Material Instance on a PIE actor slot") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index"))
			.Optional(TEXT("source_material"), TEXT("string"), TEXT("Optional source material override"))
			.Optional(TEXT("parameters"), TEXT("array"), TEXT("Initial DMI parameter overrides")),
		FCortexCommandInfo{ TEXT("destroy_dynamic_instance"), TEXT("Remove a DMI and revert to parent material") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("set_dynamic_parameter"), TEXT("Set parameter on a DMI at runtime") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("type"), TEXT("string"), TEXT("Parameter type"))
			.Required(TEXT("value"), TEXT("object"), TEXT("Parameter value"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("get_dynamic_parameter"), TEXT("Get single parameter from a DMI") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("list_dynamic_parameters"), TEXT("List all overrideable parameters on a DMI") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("set_dynamic_parameters"), TEXT("Batch set multiple parameters on a DMI") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Required(TEXT("parameters"), TEXT("array"), TEXT("Parameter updates"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
		FCortexCommandInfo{ TEXT("reset_dynamic_parameter"), TEXT("Reset a DMI parameter to parent default") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("PIE actor path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Target component name"))
			.Optional(TEXT("slot_index"), TEXT("number"), TEXT("Material slot index")),
	};
}
