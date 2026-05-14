#include "CortexStateTreeCommandHandler.h"

#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexSTAssetOps.h"
#include "Operations/CortexSTInspectOps.h"
#include "Operations/CortexSTStateOps.h"
#include "Operations/CortexSTTransitionOps.h"
#include "Operations/CortexSTValidationOps.h"

FCortexCommandResult FCortexStateTreeCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("list_assets"))
	{
		return FCortexSTAssetOps::ListAssets(Params);
	}
	if (Command == TEXT("create_asset"))
	{
		return FCortexSTAssetOps::CreateAsset(Params);
	}
	if (Command == TEXT("duplicate_asset"))
	{
		return FCortexSTAssetOps::DuplicateAsset(Params);
	}
	if (Command == TEXT("delete_asset"))
	{
		return FCortexSTAssetOps::DeleteAsset(Params);
	}
	if (Command == TEXT("dump_tree"))
	{
		return FCortexSTInspectOps::DumpTree(Params);
	}
	if (Command == TEXT("get_state"))
	{
		return FCortexSTInspectOps::GetState(Params);
	}
	if (Command == TEXT("check_structure"))
	{
		return FCortexSTValidationOps::CheckStructure(Params);
	}
	if (Command == TEXT("validate_asset"))
	{
		return FCortexSTValidationOps::ValidateAsset(Params);
	}
	if (Command == TEXT("compile"))
	{
		return FCortexSTValidationOps::Compile(Params);
	}
	if (Command == TEXT("add_state"))
	{
		return FCortexSTStateOps::AddState(Params);
	}
	if (Command == TEXT("remove_state"))
	{
		return FCortexSTStateOps::RemoveState(Params);
	}
	if (Command == TEXT("rename_state"))
	{
		return FCortexSTStateOps::RenameState(Params);
	}
	if (Command == TEXT("move_state"))
	{
		return FCortexSTStateOps::MoveState(Params);
	}
	if (Command == TEXT("set_state_properties"))
	{
		return FCortexSTStateOps::SetStateProperties(Params);
	}
	if (Command == TEXT("add_transition"))
	{
		return FCortexSTTransitionOps::AddTransition(Params);
	}
	if (Command == TEXT("remove_transition"))
	{
		return FCortexSTTransitionOps::RemoveTransition(Params);
	}
	if (Command == TEXT("set_transition_properties"))
	{
		return FCortexSTTransitionOps::SetTransitionProperties(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown StateTree command: %s"), *Command));
}

TArray<FCortexCommandInfo> FCortexStateTreeCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("list_assets"), TEXT("List StateTree assets") }
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Content path prefix"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum assets to return")),
		FCortexCommandInfo{ TEXT("create_asset"), TEXT("Create a StateTree asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Destination asset path"))
			.Required(TEXT("schema_class"), TEXT("string"), TEXT("StateTree schema class path or name"))
			.Optional(TEXT("root_name"), TEXT("string"), TEXT("Root state display name"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after creation")),
		FCortexCommandInfo{ TEXT("duplicate_asset"), TEXT("Duplicate a StateTree asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Source asset path"))
			.Required(TEXT("new_asset_path"), TEXT("string"), TEXT("Destination asset path"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after duplication")),
		FCortexCommandInfo{ TEXT("delete_asset"), TEXT("Delete a StateTree asset") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report referencers without deleting"))
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Delete despite referencers"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("dump_tree"), TEXT("Serialize a StateTree") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("include_transitions"), TEXT("boolean"), TEXT("Include transitions"))
			.Optional(TEXT("include_nodes"), TEXT("boolean"), TEXT("Include read-only node metadata")),
		FCortexCommandInfo{ TEXT("get_state"), TEXT("Get one StateTree state") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("State GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("State path")),
		FCortexCommandInfo{ TEXT("check_structure"), TEXT("Run read-only StateTree structure checks") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path")),
		FCortexCommandInfo{ TEXT("validate_asset"), TEXT("Run mutating StateTree validation fixups") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after validation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("compile"), TEXT("Compile a StateTree") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after compile"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("add_state"), TEXT("Add a StateTree state") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("New state display name"))
			.Optional(TEXT("parent_state_id"), TEXT("string"), TEXT("Parent state GUID"))
			.Optional(TEXT("parent_state_path"), TEXT("string"), TEXT("Parent state path"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("State type"))
			.Optional(TEXT("tag"), TEXT("string"), TEXT("Gameplay Tag"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enabled flag"))
			.Optional(TEXT("selection_behavior"), TEXT("string"), TEXT("Selection behavior"))
			.Optional(TEXT("index"), TEXT("number"), TEXT("Insert index under parent"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("remove_state"), TEXT("Remove a StateTree state") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("State GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("State path"))
			.Optional(TEXT("remove_children"), TEXT("boolean"), TEXT("Allow child deletion"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("rename_state"), TEXT("Rename a StateTree state") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("New state display name"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("State GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("State path"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("move_state"), TEXT("Move a StateTree state") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("State GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("State path"))
			.Optional(TEXT("new_parent_state_id"), TEXT("string"), TEXT("New parent state GUID"))
			.Optional(TEXT("new_parent_state_path"), TEXT("string"), TEXT("New parent state path"))
			.Optional(TEXT("index"), TEXT("number"), TEXT("Insert index under the new parent"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("set_state_properties"), TEXT("Set whitelisted StateTree state properties") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("State GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("State path"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("State properties patch"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.OptionalExpectedFingerprint(),
		FCortexCommandInfo{ TEXT("add_transition"), TEXT("Add a simple StateTree transition") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Optional(TEXT("source_state_id"), TEXT("string"), TEXT("Source state GUID"))
			.Optional(TEXT("source_state_path"), TEXT("string"), TEXT("Source state path"))
			.Optional(TEXT("target_state_id"), TEXT("string"), TEXT("Target state GUID"))
			.Optional(TEXT("target_state_path"), TEXT("string"), TEXT("Target state path"))
			.Optional(TEXT("trigger"), TEXT("string"), TEXT("Transition trigger enum name"))
			.Optional(TEXT("event_tag"), TEXT("string"), TEXT("Optional Gameplay Tag for event transitions"))
			.Optional(TEXT("priority"), TEXT("string"), TEXT("Transition priority"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Required stale-write guard for transition mutation")),
		FCortexCommandInfo{ TEXT("remove_transition"), TEXT("Remove a StateTree transition") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Required(TEXT("transition_id"), TEXT("string"), TEXT("Transition GUID or index token"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("Owning state GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("Owning state path"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Required stale-write guard for transition mutation")),
		FCortexCommandInfo{ TEXT("set_transition_properties"), TEXT("Set transition fields") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
			.Required(TEXT("transition_id"), TEXT("string"), TEXT("Transition GUID or index token"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Transition property values"))
			.Optional(TEXT("state_id"), TEXT("string"), TEXT("Owning state GUID"))
			.Optional(TEXT("state_path"), TEXT("string"), TEXT("Owning state path"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after mutation"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Persist package after mutation"))
			.Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Required stale-write guard for transition mutation")),
	};
}
