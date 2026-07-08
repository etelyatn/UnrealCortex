#include "CortexAnimationCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexAnimInspectOps.h"

FCortexCommandResult FCortexAnimationCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("list_assets"))
	{
		return FCortexAnimInspectOps::ListAssets(Params);
	}
	if (Command == TEXT("get_sequence_info"))
	{
		return FCortexAnimInspectOps::GetSequenceInfo(Params);
	}
	if (Command == TEXT("get_montage_info"))
	{
		return FCortexAnimInspectOps::GetMontageInfo(Params);
	}
	if (Command == TEXT("get_skeleton_info"))
	{
		return FCortexAnimInspectOps::GetSkeletonInfo(Params);
	}
	if (Command == TEXT("get_animbp_info"))
	{
		return FCortexAnimInspectOps::GetAnimBlueprintInfo(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown anim command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexAnimationCommandHandler::GetSupportedCommands() const
{
	TArray<FCortexCommandInfo> Commands;

	Commands.Add(
		FCortexCommandInfo{ TEXT("list_assets"), TEXT("List animation assets by type, path, and query. Read-only.") }
			.Optional(TEXT("asset_type"), TEXT("string"), TEXT("One of AnimSequence, AnimMontage, Skeleton, AnimBlueprint"))
			.Optional(TEXT("path"), TEXT("string"), TEXT("Package path prefix such as /Game/Characters"))
			.Optional(TEXT("query"), TEXT("string"), TEXT("Case-insensitive substring matched against object and package path"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum assets returned; default 50, max 200"))
	);
	Commands.Add(
		FCortexCommandInfo{ TEXT("get_sequence_info"), TEXT("Inspect UAnimSequence timing, skeleton, curves, notifies, sync markers, root motion, and fingerprint. Read-only.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence asset path"))
			.Optional(TEXT("notify_limit"), TEXT("number"), TEXT("Maximum notifies returned; default 50, max 200"))
			.Optional(TEXT("curve_limit"), TEXT("number"), TEXT("Maximum curves returned; default 50, max 200"))
			.Optional(TEXT("sync_marker_limit"), TEXT("number"), TEXT("Maximum sync markers returned; default 50, max 200"))
	);
	Commands.Add(
		FCortexCommandInfo{ TEXT("get_montage_info"), TEXT("Inspect UAnimMontage sections, slots, segments, notifies, branching points, skeleton, and fingerprint. Read-only.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimMontage asset path"))
			.Optional(TEXT("section_limit"), TEXT("number"), TEXT("Maximum sections returned; default 50, max 200"))
			.Optional(TEXT("slot_limit"), TEXT("number"), TEXT("Maximum slot tracks returned; default 20, max 100"))
			.Optional(TEXT("segment_limit"), TEXT("number"), TEXT("Maximum segments returned per slot; default 50, max 200"))
			.Optional(TEXT("notify_limit"), TEXT("number"), TEXT("Maximum notifies returned; default 50, max 200"))
	);
	Commands.Add(
		FCortexCommandInfo{ TEXT("get_skeleton_info"), TEXT("Inspect USkeleton bones, sockets, virtual bones, preview mesh, and fingerprint. Read-only.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Skeleton asset path"))
			.Optional(TEXT("bone_limit"), TEXT("number"), TEXT("Maximum bones returned; default 200, max 500"))
			.Optional(TEXT("socket_limit"), TEXT("number"), TEXT("Maximum sockets returned; default 100, max 200"))
			.Optional(TEXT("virtual_bone_limit"), TEXT("number"), TEXT("Maximum virtual bones returned; default 100, max 200"))
	);
	Commands.Add(
		FCortexCommandInfo{ TEXT("get_animbp_info"), TEXT("Inspect UAnimBlueprint skeleton, parent/generated classes, state machines, states, and transitions. Read-only.") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimBlueprint asset path"))
			.Optional(TEXT("state_machine_limit"), TEXT("number"), TEXT("Maximum state machines returned; default 20, max 100"))
			.Optional(TEXT("state_limit"), TEXT("number"), TEXT("Maximum states per state machine returned; default 100, max 200"))
			.Optional(TEXT("transition_limit"), TEXT("number"), TEXT("Maximum transitions per state machine returned; default 100, max 200"))
	);

	return Commands;
}
