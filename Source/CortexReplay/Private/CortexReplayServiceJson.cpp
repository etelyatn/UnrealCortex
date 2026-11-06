#include "CortexReplayServiceImpl.h"

#include "Dom/JsonValue.h"

namespace
{
const TCHAR* ServiceObservationStateToString(ECortexEditorUIObservationState State)
{
	switch (State)
	{
	case ECortexEditorUIObservationState::Ready: return TEXT("ready");
	case ECortexEditorUIObservationState::PointerPending: return TEXT("pointer_pending");
	case ECortexEditorUIObservationState::LayoutPending: return TEXT("layout_pending");
	case ECortexEditorUIObservationState::TargetMissing: return TEXT("target_missing");
	case ECortexEditorUIObservationState::TargetDisabled: return TEXT("target_disabled");
	case ECortexEditorUIObservationState::TargetNotHitTestable: return TEXT("target_not_hit_testable");
	case ECortexEditorUIObservationState::WrongTarget: return TEXT("wrong_target");
	case ECortexEditorUIObservationState::Ambiguous: return TEXT("ambiguous");
	case ECortexEditorUIObservationState::Unavailable: return TEXT("unavailable");
	default: return TEXT("unknown");
	}
}
}

namespace CortexReplayServicePrivate
{
TSharedRef<FJsonObject> ServiceGuardCoverageToJson(const FCortexReplayGuardCoverage& Coverage)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scope"), TEXT("press_only"));
	Object->SetNumberField(TEXT("pose_presses"), Coverage.PosePresses);
	Object->SetNumberField(TEXT("ui_supported_presses"), Coverage.UISupportedPresses);
	Object->SetNumberField(TEXT("ui_unavailable_presses"), Coverage.UIUnavailablePresses);
	Object->SetNumberField(TEXT("ui_not_applicable_presses"), Coverage.UINotApplicablePresses);
	return Object;
}

TSharedRef<FJsonObject> ServiceMetadataToJson(const FCortexReplayMetadata& Metadata)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplay"));
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("name"), Metadata.Name);
	Object->SetStringField(TEXT("description"), Metadata.Description);
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetStringField(TEXT("created_at_utc"), ServiceIsoUtc(Metadata.CreatedAtUtc));
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("initial_state_sha256"), Metadata.InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), Metadata.InputsSha256);

	TSharedRef<FJsonObject> Prerequisites = MakeShared<FJsonObject>();
	Prerequisites->SetNumberField(TEXT("local_player_index"), Metadata.Prerequisites.LocalPlayerIndex);
	TSharedRef<FJsonObject> ViewportSize = MakeShared<FJsonObject>();
	ViewportSize->SetNumberField(TEXT("x"), Metadata.Prerequisites.ViewportSize.X);
	ViewportSize->SetNumberField(TEXT("y"), Metadata.Prerequisites.ViewportSize.Y);
	Prerequisites->SetObjectField(TEXT("viewport_size"), ViewportSize);
	Prerequisites->SetNumberField(TEXT("dpi_scale"), Metadata.Prerequisites.DpiScale);
	// Every field readiness compares is reported, so a caller can pre-check INCOMPATIBLE_PREREQUISITES.
	Prerequisites->SetStringField(TEXT("player_input_class"), Metadata.Prerequisites.PlayerInputClass);
	Prerequisites->SetStringField(TEXT("input_config_sha256"), Metadata.Prerequisites.InputConfigSha256);
	Object->SetObjectField(TEXT("prerequisites"), Prerequisites);

	Object->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(Metadata.GuardCoverage));
	return Object;
}

/** Bounded waiting/progress object shared by get_run and the human current-operation status. */
TSharedRef<FJsonObject> ServiceWaitingToJson(const FCortexReplayScheduler& Scheduler)
{
	const double Committed = Scheduler.GetAuthorizedWaitSeconds();
	const double Current = Scheduler.GetCurrentWaitSeconds();
	const double RemainingRun = FMath::Max(0.0, CortexReplayWaitBudget::MaxAuthorizedWaitSeconds - Committed);
	const double PerPressLimit = FMath::Min(CortexReplayWaitBudget::MaxWaitPerPressSeconds, RemainingRun);

	TSharedRef<FJsonObject> Waiting = MakeShared<FJsonObject>();
	Waiting->SetNumberField(TEXT("sequence"), Scheduler.GetWaitingSequence());
	Waiting->SetStringField(TEXT("reason"),
		FString(ServiceObservationStateToString(Scheduler.GetWaitReason())));
	// The blocked event's current wait duration, reported separately from the committed offset.
	Waiting->SetNumberField(TEXT("elapsed_seconds"), Current);
	Waiting->SetNumberField(TEXT("committed_wait_seconds"), Committed);
	Waiting->SetNumberField(TEXT("remaining_event_seconds"), FMath::Max(0.0, PerPressLimit - Current));
	Waiting->SetNumberField(TEXT("remaining_run_seconds"),
		FMath::Max(0.0, CortexReplayWaitBudget::MaxAuthorizedWaitSeconds - Committed - Current));
	return Waiting;
}
}

