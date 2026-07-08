#include "Operations/CortexAnimInspectOps.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Operations/CortexAnimAssetUtils.h"

namespace
{
FCortexCommandResult CommandRegisteredBeforeInspectionSlice(const FString& Command)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		FString::Printf(TEXT("anim.%s is registered before the inspection slice is installed"), *Command)
	);
}

FString ObjectPathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

void SetUnavailableFields(TSharedPtr<FJsonObject>& Data, const TArray<FString>& Fields)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Field : Fields)
	{
		Values.Add(MakeShared<FJsonValueString>(Field));
	}

	Data->SetArrayField(TEXT("unavailable_fields"), Values);
}
}

FCortexCommandResult FCortexAnimInspectOps::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetType;
	FString Path = TEXT("/Game");
	FString Query;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset_type"), AssetType);
		Params->TryGetStringField(TEXT("path"), Path);
		Params->TryGetStringField(TEXT("query"), Query);
	}

	if (!AssetType.IsEmpty() && !FCortexAnimAssetUtils::IsSupportedAssetType(AssetType))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Unsupported animation asset_type: %s"), *AssetType));
	}

	const int32 Limit = FCortexAnimAssetUtils::ReadLimit(Params, TEXT("limit"), 50, 200);
	TArray<FAssetData> Assets;
	FCortexCommandResult Error;
	if (!FCortexAnimAssetUtils::ListAnimationAssets(AssetType, Path, Query, Assets, Error))
	{
		return Error;
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Assets.Num());
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Item->SetStringField(TEXT("asset_type"), AssetData.AssetClassPath.GetAssetName().ToString());
		Item->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_type"), AssetType);
	Data->SetStringField(TEXT("path"), Path);
	Data->SetStringField(TEXT("query"), Query);
	Data->SetObjectField(TEXT("assets"), FCortexAnimAssetUtils::MakeLimitedArray(Items, Limit));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAnimInspectOps::GetSequenceInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimResolvedAsset Resolved;
	FCortexCommandResult Error;
	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimSequence>(Params, TEXT("AnimSequence"), Resolved, Error))
	{
		return Error;
	}

	UAnimSequence* Sequence = CastChecked<UAnimSequence>(Resolved.Asset);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetNumberField(TEXT("length_seconds"), Sequence->GetPlayLength());
	Data->SetStringField(TEXT("skeleton"), ObjectPathOf(Sequence->GetSkeleton()));
	Data->SetBoolField(TEXT("enable_root_motion"), Sequence->bEnableRootMotion);
	Data->SetNumberField(TEXT("num_sampled_keys"), Sequence->GetNumberOfSampledKeys());
	Data->SetStringField(TEXT("sampling_frame_rate"), Sequence->GetSamplingFrameRate().ToPrettyText().ToString());

	TArray<FString> Unavailable;
	const IAnimationDataModel* DataModel = Sequence->GetDataModel();
	if (DataModel != nullptr)
	{
		Data->SetStringField(TEXT("source_frame_rate"), DataModel->GetFrameRate().ToPrettyText().ToString());
		Data->SetNumberField(TEXT("source_frame_count"), DataModel->GetNumberOfFrames());
	}
	else
	{
		Unavailable.Add(TEXT("source_frame_rate"));
		Unavailable.Add(TEXT("source_frame_count"));
	}

	TArray<TSharedPtr<FJsonValue>> Notifies;
	for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
	{
		const FAnimNotifyEvent& Notify = Sequence->Notifies[Index];
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		Item->SetNumberField(TEXT("time"), Notify.GetTime());
		Item->SetNumberField(TEXT("duration"), Notify.GetDuration());
		Item->SetStringField(TEXT("notify_class"), Notify.Notify != nullptr ? Notify.Notify->GetClass()->GetPathName() : FString());
		Item->SetStringField(TEXT("notify_object"), ObjectPathOf(Notify.Notify));
		Notifies.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("notifies"), FCortexAnimAssetUtils::MakeLimitedArray(Notifies, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("notify_limit"), 50, 200)));

	TArray<TSharedPtr<FJsonValue>> Curves;
	if (DataModel != nullptr)
	{
		const FAnimationCurveData& CurveData = DataModel->GetCurveData();
		for (const FFloatCurve& Curve : CurveData.FloatCurves)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Curve.GetName().ToString());
			Item->SetStringField(TEXT("type"), TEXT("float"));
			Curves.Add(MakeShared<FJsonValueObject>(Item));
		}
		for (const FTransformCurve& Curve : CurveData.TransformCurves)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Curve.GetName().ToString());
			Item->SetStringField(TEXT("type"), TEXT("transform"));
			Curves.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	Data->SetObjectField(TEXT("curves"), FCortexAnimAssetUtils::MakeLimitedArray(Curves, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("curve_limit"), 50, 200)));

	TArray<TSharedPtr<FJsonValue>> SyncMarkers;
	for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
		Item->SetNumberField(TEXT("time"), Marker.Time);
		SyncMarkers.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("sync_markers"), FCortexAnimAssetUtils::MakeLimitedArray(SyncMarkers, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("sync_marker_limit"), 50, 200)));

	SetUnavailableFields(Data, Unavailable);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAnimInspectOps::GetMontageInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimResolvedAsset Resolved;
	FCortexCommandResult Error;
	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimMontage>(Params, TEXT("AnimMontage"), Resolved, Error))
	{
		return Error;
	}

	UAnimMontage* Montage = CastChecked<UAnimMontage>(Resolved.Asset);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetNumberField(TEXT("length_seconds"), Montage->GetPlayLength());
	Data->SetStringField(TEXT("skeleton"), ObjectPathOf(Montage->GetSkeleton()));

	TArray<TSharedPtr<FJsonValue>> Sections;
	for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
	{
		const FCompositeSection& Section = Montage->CompositeSections[Index];
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("name"), Section.SectionName.ToString());
		Item->SetNumberField(TEXT("time"), Section.GetTime());
		Item->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
		Sections.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("sections"), FCortexAnimAssetUtils::MakeLimitedArray(Sections, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("section_limit"), 50, 200)));

	const int32 SegmentLimit = FCortexAnimAssetUtils::ReadLimit(Params, TEXT("segment_limit"), 50, 200);
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
	{
		TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("slot_name"), SlotTrack.SlotName.ToString());

		TArray<TSharedPtr<FJsonValue>> Segments;
		for (const FAnimSegment& Segment : SlotTrack.AnimTrack.AnimSegments)
		{
			TSharedPtr<FJsonObject> SegmentObj = MakeShared<FJsonObject>();
			SegmentObj->SetStringField(TEXT("asset_path"), ObjectPathOf(Segment.GetAnimReference()));
			SegmentObj->SetNumberField(TEXT("start_pos"), Segment.StartPos);
			SegmentObj->SetNumberField(TEXT("anim_start_time"), Segment.AnimStartTime);
			SegmentObj->SetNumberField(TEXT("anim_end_time"), Segment.AnimEndTime);
			Segments.Add(MakeShared<FJsonValueObject>(SegmentObj));
		}

		Slot->SetObjectField(TEXT("segments"), FCortexAnimAssetUtils::MakeLimitedArray(Segments, SegmentLimit));
		Slots.Add(MakeShared<FJsonValueObject>(Slot));
	}
	Data->SetObjectField(TEXT("slots"), FCortexAnimAssetUtils::MakeLimitedArray(Slots, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("slot_limit"), 20, 100)));

	TArray<TSharedPtr<FJsonValue>> Notifies;
	TArray<TSharedPtr<FJsonValue>> BranchingPoints;
	for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
	{
		const FAnimNotifyEvent& Notify = Montage->Notifies[Index];
		const bool bIsBranchingPoint = Notify.IsBranchingPoint();
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		Item->SetNumberField(TEXT("time"), Notify.GetTime());
		Item->SetNumberField(TEXT("duration"), Notify.GetDuration());
		Item->SetStringField(TEXT("notify_class"), Notify.Notify != nullptr ? Notify.Notify->GetClass()->GetPathName() : FString());
		Item->SetStringField(TEXT("notify_object"), ObjectPathOf(Notify.Notify));
		Item->SetBoolField(TEXT("is_branching_point"), bIsBranchingPoint);
		Notifies.Add(MakeShared<FJsonValueObject>(Item));

		if (bIsBranchingPoint)
		{
			TSharedPtr<FJsonObject> BranchingPoint = MakeShared<FJsonObject>();
			BranchingPoint->SetNumberField(TEXT("notify_index"), Index);
			BranchingPoint->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
			BranchingPoint->SetNumberField(TEXT("time"), Notify.GetTime());
			BranchingPoint->SetNumberField(TEXT("duration"), Notify.GetDuration());
			BranchingPoints.Add(MakeShared<FJsonValueObject>(BranchingPoint));
		}
	}
	Data->SetObjectField(TEXT("notifies"), FCortexAnimAssetUtils::MakeLimitedArray(Notifies, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("notify_limit"), 50, 200)));
	Data->SetObjectField(TEXT("branching_points"), FCortexAnimAssetUtils::MakeLimitedArray(BranchingPoints, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("notify_limit"), 50, 200)));

	SetUnavailableFields(Data, {});
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAnimInspectOps::GetSkeletonInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimResolvedAsset Resolved;
	FCortexCommandResult Error;
	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<USkeleton>(Params, TEXT("Skeleton"), Resolved, Error))
	{
		return Error;
	}

	USkeleton* Skeleton = CastChecked<USkeleton>(Resolved.Asset);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetStringField(TEXT("preview_mesh"), ObjectPathOf(Skeleton->GetPreviewMesh()));

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	TArray<TSharedPtr<FJsonValue>> Bones;
	for (int32 Index = 0; Index < RefSkeleton.GetNum(); ++Index)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("name"), RefSkeleton.GetBoneName(Index).ToString());
		Item->SetNumberField(TEXT("parent_index"), RefSkeleton.GetParentIndex(Index));
		Bones.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("bones"), FCortexAnimAssetUtils::MakeLimitedArray(Bones, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("bone_limit"), 200, 500)));

	TArray<TSharedPtr<FJsonValue>> Sockets;
	for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket == nullptr)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		Item->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
		Sockets.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("sockets"), FCortexAnimAssetUtils::MakeLimitedArray(Sockets, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("socket_limit"), 100, 200)));

	TArray<TSharedPtr<FJsonValue>> VirtualBones;
	for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source_bone"), VirtualBone.SourceBoneName.ToString());
		Item->SetStringField(TEXT("target_bone"), VirtualBone.TargetBoneName.ToString());
		Item->SetStringField(TEXT("virtual_bone"), VirtualBone.VirtualBoneName.ToString());
		VirtualBones.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetObjectField(TEXT("virtual_bones"), FCortexAnimAssetUtils::MakeLimitedArray(VirtualBones, FCortexAnimAssetUtils::ReadLimit(Params, TEXT("virtual_bone_limit"), 100, 200)));

	SetUnavailableFields(Data, {});
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexAnimInspectOps::GetAnimBlueprintInfo(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimResolvedAsset Resolved;
	FCortexCommandResult Error;
	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimBlueprint>(Params, TEXT("AnimBlueprint"), Resolved, Error))
	{
		return Error;
	}

	UAnimBlueprint* AnimBlueprint = CastChecked<UAnimBlueprint>(Resolved.Asset);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	FCortexAnimAssetUtils::SetCommonAssetFields(Data, Resolved);
	Data->SetStringField(TEXT("target_skeleton"), ObjectPathOf(AnimBlueprint->TargetSkeleton));
	Data->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass != nullptr ? AnimBlueprint->ParentClass->GetPathName() : FString());
	Data->SetStringField(TEXT("generated_class"), AnimBlueprint->GeneratedClass != nullptr ? AnimBlueprint->GeneratedClass->GetPathName() : FString());

	const int32 StateMachineLimit = FCortexAnimAssetUtils::ReadLimit(Params, TEXT("state_machine_limit"), 20, 100);
	const int32 StateLimit = FCortexAnimAssetUtils::ReadLimit(Params, TEXT("state_limit"), 100, 200);
	const int32 TransitionLimit = FCortexAnimAssetUtils::ReadLimit(Params, TEXT("transition_limit"), 100, 200);

	TArray<TSharedPtr<FJsonValue>> StateMachines;
	TArray<UEdGraph*> AllGraphs;
	AnimBlueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		UAnimationStateMachineGraph* StateMachineGraph = Cast<UAnimationStateMachineGraph>(Graph);
		if (StateMachineGraph == nullptr)
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> States;
		TArray<TSharedPtr<FJsonValue>> Transitions;
		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
			{
				TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("name"), StateNode->GetStateName());
				State->SetStringField(TEXT("node_guid"), StateNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				State->SetStringField(TEXT("graph_guid"), StateNode->BoundGraph != nullptr ? StateNode->BoundGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
				States.Add(MakeShared<FJsonValueObject>(State));
				continue;
			}

			if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
			{
				UAnimStateNode* PreviousState = Cast<UAnimStateNode>(TransitionNode->GetPreviousState());
				UAnimStateNode* NextState = Cast<UAnimStateNode>(TransitionNode->GetNextState());
				TSharedPtr<FJsonObject> Transition = MakeShared<FJsonObject>();
				Transition->SetStringField(TEXT("name"), TransitionNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Transition->SetStringField(TEXT("from_state"), PreviousState != nullptr ? PreviousState->GetStateName() : FString());
				Transition->SetStringField(TEXT("to_state"), NextState != nullptr ? NextState->GetStateName() : FString());
				Transition->SetStringField(TEXT("node_guid"), TransitionNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				Transition->SetStringField(TEXT("graph_guid"), TransitionNode->BoundGraph != nullptr ? TransitionNode->BoundGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
				Transitions.Add(MakeShared<FJsonValueObject>(Transition));
			}
		}

		TSharedPtr<FJsonObject> Machine = MakeShared<FJsonObject>();
		Machine->SetStringField(TEXT("name"), StateMachineGraph->GetName());
		Machine->SetStringField(TEXT("graph_guid"), StateMachineGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Machine->SetObjectField(TEXT("states"), FCortexAnimAssetUtils::MakeLimitedArray(States, StateLimit));
		Machine->SetObjectField(TEXT("transitions"), FCortexAnimAssetUtils::MakeLimitedArray(Transitions, TransitionLimit));
		StateMachines.Add(MakeShared<FJsonValueObject>(Machine));
	}

	Data->SetObjectField(TEXT("state_machines"), FCortexAnimAssetUtils::MakeLimitedArray(StateMachines, StateMachineLimit));
	SetUnavailableFields(Data, {});
	return FCortexCommandRouter::Success(Data);
}
