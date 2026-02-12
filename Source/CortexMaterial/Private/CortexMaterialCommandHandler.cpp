#include "CortexMaterialCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexMaterialAssetOps.h"

FCortexCommandResult FCortexMaterialCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params)
{
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
	};
}