using namespace CortexReplayServicePrivate;

TSharedRef<FJsonObject> FCortexReplayService::BuildRunData(const FCortexReplayRunRecord& Record,
	bool bLive, const FCortexReplayScheduler* Scheduler)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), ServiceGuidToString(Record.Id));
	Data->SetNumberField(TEXT("recording_id"), Record.RecordingId);
	Data->SetStringField(TEXT("origin"), ServiceOriginToString(Record.Origin));
	Data->SetStringField(TEXT("editor_instance_id"), Record.EditorInstanceId);
	Data->SetStringField(TEXT("state"), ServiceStateToString(Record.State));
	Data->SetStringField(TEXT("guard_scope"), TEXT("press_only"));
	Data->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(Record.GuardCoverage));
	Data->SetNumberField(TEXT("dispatched_events"), Record.DispatchedEvents);
	Data->SetNumberField(TEXT("total_events"), Record.TotalEvents);
	Data->SetNumberField(TEXT("authorized_wait_seconds"), Record.AuthorizedWaitSeconds);
	if (!Record.bLegacyFormat)
	{
		Data->SetNumberField(TEXT("completed_frames"), Record.CompletedFrames);
		Data->SetNumberField(TEXT("frame_count"), Record.FrameCount);
		Data->SetStringField(TEXT("frames_sha256"), Record.FramesSha256);
		Data->SetNumberField(TEXT("behind_seconds"), Record.BehindSeconds);
	}
	// A pre-frame record has no frame identity to report, so the fields are absent rather than
	// fabricated as zeros; the frame in preparation exists only while a replay is running.
	if (bLive && Scheduler != nullptr && Scheduler->GetCurrentFrame().IsSet())
	{
		Data->SetNumberField(TEXT("current_frame"), Scheduler->GetCurrentFrame().GetValue());
	}
	else
	{
		Data->SetField(TEXT("current_frame"), MakeShared<FJsonValueNull>());
	}
	Data->SetStringField(TEXT("recording_snapshot_sha256"), Record.RecordingSnapshotSha256);
	Data->SetStringField(TEXT("initial_state_sha256"), Record.InitialStateSha256);
	Data->SetStringField(TEXT("inputs_sha256"), Record.InputsSha256);
	Data->SetStringField(TEXT("started_at_utc"), ServiceIsoUtc(Record.StartedAtUtc));
	if (Record.FinalizedAtUtc.GetTicks() > 0)
	{
		Data->SetField(TEXT("finalized_at_utc"),
			MakeShared<FJsonValueString>(ServiceIsoUtc(Record.FinalizedAtUtc)));
	}
	else
	{
		Data->SetField(TEXT("finalized_at_utc"), MakeShared<FJsonValueNull>());
	}

	if (Record.State != ECortexReplayState::Completed && !Record.ExecutionError.ErrorCode.IsEmpty())
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Record.ExecutionError.ErrorCode);
		Error->SetStringField(TEXT("message"), Record.ExecutionError.ErrorMessage);
		if (Record.ExecutionError.ErrorDetails.IsValid())
		{
			// Bounded details (sequence/kind/tolerances/normalized positions/hashes/wait values).
			Error->SetObjectField(TEXT("details"), Record.ExecutionError.ErrorDetails);
		}
		Data->SetObjectField(TEXT("execution_error"), Error);
	}
	else
	{
		Data->SetField(TEXT("execution_error"), MakeShared<FJsonValueNull>());
	}

	if (bLive && Scheduler != nullptr && Scheduler->GetWaitingSequence() != INDEX_NONE)
	{
		Data->SetObjectField(TEXT("waiting"), ServiceWaitingToJson(*Scheduler));
	}
	else
	{
		Data->SetField(TEXT("waiting"), MakeShared<FJsonValueNull>());
	}

	return Data;
}

TSharedRef<FJsonObject> FCortexReplayService::FImpl::BuildActiveRunData() const
{
	return BuildRunData(ActiveRun, IsLiveReplaying(), Scheduler.Get());
}
