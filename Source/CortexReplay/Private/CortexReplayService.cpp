#include "CortexReplayService.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayModule.h"
#include "CortexReplayRunStore.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTypes.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/Package.h"

namespace
{
/** The service's monotonic preparation deadline starts at native acceptance. */
constexpr double ServicePreparationDeadlineSeconds = 30.0;

constexpr int32 ServiceRecentTerminalRunLimit = 100;
constexpr double ServiceRecentTerminalWindowSeconds = 86400.0;

/** Mirrors of the scheduler's fixed wait budgets for the reported progress fields. */
constexpr double ServiceMaxAuthorizedWaitSeconds = 5.0;
constexpr double ServiceMaxWaitPerPressSeconds = 1.0;

FString ServiceGuidToString(const FGuid& Id)
{
	return Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

const TCHAR* ServiceStateToString(ECortexReplayState State)
{
	switch (State)
	{
	case ECortexReplayState::Preparing: return TEXT("Preparing");
	case ECortexReplayState::Recording: return TEXT("Recording");
	case ECortexReplayState::Replaying: return TEXT("Replaying");
	case ECortexReplayState::Finalizing: return TEXT("Finalizing");
	case ECortexReplayState::Completed: return TEXT("Completed");
	case ECortexReplayState::Cancelled: return TEXT("Cancelled");
	case ECortexReplayState::Interrupted: return TEXT("Interrupted");
	case ECortexReplayState::Error: return TEXT("Error");
	default: return TEXT("Error");
	}
}

const TCHAR* ServiceOriginToString(ECortexReplayOrigin Origin)
{
	return Origin == ECortexReplayOrigin::AI ? TEXT("ai") : TEXT("human");
}

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

FString ServiceIsoUtc(const FDateTime& Time)
{
	return Time.GetTicks() > 0 ? Time.ToIso8601() : FString();
}

FCortexCommandResult ServiceSuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult ServiceError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}

FCortexCommandResult ServiceError(const FString& Code, const FString& Message)
{
	return FCortexCommandRouter::Error(Code, Message);
}

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
	Object->SetObjectField(TEXT("prerequisites"), Prerequisites);

	Object->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(Metadata.GuardCoverage));
	return Object;
}

/**
 * Reads only the live `ai_enabled` permission bit for a recording.
 *
 * The immutable admitted payload is never reloaded for this; a small metadata-only read keeps the
 * live permission check independent of the recorded inputs.
 */
bool ServiceReadMetadataAIEnabled(const FString& ProjectRoot, int32 RecordingId, bool& bOutEnabled)
{
	const FString Path = FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(ProjectRoot, TEXT(".cortex/replay/recordings")),
			FString::FromInt(RecordingId)),
		TEXT("metadata.json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	int32 ParsedId = 0;
	if (!Object->TryGetNumberField(TEXT("recording_id"), ParsedId) || ParsedId != RecordingId)
	{
		return false;
	}
	return Object->TryGetBoolField(TEXT("ai_enabled"), bOutEnabled);
}

/** Bounded waiting/progress object shared by get_run and the human current-operation status. */
TSharedRef<FJsonObject> ServiceWaitingToJson(const FCortexReplayScheduler& Scheduler)
{
	const double Committed = Scheduler.GetAuthorizedWaitSeconds();
	const double Current = Scheduler.GetCurrentWaitSeconds();
	const double RemainingRun = FMath::Max(0.0, ServiceMaxAuthorizedWaitSeconds - Committed);
	const double PerPressLimit = FMath::Min(ServiceMaxWaitPerPressSeconds, RemainingRun);

	TSharedRef<FJsonObject> Waiting = MakeShared<FJsonObject>();
	Waiting->SetNumberField(TEXT("sequence"), Scheduler.GetWaitingSequence());
	Waiting->SetStringField(TEXT("reason"),
		FString(ServiceObservationStateToString(Scheduler.GetWaitReason())));
	// The blocked event's current wait duration, reported separately from the committed offset.
	Waiting->SetNumberField(TEXT("elapsed_seconds"), Current);
	Waiting->SetNumberField(TEXT("committed_wait_seconds"), Committed);
	Waiting->SetNumberField(TEXT("remaining_event_seconds"), FMath::Max(0.0, PerPressLimit - Current));
	Waiting->SetNumberField(TEXT("remaining_run_seconds"),
		FMath::Max(0.0, ServiceMaxAuthorizedWaitSeconds - Committed - Current));
	return Waiting;
}
}

/**
 * All mutable service state lives here so the public header stays declaration-only.
 *
 * The finalization ticker captures a shared reference to this state, so an asynchronous owned
 * teardown can finish (and persist its terminal record) after the service object itself is
 * destroyed without any tick touching freed memory.
 */
struct FCortexReplayService::FImpl : public TSharedFromThis<FCortexReplayService::FImpl>
{
	/** The single capture operation's phase; capture ownership spans preparation through teardown. */
	enum class ECapturePhase : uint8 { None, Preparing, Recording, Finalizing };

	explicit FImpl(FCortexReplayService* InOwner, const FString& InProjectRoot)
		: Owner(InOwner)
		, ProjectRoot(InProjectRoot)
		, Library(InProjectRoot)
		, RunStore(InProjectRoot)
		, EditorInstanceId(ServiceGuidToString(FGuid::NewGuid()))
	{
		RunStore.Initialize();
	}

	FCortexReplayService* Owner = nullptr;
	FString ProjectRoot;
	FCortexReplayLibrary Library;
	FCortexReplayRunStore RunStore;
	FString EditorInstanceId;

	TSharedPtr<FCortexEditorPhysicalInputSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bShutdown = false;

	// ---- capture (human) ----
	ECapturePhase CapturePhase = ECapturePhase::None;
	/** Scopes async capture callbacks to the exact operation that started them. */
	uint64 CaptureOperationGeneration = 0;
	bool bBorrowedCapture = false;
	bool bOwnedCapture = false;
	bool bCaptureFaulted = false;
	bool bCapturePublishOnComplete = false;
	FCortexCommandResult CaptureFaultResult;
	int32 CaptureRecordingId = 0;
	FString CaptureMapAssetPath;
	FCortexEditorPhysicalInputTargetInfo CaptureTargetInfo;
	FCortexEditorPhysicalInputPlayerPose CaptureInitialPose;
	TArray<FCortexReplayEvent> CaptureEvents;
	double CaptureEpochSeconds = 0.0;
	double CaptureStopSeconds = 0.0;
	/** Throttle for owned-capture publication retries after a storage/validation failure. */
	double LastCapturePublishAttemptSeconds = 0.0;
	/**
	 * Retained capture publication failure, kept separate from the interruption faults
	 * (`bCaptureFaulted`/`CaptureFaultResult`) so it never suppresses the publication retry.
	 */
	bool bCapturePublicationFailed = false;
	FCortexCommandResult CapturePublicationError;

	// ---- run ----
	bool bRunActive = false;
	bool bFinalizing = false;
	bool bTerminalPublished = false;
	bool bFrozen = false;
	bool bOwnedPie = false;
	ECortexReplayState RunState = ECortexReplayState::Preparing;
	FCortexReplayRunRecord ActiveRun;
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	TSharedPtr<FCortexReplayScheduler> Scheduler;
	double PrepareDeadline = 0.0;
	double ReplayEpoch = 0.0;
	bool bReadySeen = false;
	FCortexCommandResult PreparationResult;
	bool bCancellationRequested = false;
	FCortexCommandResult CancellationResult;
	bool bInterruptionRequested = false;
	FCortexCommandResult InterruptionResult;
	bool bPermissionRevoked = false;
	FCortexCommandResult PermissionRevocationResult;
	ECortexReplayState PendingTerminalState = ECortexReplayState::Cancelled;
	FCortexCommandResult PendingTerminalResult;

	bool Tick(float DeltaSeconds);
	void TickRun();
	void TickCapture();
	void TickFinalization();
	void EnsureTicker();
	void DetachTickerAndSession();
	void ResetRun();
	void ResetCapture();
	FCortexReplayGuardDecision EvaluateGuard(const FCortexReplayEvent& Event);
	FCortexCommandResult DispatchEvent(const FCortexReplayEvent& Event);
	FCortexCommandResult DispatchBlockError() const;
	bool CheckLivePermission();
	void MarkCaptureFaulted(const FCortexCommandResult& Result);
	void BeginCaptureFinalization(bool bPublish);
	void RetainCaptureForPublicationRetry(const FCortexCommandResult& PublishResult);
	void ClearCapturePublicationFailure();
	void CompleteCaptureFinalization();
	FCortexCommandResult PublishCaptureSnapshot();
	void OnCaptureInterruption(uint64 Generation, const FCortexCommandResult& Result);
	void OnOwnedCaptureReady(uint64 Generation, const FCortexCommandResult& Ready);
	void OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
		const FCortexEditorPhysicalInputCaptureContext& Context);
	void SetRunState(ECortexReplayState NewState);
};

void FCortexReplayService::FImpl::EnsureTicker()
{
	if (TickerHandle.IsValid() || bShutdown)
	{
		return;
	}
	// The ticker holds a shared reference to this state so an in-flight finalization can never
	// outlive the memory that owns it.
	const TSharedRef<FCortexReplayService::FImpl> Self = AsShared();
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Self](float DeltaSeconds)
		{
			return Self->Tick(DeltaSeconds);
		}));
}

void FCortexReplayService::FImpl::DetachTickerAndSession()
{
	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FCortexReplayService::FImpl::SetRunState(ECortexReplayState NewState)
{
	RunState = NewState;
	ActiveRun.State = NewState;
}

bool FCortexReplayService::FImpl::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (bShutdown)
	{
		// The owning service may already be destroyed: only the lifetime-owned finalization
		// bookkeeping runs, and the ticker stops as soon as nothing is pending.
		TickCapture();
		TickFinalization();
		return bRunActive || bFinalizing || CapturePhase != ECapturePhase::None;
	}
	TickRun();
	TickCapture();
	TickFinalization();
	return true;
}

FCortexCommandResult FCortexReplayService::FImpl::DispatchBlockError() const
{
	if (bInterruptionRequested && !InterruptionResult.ErrorCode.IsEmpty())
	{
		return InterruptionResult;
	}
	if (bCancellationRequested && !CancellationResult.ErrorCode.IsEmpty())
	{
		return CancellationResult;
	}
	return ServiceError(CortexReplayErrorCodes::InvalidOperation,
		TEXT("Replay dispatch is no longer active"));
}

bool FCortexReplayService::FImpl::CheckLivePermission()
{
	if (!bRunActive || bFinalizing || ActiveRun.Origin != ECortexReplayOrigin::AI)
	{
		return true;
	}
	if (bPermissionRevoked)
	{
		return false;
	}

	// The live grant is re-read for every guard/dispatch decision and at trailing completion: a
	// cached value is never treated as fresh authorization, so an external revocation takes effect
	// before the next press. Only the metadata permission bit is read, never the recorded inputs.
	bool bEnabled = false;
	if (!ServiceReadMetadataAIEnabled(ProjectRoot, ActiveRun.RecordingId, bEnabled) || !bEnabled)
	{
		bPermissionRevoked = true;
		PermissionRevocationResult = ServiceError(CortexReplayErrorCodes::PermissionDenied,
			TEXT("AI replay permission was revoked"));
		bCancellationRequested = true;
		CancellationResult = PermissionRevocationResult;
		return false;
	}
	return true;
}

void FCortexReplayService::FImpl::ResetRun()
{
	bRunActive = false;
	bFinalizing = false;
	bTerminalPublished = false;
	bFrozen = false;
	bOwnedPie = false;
	bReadySeen = false;
	bCancellationRequested = false;
	bInterruptionRequested = false;
	bPermissionRevoked = false;
	CancellationResult = FCortexCommandResult();
	InterruptionResult = FCortexCommandResult();
	PermissionRevocationResult = FCortexCommandResult();
	PreparationResult = FCortexCommandResult();
	PendingTerminalResult = FCortexCommandResult();
	PendingTerminalState = ECortexReplayState::Cancelled;
	ActiveRun = FCortexReplayRunRecord();
	Snapshot.Reset();
	Scheduler.Reset();
	PrepareDeadline = 0.0;
	ReplayEpoch = 0.0;
}

void FCortexReplayService::FImpl::TickRun()
{
	if (!bRunActive || bFinalizing || !Snapshot.IsValid())
	{
		return;
	}

	if (RunState == ECortexReplayState::Preparing)
	{
		if (!Session.IsValid())
		{
			Owner->Finalize(ECortexReplayState::Error,
				ServiceError(CortexReplayErrorCodes::TargetUnavailable,
					TEXT("Replay target session is unavailable")));
			return;
		}

		// The service's deadline cannot be extended by the session's own deadline.
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
			return;
		}
		if (!bReadySeen)
		{
			return;
		}
		if (!PreparationResult.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s preparation failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *PreparationResult.ErrorCode,
				*PreparationResult.ErrorMessage);
			Owner->Finalize(ECortexReplayState::Error, PreparationResult);
			return;
		}

		// Recheck permission after readiness and before the epoch starts.
		if (ActiveRun.Origin == ECortexReplayOrigin::AI)
		{
			TSharedPtr<const FCortexReplaySnapshot> Recheck;
			const FCortexCommandResult Eligible = Library.Load(ActiveRun.RecordingId, true, Recheck);
			if (!Eligible.bSuccess)
			{
				UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s lost eligibility after readiness: %s (%s)"),
					*ServiceGuidToString(ActiveRun.Id), *Eligible.ErrorCode, *Eligible.ErrorMessage);
				Owner->Finalize(ECortexReplayState::Error, Eligible);
				return;
			}
		}

		// One-time pose restoration and immediate readback.
		const FCortexCommandResult Restored = Session->RestorePlayerPose(
			Snapshot->InitialState.Pose, Snapshot->InitialState.PawnClassPath);
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
			return;
		}
		if (!Restored.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s pose restoration failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *Restored.ErrorCode, *Restored.ErrorMessage);
			Owner->Finalize(ECortexReplayState::Error, Restored);
			return;
		}

		FCortexCommandResult TargetError;
		if (!Session->ValidateTarget(TargetError))
		{
			Owner->Finalize(ECortexReplayState::Interrupted, TargetError);
			return;
		}

		// Final deadline check immediately before the epoch is established.
		if (FPlatformTime::Seconds() >= PrepareDeadline)
		{
			Owner->Finalize(ECortexReplayState::Error, ServiceError(
				CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
				TEXT("Replay preparation exceeded the monotonic deadline")));
			return;
		}

		Scheduler = MakeShared<FCortexReplayScheduler>(Snapshot.ToSharedRef());
		ReplayEpoch = FPlatformTime::Seconds();

		// Arm replay interference ownership at epoch establishment, before the first dispatch, so a
		// foreign focus/input established during the pre-first-event window is treated as
		// interference rather than being delivered to the foreign consumer.
		const FCortexCommandResult Epoch = Session->BeginReplayEpoch();
		if (!Epoch.bSuccess)
		{
			Owner->Finalize(ECortexReplayState::Interrupted, Epoch);
			return;
		}

		SetRunState(ECortexReplayState::Replaying);
	}

	if (RunState == ECortexReplayState::Replaying)
	{
		if (bCancellationRequested)
		{
			Owner->Finalize(ECortexReplayState::Cancelled,
				CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			return;
		}
		if (bInterruptionRequested)
		{
			Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			return;
		}

		const FCortexCommandResult Advanced = Scheduler->Advance(
			[this]() { return FPlatformTime::Seconds() - ReplayEpoch; },
			[this](const FCortexReplayEvent& Event) { return EvaluateGuard(Event); },
			[this](const FCortexReplayEvent& Event) { return DispatchEvent(Event); });

		ActiveRun.DispatchedEvents = Scheduler->GetDispatchedCount();
		ActiveRun.AuthorizedWaitSeconds = Scheduler->GetAuthorizedWaitSeconds();

		if (!Advanced.bSuccess)
		{
			UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s dispatch failed: %s (%s)"),
				*ServiceGuidToString(ActiveRun.Id), *Advanced.ErrorCode, *Advanced.ErrorMessage);
			if (bInterruptionRequested)
			{
				Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			}
			else if (bCancellationRequested)
			{
				Owner->Finalize(ECortexReplayState::Cancelled,
					CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			}
			else
			{
				Owner->Finalize(ECortexReplayState::Error, Advanced);
			}
			return;
		}

		// Completion is only claimed when no cancellation/interruption arrived at the boundary, and
		// a recording whose tail is idle still revalidates the live grant before completing.
		if (Scheduler->IsComplete())
		{
			CheckLivePermission();
			if (bCancellationRequested)
			{
				Owner->Finalize(ECortexReplayState::Cancelled,
					CancellationResult.bSuccess ? ServiceSuccess() : CancellationResult);
			}
			else if (bInterruptionRequested)
			{
				Owner->Finalize(ECortexReplayState::Interrupted, InterruptionResult);
			}
			else
			{
				Owner->Finalize(ECortexReplayState::Completed, ServiceSuccess());
			}
		}
	}
}

void FCortexReplayService::FImpl::MarkCaptureFaulted(const FCortexCommandResult& Result)
{
	bCaptureFaulted = true;
	CaptureFaultResult = Result.bSuccess
		? ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Capture target was lost"))
		: Result;
	if (CapturePhase == ECapturePhase::Finalizing)
	{
		// A fault during teardown withholds the pending publication.
		bCapturePublishOnComplete = false;
	}
}

void FCortexReplayService::FImpl::BeginCaptureFinalization(bool bPublish)
{
	if (CapturePhase == ECapturePhase::None || CapturePhase == ECapturePhase::Finalizing)
	{
		return;
	}
	CapturePhase = ECapturePhase::Finalizing;
	bCapturePublishOnComplete = bPublish;
	bFrozen = true;
	if (Session.IsValid())
	{
		// Freeze dispatch and detach capture; only a matching owned session is ended.
		Session->ReleaseHeldInputs();
		if (bOwnedCapture)
		{
			Session->EndOwnedPIE();
		}
	}
}

void FCortexReplayService::FImpl::RetainCaptureForPublicationRetry(
	const FCortexCommandResult& PublishResult)
{
	// Keep the captured events, recording id and Finalizing phase so the publication failure stays
	// queryable and can be retried instead of silently completing. The failure is retained
	// separately from the interruption faults so the retry path is never suppressed.
	bCapturePublicationFailed = true;
	CapturePublicationError = PublishResult;
	CapturePhase = ECapturePhase::Finalizing;
	bCapturePublishOnComplete = true;
	bFrozen = true;
	LastCapturePublishAttemptSeconds = FPlatformTime::Seconds();
}

void FCortexReplayService::FImpl::ClearCapturePublicationFailure()
{
	bCapturePublicationFailed = false;
	CapturePublicationError = FCortexCommandResult();
}

void FCortexReplayService::FImpl::TickCapture()
{
	if (CapturePhase == ECapturePhase::None)
	{
		return;
	}

	if (CapturePhase == ECapturePhase::Preparing)
	{
		if (bCaptureFaulted)
		{
			BeginCaptureFinalization(false);
			TickCapture();
		}
		return;
	}

	if (CapturePhase == ECapturePhase::Recording)
	{
		if (bCaptureFaulted)
		{
			BeginCaptureFinalization(false);
			TickCapture();
			return;
		}
		// Target destruction is abnormal capture termination, never a complete recording.
		if (Session.IsValid())
		{
			FCortexCommandResult TargetError;
			if (!Session->ValidateTarget(TargetError))
			{
				MarkCaptureFaulted(TargetError);
				BeginCaptureFinalization(false);
				TickCapture();
			}
		}
		return;
	}

	// Finalizing: owned captures retain their session until the matching teardown is observed.
	if (Session.IsValid() && bOwnedCapture && !Session->IsOwnedPIEEnded())
	{
		return;
	}
	// A failed publication is retained and retried at most once per second.
	if (bCapturePublishOnComplete && !bCaptureFaulted
		&& LastCapturePublishAttemptSeconds > 0.0
		&& FPlatformTime::Seconds() - LastCapturePublishAttemptSeconds < 1.0)
	{
		return;
	}
	CompleteCaptureFinalization();
}

void FCortexReplayService::FImpl::CompleteCaptureFinalization()
{
	const bool bShouldPublish = bCapturePublishOnComplete && !bCaptureFaulted;
	if (bShouldPublish)
	{
		LastCapturePublishAttemptSeconds = FPlatformTime::Seconds();
		const FCortexCommandResult PublishResult = PublishCaptureSnapshot();
		if (!PublishResult.bSuccess)
		{
			// Ownership, the recording id and the captured events are retained (the active capture
			// status shows Finalizing plus the retained failure) and the publication is retried.
			RetainCaptureForPublicationRetry(PublishResult);
			UE_LOG(LogCortexReplay, Log, TEXT("Capture %d publication failed: %s (%s); retained for retry"),
				CaptureRecordingId, *PublishResult.ErrorCode, *PublishResult.ErrorMessage);
			return;
		}
		ClearCapturePublicationFailure();
	}
	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}
	ResetCapture();
	if (bShutdown)
	{
		DetachTickerAndSession();
	}
}

void FCortexReplayService::FImpl::ResetCapture()
{
	CapturePhase = ECapturePhase::None;
	bBorrowedCapture = false;
	bOwnedCapture = false;
	bCaptureFaulted = false;
	bCapturePublishOnComplete = false;
	CaptureFaultResult = FCortexCommandResult();
	ClearCapturePublicationFailure();
	CaptureRecordingId = 0;
	CaptureMapAssetPath.Reset();
	CaptureTargetInfo = FCortexEditorPhysicalInputTargetInfo();
	CaptureInitialPose = FCortexEditorPhysicalInputPlayerPose();
	CaptureEvents.Reset();
	CaptureEpochSeconds = 0.0;
	CaptureStopSeconds = 0.0;
	LastCapturePublishAttemptSeconds = 0.0;
	bFrozen = false;
}

FCortexCommandResult FCortexReplayService::FImpl::PublishCaptureSnapshot()
{
	FCortexReplaySnapshot CaptureSnapshot;
	CaptureSnapshot.InitialState.SchemaVersion = 1;
	CaptureSnapshot.InitialState.RecordingId = CaptureRecordingId;
	CaptureSnapshot.InitialState.PawnClassPath = CaptureTargetInfo.PawnClassPath;
	CaptureSnapshot.InitialState.Pose = CaptureInitialPose;
	CaptureSnapshot.Events = CaptureEvents;

	for (const FCortexReplayEvent& Event : CaptureSnapshot.Events)
	{
		if (Event.Guard.IsSet())
		{
			++CaptureSnapshot.Metadata.GuardCoverage.PosePresses;
			switch (Event.Guard->UICoverage)
			{
			case ECortexEditorUICoverage::Supported:
				++CaptureSnapshot.Metadata.GuardCoverage.UISupportedPresses;
				break;
			case ECortexEditorUICoverage::Unavailable:
				++CaptureSnapshot.Metadata.GuardCoverage.UIUnavailablePresses;
				break;
			default:
				++CaptureSnapshot.Metadata.GuardCoverage.UINotApplicablePresses;
				break;
			}
		}
	}

	CaptureSnapshot.Metadata.SchemaVersion = 1;
	CaptureSnapshot.Metadata.RecordingId = CaptureRecordingId;
	CaptureSnapshot.Metadata.Name = FString::Printf(TEXT("Capture %d"), CaptureRecordingId);
	CaptureSnapshot.Metadata.Description.Reset();
	CaptureSnapshot.Metadata.MapAssetPath = CaptureMapAssetPath;
	CaptureSnapshot.Metadata.EngineVersion = FEngineVersion::Current().ToString();
	CaptureSnapshot.Metadata.PluginVersion = TEXT("0.4.0");
	CaptureSnapshot.Metadata.CreatedAtUtc = FDateTime::UtcNow();
	// The recorded duration is the full monotonic capture span, not the last input timestamp.
	CaptureSnapshot.Metadata.DurationSeconds = FMath::Max(0.0, CaptureStopSeconds - CaptureEpochSeconds);
	CaptureSnapshot.Metadata.bAIEnabled = false;
	CaptureSnapshot.Metadata.bComplete = true;
	CaptureSnapshot.Metadata.Prerequisites = CaptureTargetInfo;

	return Library.Publish(CaptureSnapshot);
}

void FCortexReplayService::FImpl::OnCaptureInterruption(uint64 Generation,
	const FCortexCommandResult& Result)
{
	if (Generation != CaptureOperationGeneration || CapturePhase == ECapturePhase::None)
	{
		return;
	}
	MarkCaptureFaulted(Result);
}

void FCortexReplayService::FImpl::OnOwnedCaptureReady(uint64 Generation,
	const FCortexCommandResult& Ready)
{
	if (Generation != CaptureOperationGeneration || CapturePhase != ECapturePhase::Preparing
		|| !Session.IsValid())
	{
		return;
	}
	if (!Ready.bSuccess)
	{
		MarkCaptureFaulted(Ready);
		return;
	}

	FCortexEditorPhysicalInputPlayerPose Pose;
	const FCortexCommandResult PoseResult = Session->ReadPlayerPose(Pose);
	if (!PoseResult.bSuccess)
	{
		MarkCaptureFaulted(PoseResult);
		return;
	}

	FCortexReplayService::FImpl* RawState = this;
	const FCortexCommandResult Armed = Session->SetCaptureCallback(
		[RawState](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
			const FCortexEditorPhysicalInputCaptureContext& Context)
		{
			RawState->OnCaptureEvent(Event, TimeSeconds, Context);
		});
	if (!Armed.bSuccess)
	{
		// A rejected neutral-state arming check is abnormal termination, never Recording.
		MarkCaptureFaulted(Armed);
		return;
	}

	CaptureTargetInfo = Session->GetTargetInfo();
	CaptureMapAssetPath = CaptureTargetInfo.MapAssetPath;
	CaptureInitialPose = Pose;
	CaptureEpochSeconds = FPlatformTime::Seconds();
	CapturePhase = ECapturePhase::Recording;
}

void FCortexReplayService::FImpl::TickFinalization()
{
	if (!bFinalizing)
	{
		return;
	}

	// Owned teardown is complete only when the matching request is cancelled and the owned
	// context has disappeared. Borrowed sessions report complete without ending human PIE.
	if (Session.IsValid() && bOwnedPie && !Session->IsOwnedPIEEnded())
	{
		return;
	}

	FCortexReplayRunRecord Terminal = ActiveRun;
	Terminal.State = PendingTerminalState;
	Terminal.FinalizedAtUtc = FDateTime::UtcNow();
	if (PendingTerminalState == ECortexReplayState::Error)
	{
		Terminal.ExecutionError = PendingTerminalResult;
		if (Terminal.ExecutionError.ErrorCode.IsEmpty())
		{
			// A terminal Error must always carry a bounded execution code so the retained record
			// stays valid and reloadable.
			Terminal.ExecutionError = ServiceError(CortexReplayErrorCodes::InvalidOperation,
				TEXT("Replay execution failed"));
		}
	}
	else
	{
		Terminal.ExecutionError = FCortexCommandResult();
	}

	// Ownership is released only after the terminal record is durably indexed.
	const FCortexCommandResult Saved = RunStore.SaveTerminal(Terminal);
	if (!Saved.bSuccess)
	{
		UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s terminal persistence failed: %s (%s)"),
			*ServiceGuidToString(Terminal.Id), *Saved.ErrorCode, *Saved.ErrorMessage);
		return;
	}

	ActiveRun = Terminal;
	bTerminalPublished = true;
	bFinalizing = false;
	bRunActive = false;
	SetRunState(Terminal.State);
	UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s finalized as %s"),
		*ServiceGuidToString(ActiveRun.Id), *FString(ServiceStateToString(ActiveRun.State)));

	if (Session.IsValid())
	{
		Session->Shutdown();
		Session.Reset();
	}
	if (bShutdown)
	{
		DetachTickerAndSession();
	}
}

FCortexReplayGuardDecision FCortexReplayService::FImpl::EvaluateGuard(const FCortexReplayEvent& Event)
{
	FCortexReplayGuardDecision Decision;

	// A frozen or already-cancelled/interrupted run never evaluates further events.
	if (bFrozen || bCancellationRequested || bInterruptionRequested)
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = DispatchBlockError();
		return Decision;
	}
	if (!CheckLivePermission())
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = PermissionRevocationResult;
		return Decision;
	}

	// Validate the exact target before every guard decision.
	FCortexCommandResult TargetError;
	if (!Session.IsValid() || !Session->ValidateTarget(TargetError))
	{
		bInterruptionRequested = true;
		InterruptionResult = TargetError.bSuccess
			? ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Replay target is unavailable"))
			: TargetError;
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = InterruptionResult;
		return Decision;
	}

	FCortexEditorPhysicalInputUIObservation Observation;
	if (Event.Guard.IsSet())
	{
		const FCortexReplayInteractionGuard& Guard = Event.Guard.GetValue();
		if (Guard.UICoverage == ECortexEditorUICoverage::Supported && Guard.UITarget.IsValid())
		{
			// Live normal-route evidence is sampled before the pose.
			Session->ObserveUI(Event.Input, *Guard.UITarget, Observation);
		}
	}

	FCortexEditorPhysicalInputPlayerPose ActualPose;
	if (!Session->ReadPlayerPose(ActualPose).bSuccess)
	{
		bInterruptionRequested = true;
		InterruptionResult = ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("Replay target pose could not be read"));
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = InterruptionResult;
		return Decision;
	}

	Decision = FCortexReplayGuardEvaluator::Evaluate(Event, ActualPose, Observation,
		Session->CanWaitForUI());
	return Decision;
}

FCortexCommandResult FCortexReplayService::FImpl::DispatchEvent(const FCortexReplayEvent& Event)
{
	// Interruption/cancellation arriving during evaluation must stop the same-time event drain.
	if (bFrozen || bCancellationRequested || bInterruptionRequested)
	{
		return DispatchBlockError();
	}
	if (!CheckLivePermission())
	{
		return PermissionRevocationResult;
	}
	if (!Session.IsValid())
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable, TEXT("Replay target is gone"));
	}
	return Session->Dispatch(Event.Input);
}

void FCortexReplayService::FImpl::OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event,
	double TimeSeconds, const FCortexEditorPhysicalInputCaptureContext& Context)
{
	if (CapturePhase != ECapturePhase::Recording || bFrozen || bCaptureFaulted)
	{
		return;
	}

	FCortexReplayEvent Recorded;
	Recorded.Sequence = CaptureEvents.Num();
	Recorded.TimeSeconds = CaptureEpochSeconds > 0.0
		? FMath::Max(0.0, TimeSeconds - CaptureEpochSeconds) : 0.0;
	Recorded.Input = Event;
	Recorded.CaptureContext.FrameNumber = Context.FrameNumber;
	Recorded.CaptureContext.WorldTimeSeconds = Context.WorldTimeSeconds;
	Recorded.CaptureContext.bWorldPaused = Context.bWorldPaused;
	Recorded.CaptureContext.bTargetOwnsPointerCapture = Context.bTargetOwnsPointerCapture;

	const bool bPress = !Event.bRepeat
		&& (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::PointerDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	if (bPress)
	{
		FCortexReplayInteractionGuard Guard;
		Guard.ExpectedPose = Context.PressPose.IsSet() ? Context.PressPose.GetValue() : CaptureInitialPose;
		Guard.UICoverage = Context.UICoverage;
		Guard.UIUnavailableReason = Context.UIUnavailableReason;
		Guard.UITarget = Context.UITarget;
		Guard.ExpectedLocalPosition = Context.UILocalPosition;
		Recorded.Guard = Guard;
	}

	CaptureEvents.Add(MoveTemp(Recorded));
}

FCortexReplayService::FCortexReplayService(const FString& ProjectRoot)
	: Impl(MakeShared<FImpl>(this, ProjectRoot))
{
	Impl->EnsureTicker();
}

FCortexReplayService::~FCortexReplayService()
{
	Shutdown();
}

FCortexCommandResult FCortexReplayService::Finalize(ECortexReplayState TerminalState,
	const FCortexCommandResult& ExecutionResult)
{
	FImpl& State = *Impl;
	if (!State.bFinalizing)
	{
		State.bFinalizing = true;
		State.bFrozen = true;
		State.PendingTerminalState = TerminalState;
		State.PendingTerminalResult = ExecutionResult;

		if (State.Session.IsValid())
		{
			// Cleanup is judged against the original live ownership (world + controller), not mere
			// target readiness: a replaced pawn or unregistered viewport can still leave owned
			// input live on the original controller, while a never-bound or destroyed target has
			// nothing to neutralize.
			const FCortexEditorPhysicalInputTargetBinding& Binding = State.Session->GetTargetBinding();
			const bool bOriginalOwnershipLive = Binding.World.IsValid() && Binding.Controller.IsValid();

			const FCortexCommandResult Cleanup = State.Session->ReleaseHeldInputs();
			if (!Cleanup.bSuccess && bOriginalOwnershipLive
				&& State.PendingTerminalState != ECortexReplayState::Error)
			{
				State.PendingTerminalState = ECortexReplayState::Error;
				State.PendingTerminalResult = Cleanup;
			}
			else if (!Cleanup.bSuccess && State.PendingTerminalState == ECortexReplayState::Error
				&& State.PendingTerminalResult.ErrorCode.IsEmpty())
			{
				State.PendingTerminalResult = Cleanup;
			}
			if (State.bOwnedPie)
			{
				State.Session->EndOwnedPIE();
			}
		}
	}

	State.SetRunState(ECortexReplayState::Finalizing);
	State.EnsureTicker();
	State.TickFinalization();
	UE_LOG(LogCortexReplay, Log, TEXT("Replay run %s finalizing as %s%s%s"),
		*ServiceGuidToString(State.ActiveRun.Id),
		*FString(ServiceStateToString(State.PendingTerminalState)),
		State.PendingTerminalResult.ErrorCode.IsEmpty() ? TEXT("") : TEXT(" "),
		State.PendingTerminalResult.ErrorCode.IsEmpty()
			? TEXT("") : *State.PendingTerminalResult.ErrorCode);
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartReplay(int32 Id, ECortexReplayOrigin Origin)
{
	FImpl& State = *Impl;
	// The preparation deadline begins at native acceptance and covers snapshot validation.
	const double AcceptanceSeconds = FPlatformTime::Seconds();
	auto Refuse = [Id](const FCortexCommandResult& Result) -> FCortexCommandResult
	{
		UE_LOG(LogCortexReplay, Log, TEXT("Replay start for recording %d refused: %s (%s)"),
			Id, *Result.ErrorCode, *Result.ErrorMessage);
		return Result;
	};
	if (State.bShutdown)
	{
		return Refuse(ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("Replay service is shut down")));
	}
	if (State.bRunActive || State.bFinalizing || State.CapturePhase != FImpl::ECapturePhase::None)
	{
		return Refuse(ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target")));
	}

	TSharedPtr<const FCortexReplaySnapshot> SnapshotPtr;
	const FCortexCommandResult Loaded = State.Library.Load(Id, Origin == ECortexReplayOrigin::AI, SnapshotPtr);
	if (!Loaded.bSuccess)
	{
		return Refuse(Loaded);
	}
	if (FPlatformTime::Seconds() - AcceptanceSeconds >= ServicePreparationDeadlineSeconds)
	{
		return Refuse(ServiceError(CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
			TEXT("Replay preparation exceeded the monotonic deadline during snapshot validation")));
	}
	if (!SnapshotPtr.IsValid())
	{
		return Refuse(ServiceError(CortexReplayErrorCodes::InvalidRecording, TEXT("Recording snapshot is invalid")));
	}

	FCortexReplayRunRecord Record;
	Record.Id = FGuid::NewGuid();
	Record.RecordingId = Id;
	Record.Origin = Origin;
	Record.State = ECortexReplayState::Preparing;
	Record.EditorInstanceId = State.EditorInstanceId;
	Record.StartedAtUtc = FDateTime::UtcNow();
	Record.RecordingSnapshotSha256 = SnapshotPtr->RecordingSnapshotSha256;
	Record.InitialStateSha256 = SnapshotPtr->Metadata.InitialStateSha256;
	Record.InputsSha256 = SnapshotPtr->Metadata.InputsSha256;
	Record.TotalEvents = SnapshotPtr->Events.Num();
	Record.GuardCoverage = SnapshotPtr->Metadata.GuardCoverage;

	State.ResetRun();
	State.ActiveRun = Record;
	State.Snapshot = SnapshotPtr;
	State.bRunActive = true;
	State.bOwnedPie = true;
	State.SetRunState(ECortexReplayState::Preparing);
	State.PrepareDeadline = AcceptanceSeconds + ServicePreparationDeadlineSeconds;

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();

	FCortexReplayService::FImpl* RawState = &State;
	State.Session->SetInterruptionCallback(
		[RawState](const FCortexCommandResult& Interruption)
		{
			// Reentrant callbacks only record flags; no second tick drains a successor.
			RawState->bInterruptionRequested = true;
			RawState->InterruptionResult = Interruption;
		});

	const FCortexCommandResult Accepted = State.Session->BeginOwnedPIE(
		SnapshotPtr->Metadata.MapAssetPath,
		SnapshotPtr->Metadata.Prerequisites.LocalPlayerIndex,
		[RawState](const FCortexCommandResult& Ready)
		{
			RawState->bReadySeen = true;
			RawState->PreparationResult = Ready;
		});
	if (!Accepted.bSuccess)
	{
		// Acceptance failures still become this run's terminal Error result.
		State.bReadySeen = true;
		State.PreparationResult = Accepted;
	}

	State.EnsureTicker();

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), ServiceGuidToString(State.ActiveRun.Id));
	Data->SetNumberField(TEXT("recording_id"), Id);
	Data->SetStringField(TEXT("editor_instance_id"), State.EditorInstanceId);
	Data->SetStringField(TEXT("state"), ServiceStateToString(State.RunState));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::GetRun(const FGuid& Id, bool bAIOnly) const
{
	const FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && (State.bRunActive || State.bFinalizing || State.bTerminalPublished))
	{
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, State.bRunActive
			&& State.RunState == ECortexReplayState::Replaying, State.Scheduler.Get()));
	}

	FCortexReplayRunRecord Record;
	if (State.RunStore.Load(Id, ECortexReplayOrigin::AI, Record).bSuccess)
	{
		return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
	}
	// An unrestricted human query resolves retained runs of either origin.
	if (!bAIOnly && State.RunStore.Load(Id, ECortexReplayOrigin::Human, Record).bSuccess)
	{
		return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
	}
	return ServiceError(CortexReplayErrorCodes::RunNotFound,
		FString::Printf(TEXT("No replay run %s"), *ServiceGuidToString(Id)));
}

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

	if (Record.State == ECortexReplayState::Error && !Record.ExecutionError.ErrorCode.IsEmpty())
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

FCortexCommandResult FCortexReplayService::CancelReplay(const FGuid& Id, bool bAIOnly)
{
	FImpl& State = *Impl;
	const bool bMatchesActive = State.ActiveRun.Id == Id
		&& (!bAIOnly || State.ActiveRun.Origin == ECortexReplayOrigin::AI);
	if (bMatchesActive && State.bRunActive)
	{
		State.bCancellationRequested = true;
		State.CancellationResult = ServiceSuccess();
		Finalize(ECortexReplayState::Cancelled, ServiceSuccess());
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, true, State.Scheduler.Get()));
	}
	if (bMatchesActive && State.bTerminalPublished)
	{
		return FCortexCommandRouter::Success(BuildRunData(State.ActiveRun, false, nullptr));
	}

	FCortexReplayRunRecord Record;
	if (State.RunStore.Load(Id, ECortexReplayOrigin::AI, Record).bSuccess)
	{
		return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
	}
	if (!bAIOnly && State.RunStore.Load(Id, ECortexReplayOrigin::Human, Record).bSuccess)
	{
		return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
	}
	return ServiceError(CortexReplayErrorCodes::RunNotFound,
		FString::Printf(TEXT("No replay run %s"), *ServiceGuidToString(Id)));
}

FCortexCommandResult FCortexReplayService::GetLastRunForRecording(int32 Id) const
{
	bool bFound = false;
	FCortexReplayRunRecord Record;
	const FCortexCommandResult Loaded = Impl->RunStore.GetLatestForRecording(Id, bFound, Record);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}
	if (!bFound)
	{
		return ServiceSuccess();
	}
	return FCortexCommandRouter::Success(BuildRunData(Record, false, nullptr));
}

FCortexCommandResult FCortexReplayService::GetCurrentOperation() const
{
	const FImpl& State = *Impl;
	if (!State.bRunActive && State.CapturePhase == FImpl::ECapturePhase::None)
	{
		return ServiceSuccess();
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	if (State.bRunActive)
	{
		Data->SetStringField(TEXT("kind"), TEXT("replay"));
		Data->SetStringField(TEXT("origin"), ServiceOriginToString(State.ActiveRun.Origin));
		Data->SetNumberField(TEXT("recording_id"), State.ActiveRun.RecordingId);
		Data->SetStringField(TEXT("run_id"), ServiceGuidToString(State.ActiveRun.Id));
		Data->SetStringField(TEXT("state"), ServiceStateToString(State.RunState));
		Data->SetObjectField(TEXT("guard_coverage"), ServiceGuardCoverageToJson(State.ActiveRun.GuardCoverage));
		Data->SetNumberField(TEXT("dispatched_events"), State.ActiveRun.DispatchedEvents);
		Data->SetNumberField(TEXT("total_events"), State.ActiveRun.TotalEvents);
		Data->SetNumberField(TEXT("authorized_wait_seconds"), State.ActiveRun.AuthorizedWaitSeconds);
		if (State.Scheduler.IsValid() && State.Scheduler->GetWaitingSequence() != INDEX_NONE)
		{
			Data->SetObjectField(TEXT("waiting"), ServiceWaitingToJson(*State.Scheduler));
		}
		else
		{
			Data->SetField(TEXT("waiting"), MakeShared<FJsonValueNull>());
		}
	}
	else
	{
		const TCHAR* CaptureState =
			State.CapturePhase == FImpl::ECapturePhase::Preparing ? TEXT("Preparing")
			: State.CapturePhase == FImpl::ECapturePhase::Finalizing ? TEXT("Finalizing")
			: TEXT("Recording");
		Data->SetStringField(TEXT("kind"), TEXT("capture"));
		Data->SetStringField(TEXT("origin"), State.bBorrowedCapture ? TEXT("human") : TEXT("ai"));
		Data->SetNumberField(TEXT("recording_id"), State.CaptureRecordingId);
		Data->SetStringField(TEXT("state"), CaptureState);
		// A pending or failed publication is visible to the human window while retries are pending.
		Data->SetBoolField(TEXT("publication_pending"),
			State.bCapturePublishOnComplete && !State.bCaptureFaulted);
		Data->SetBoolField(TEXT("publication_failed"), State.bCapturePublicationFailed);
		if (State.bCapturePublicationFailed)
		{
			TSharedRef<FJsonObject> PublicationError = MakeShared<FJsonObject>();
			PublicationError->SetStringField(TEXT("code"), State.CapturePublicationError.ErrorCode);
			PublicationError->SetStringField(TEXT("message"), State.CapturePublicationError.ErrorMessage);
			Data->SetObjectField(TEXT("publication_error"), PublicationError);
		}
		else
		{
			Data->SetField(TEXT("publication_error"), MakeShared<FJsonValueNull>());
		}
	}
	return FCortexCommandRouter::Success(Data);
}

bool FCortexReplayService::IsRecordInUse(int32 Id) const
{
	const FImpl& State = *Impl;
	if (State.CapturePhase != FImpl::ECapturePhase::None && State.CaptureRecordingId == Id)
	{
		return true;
	}
	if ((State.bRunActive || State.bFinalizing) && State.ActiveRun.RecordingId == Id)
	{
		return true;
	}
	return false;
}

FCortexCommandResult FCortexReplayService::EnumerateHumanCaptureTargets(
	TArray<FCortexReplayCaptureTargetChoice>& Out) const
{
	Out.Reset();
	if (!GEngine)
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("No engine world contexts are available"));
	}

	int32 Resolved = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (Context.WorldType != EWorldType::PIE || World == nullptr)
		{
			continue;
		}
		// Viewport existence alone is not readiness: the client must expose a registered scene
		// viewport and a game layer manager, and each candidate is validated per local player.
		UGameViewportClient* ViewportClient = World->GetGameViewport();
		if (ViewportClient == nullptr || ViewportClient->GetGameViewport() == nullptr
			|| !ViewportClient->GetGameLayerManager().IsValid())
		{
			continue;
		}
		UGameInstance* GameInstance = World->GetGameInstance();
		if (GameInstance == nullptr)
		{
			continue;
		}

		const int32 LocalPlayerCount = GameInstance->GetNumLocalPlayers();
		for (int32 LocalPlayerIndex = 0; LocalPlayerIndex < LocalPlayerCount; ++LocalPlayerIndex)
		{
			ULocalPlayer* LocalPlayer = GameInstance->GetLocalPlayerByIndex(LocalPlayerIndex);
			if (LocalPlayer == nullptr)
			{
				continue;
			}
			APlayerController* Controller = LocalPlayer->GetPlayerController(World);
			if (!IsValid(Controller) || !IsValid(Controller->GetPawn()))
			{
				continue;
			}

			FCortexReplayCaptureTargetChoice Choice;
			Choice.World = World;
			Choice.LocalPlayerIndex = LocalPlayerIndex;
			Choice.MapAssetPath = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
			Choice.ViewportLabel = FString::Printf(TEXT("%s [PIE player %d]"),
				*Choice.MapAssetPath, LocalPlayerIndex);
			Out.Add(MoveTemp(Choice));
			++Resolved;
		}
	}

	if (Resolved == 0)
	{
		return ServiceError(CortexReplayErrorCodes::TargetUnavailable,
			TEXT("No ready PIE local-player/viewport candidate could be resolved"));
	}
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartCaptureAtTarget(UWorld& World, int32 LocalPlayerIndex)
{
	FImpl& State = *Impl;
	if (State.bShutdown)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("Replay service is shut down"));
	}
	if (State.bRunActive || State.bFinalizing || State.CapturePhase != FImpl::ECapturePhase::None)
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target"));
	}

	// Reserve capture ownership before any asynchronous work so status, IsRecordInUse and
	// competing admission all see the operation from its first frame.
	State.ResetCapture();
	State.CapturePhase = FImpl::ECapturePhase::Preparing;
	State.bBorrowedCapture = true;
	State.bOwnedCapture = false;
	++State.CaptureOperationGeneration;
	const uint64 Generation = State.CaptureOperationGeneration;

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();
	FCortexReplayService::FImpl* RawState = &State;
	State.Session->SetInterruptionCallback(
		[RawState, Generation](const FCortexCommandResult& Interruption)
		{
			RawState->OnCaptureInterruption(Generation, Interruption);
		});

	const FCortexCommandResult Bound = State.Session->BindTarget(World, LocalPlayerIndex);
	if (!Bound.bSuccess)
	{
		State.Session->Shutdown();
		State.Session.Reset();
		State.ResetCapture();
		return Bound;
	}

	int32 ReservedId = 0;
	const FCortexCommandResult Reserved = State.Library.ReserveId(ReservedId);
	if (!Reserved.bSuccess)
	{
		State.Session->Shutdown();
		State.Session.Reset();
		State.ResetCapture();
		return Reserved;
	}
	State.CaptureRecordingId = ReservedId;
	State.CaptureEvents.Reset();

	FCortexEditorPhysicalInputPlayerPose Pose;
	const FCortexCommandResult PoseResult = State.Session->ReadPlayerPose(Pose);
	if (!PoseResult.bSuccess)
	{
		State.MarkCaptureFaulted(PoseResult);
		State.BeginCaptureFinalization(false);
		State.TickCapture();
		return PoseResult;
	}

	const FCortexCommandResult Armed = State.Session->SetCaptureCallback(
		[RawState](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
			const FCortexEditorPhysicalInputCaptureContext& Context)
		{
			RawState->OnCaptureEvent(Event, TimeSeconds, Context);
		});
	if (!Armed.bSuccess)
	{
		// A human-held key makes the neutral-state arming check reject this; never report Recording.
		State.MarkCaptureFaulted(Armed);
		State.BeginCaptureFinalization(false);
		State.TickCapture();
		return Armed;
	}

	State.CaptureTargetInfo = State.Session->GetTargetInfo();
	State.CaptureMapAssetPath = State.CaptureTargetInfo.MapAssetPath;
	State.CaptureInitialPose = Pose;
	State.CaptureEpochSeconds = FPlatformTime::Seconds();
	State.CapturePhase = FImpl::ECapturePhase::Recording;
	State.EnsureTicker();
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StartCapture(const FString& SavedEditorMapAssetPath)
{
	FImpl& State = *Impl;
	if (State.bShutdown)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation, TEXT("Replay service is shut down"));
	}
	if (State.bRunActive || State.bFinalizing || State.CapturePhase != FImpl::ECapturePhase::None)
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Another replay operation owns the target"));
	}

	int32 ReservedId = 0;
	const FCortexCommandResult Reserved = State.Library.ReserveId(ReservedId);
	if (!Reserved.bSuccess)
	{
		return Reserved;
	}

	// Capture ownership is reserved for the whole owned preparation, not only after readiness.
	State.ResetCapture();
	State.CapturePhase = FImpl::ECapturePhase::Preparing;
	State.bBorrowedCapture = false;
	State.bOwnedCapture = true;
	State.CaptureRecordingId = ReservedId;
	++State.CaptureOperationGeneration;
	const uint64 Generation = State.CaptureOperationGeneration;

	State.Session = MakeShared<FCortexEditorPhysicalInputSession>();
	FCortexReplayService::FImpl* RawState = &State;
	State.Session->SetInterruptionCallback(
		[RawState, Generation](const FCortexCommandResult& Interruption)
		{
			RawState->OnCaptureInterruption(Generation, Interruption);
		});

	const FCortexCommandResult Accepted = State.Session->BeginOwnedPIE(SavedEditorMapAssetPath, 0,
		[RawState, Generation](const FCortexCommandResult& Ready)
		{
			RawState->OnOwnedCaptureReady(Generation, Ready);
		});
	if (!Accepted.bSuccess)
	{
		State.MarkCaptureFaulted(Accepted);
		State.BeginCaptureFinalization(false);
		State.TickCapture();
		return Accepted;
	}

	State.EnsureTicker();
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::StopCapture(bool bAbnormal)
{
	FImpl& State = *Impl;
	if (State.CapturePhase == FImpl::ECapturePhase::None)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("No capture is active"));
	}
	// A capture retained in Finalizing with a failed asynchronous publication reports that retained
	// failure; it is not "inactive" while the publication retry is still outstanding.
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing && State.bCapturePublicationFailed)
	{
		return State.CapturePublicationError;
	}
	if (State.CapturePhase == FImpl::ECapturePhase::Finalizing)
	{
		return ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("No capture is active"));
	}

	// Duration is the full monotonic capture span, sampled before any cleanup; a capture aborted
	// before it began recording is never published.
	State.CaptureStopSeconds = FPlatformTime::Seconds();
	const bool bPublish = !bAbnormal && !State.bCaptureFaulted
		&& State.CapturePhase == FImpl::ECapturePhase::Recording;

	if (State.bBorrowedCapture)
	{
		// Borrowed capture detaches without ending the human PIE session.
		State.bFrozen = true;
		if (State.Session.IsValid())
		{
			State.Session->ReleaseHeldInputs();
			State.Session->Shutdown();
			State.Session.Reset();
		}
		if (bPublish)
		{
			const FCortexCommandResult PublishResult = State.PublishCaptureSnapshot();
			if (!PublishResult.bSuccess)
			{
				// The captured data and recording id are retained so the failure is queryable and
				// the publication is retried instead of silently completing.
				State.RetainCaptureForPublicationRetry(PublishResult);
				State.EnsureTicker();
				return PublishResult;
			}
		}
		State.ResetCapture();
		if (State.bShutdown)
		{
			State.DetachTickerAndSession();
		}
		return ServiceSuccess();
	}

	// Owned capture retains its session and ownership until the matching teardown is observed.
	State.BeginCaptureFinalization(bPublish);
	State.TickCapture();
	// A publication attempt that already failed (and whose retries are outstanding) is reported
	// instead of a plain success; otherwise success means the stop was accepted and publication is
	// still pending, which the capture status exposes.
	if (State.bCapturePublicationFailed)
	{
		return State.CapturePublicationError;
	}
	return ServiceSuccess();
}

FCortexCommandResult FCortexReplayService::GetRecording(int32 Id, bool bAIOnly) const
{
	TSharedPtr<const FCortexReplaySnapshot> LoadedSnapshot;
	const FCortexCommandResult Loaded = Impl->Library.Load(Id, bAIOnly, LoadedSnapshot);
	if (!Loaded.bSuccess)
	{
		return Loaded;
	}

	TSharedRef<FJsonObject> Data = ServiceMetadataToJson(LoadedSnapshot->Metadata);
	TSharedRef<FJsonObject> InitialState = MakeShared<FJsonObject>();
	InitialState->SetNumberField(TEXT("schema_version"), LoadedSnapshot->InitialState.SchemaVersion);
	InitialState->SetNumberField(TEXT("recording_id"), LoadedSnapshot->InitialState.RecordingId);
	InitialState->SetStringField(TEXT("pawn_class_path"), LoadedSnapshot->InitialState.PawnClassPath);

	const FCortexEditorPhysicalInputPlayerPose& Pose = LoadedSnapshot->InitialState.Pose;
	const FVector Location = Pose.PawnTransform.GetLocation();
	TSharedRef<FJsonObject> PawnTransform = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	PawnTransform->SetObjectField(TEXT("location_cm"), LocationJson);
	const FRotator PawnRotation = Pose.PawnTransform.Rotator();
	TSharedRef<FJsonObject> PawnRotationJson = MakeShared<FJsonObject>();
	PawnRotationJson->SetNumberField(TEXT("pitch"), PawnRotation.Pitch);
	PawnRotationJson->SetNumberField(TEXT("yaw"), PawnRotation.Yaw);
	PawnRotationJson->SetNumberField(TEXT("roll"), PawnRotation.Roll);
	PawnTransform->SetObjectField(TEXT("rotation_deg"), PawnRotationJson);
	const FVector Scale = Pose.PawnTransform.GetScale3D();
	TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	PawnTransform->SetObjectField(TEXT("scale"), ScaleJson);
	InitialState->SetObjectField(TEXT("pawn_transform"), PawnTransform);

	TSharedRef<FJsonObject> ControlRotation = MakeShared<FJsonObject>();
	ControlRotation->SetNumberField(TEXT("pitch"), Pose.ControlRotation.Pitch);
	ControlRotation->SetNumberField(TEXT("yaw"), Pose.ControlRotation.Yaw);
	ControlRotation->SetNumberField(TEXT("roll"), Pose.ControlRotation.Roll);
	InitialState->SetObjectField(TEXT("control_rotation_deg"), ControlRotation);

	Data->SetObjectField(TEXT("initial_state"), InitialState);
	Data->SetStringField(TEXT("recording_snapshot_sha256"), LoadedSnapshot->RecordingSnapshotSha256);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListRecordings(int32 AfterId, int32 PageSize) const
{
	TArray<FCortexReplayMetadata> Metadata;
	bool bHasMore = false;
	// Discovery is AI-only eligibility; human listing keeps its unrestricted API.
	const FCortexCommandResult Listed = Impl->Library.ListPage(true, AfterId, PageSize, Metadata, bHasMore);
	if (!Listed.bSuccess)
	{
		return Listed;
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Metadata.Num());
	for (const FCortexReplayMetadata& Row : Metadata)
	{
		Rows.Add(MakeShared<FJsonValueObject>(ServiceMetadataToJson(Row)));
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("editor_instance_id"), Impl->EditorInstanceId);
	Data->SetArrayField(TEXT("recordings"), Rows);
	Data->SetBoolField(TEXT("has_more"), bHasMore);
	if (bHasMore && Metadata.Num() > 0)
	{
		Data->SetNumberField(TEXT("next_after_recording_id"), Metadata.Last().RecordingId);
	}
	else
	{
		Data->SetField(TEXT("next_after_recording_id"), MakeShared<FJsonValueNull>());
	}

	TArray<FCortexReplayRunRecord> Recent;
	Impl->RunStore.ListRecent(ECortexReplayOrigin::AI, ServiceRecentTerminalRunLimit,
		ServiceRecentTerminalWindowSeconds, Recent);
	TArray<TSharedPtr<FJsonValue>> RecentRows;
	RecentRows.Reserve(Recent.Num());
	for (const FCortexReplayRunRecord& Run : Recent)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("run_id"), ServiceGuidToString(Run.Id));
		Summary->SetNumberField(TEXT("recording_id"), Run.RecordingId);
		Summary->SetStringField(TEXT("editor_instance_id"), Run.EditorInstanceId);
		Summary->SetStringField(TEXT("started_at_utc"), ServiceIsoUtc(Run.StartedAtUtc));
		Summary->SetStringField(TEXT("finalized_at_utc"), ServiceIsoUtc(Run.FinalizedAtUtc));
		Summary->SetStringField(TEXT("state"), ServiceStateToString(Run.State));
		RecentRows.Add(MakeShared<FJsonValueObject>(Summary));
	}
	Data->SetArrayField(TEXT("recent_ai_runs"), RecentRows);
	Data->SetNumberField(TEXT("recovery_window_seconds"), ServiceRecentTerminalWindowSeconds);
	Data->SetNumberField(TEXT("recovery_max_terminal_runs"), ServiceRecentTerminalRunLimit);
	if (Impl->bRunActive && Impl->ActiveRun.Origin == ECortexReplayOrigin::AI)
	{
		Data->SetField(TEXT("active_ai_run"), MakeShared<FJsonValueObject>(
			BuildRunData(Impl->ActiveRun, true, Impl->Scheduler.Get())));
	}
	else
	{
		Data->SetField(TEXT("active_ai_run"), MakeShared<FJsonValueNull>());
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexReplayService::ListHumanRecordings(
	TArray<FCortexReplayMetadata>& Out) const
{
	return Impl->Library.List(false, Out);
}

int64 FCortexReplayService::GetLibraryRevision() const
{
	return Impl->Library.GetRevision();
}

FCortexCommandResult FCortexReplayService::SaveMetadata(int32 Id, const FString& Name,
	const FString& Description, bool bAIEnabled)
{
	FImpl& State = *Impl;
	const FCortexCommandResult Saved = State.Library.SaveMetadata(Id, Name, Description, bAIEnabled);
	if (!Saved.bSuccess)
	{
		return Saved;
	}

	// Revocation cancels only a still-active AI run of that recording.
	if (!bAIEnabled && State.ActiveRun.Origin == ECortexReplayOrigin::AI
		&& State.ActiveRun.RecordingId == Id)
	{
		State.bPermissionRevoked = true;
		State.PermissionRevocationResult = ServiceError(CortexReplayErrorCodes::PermissionDenied,
			TEXT("Replay permission was revoked"));
		if (State.bRunActive && !State.bFinalizing)
		{
			State.bCancellationRequested = true;
			State.CancellationResult = State.PermissionRevocationResult;
			Finalize(ECortexReplayState::Cancelled, State.PermissionRevocationResult);
		}
	}
	return Saved;
}

FCortexCommandResult FCortexReplayService::DeleteRecording(int32 Id)
{
	// Refusal is side-effect-free; an actual external deletion during playback is picked up by the
	// live eligibility check, which cancels only an AI run whose recording disappeared.
	if (IsRecordInUse(Id))
	{
		return ServiceError(CortexErrorCodes::EditorBusy, TEXT("Recording is in use"));
	}
	return Impl->Library.Delete(Id);
}

void FCortexReplayService::Shutdown()
{
	if (!Impl.IsValid())
	{
		return;
	}
	FImpl& State = *Impl;
	if (State.bShutdown)
	{
		return;
	}
	State.bShutdown = true;

	// An active capture terminates abnormally and never publishes a partial record.
	if (State.CapturePhase != FImpl::ECapturePhase::None
		&& State.CapturePhase != FImpl::ECapturePhase::Finalizing)
	{
		State.MarkCaptureFaulted(ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("Replay service shut down")));
		State.BeginCaptureFinalization(false);
		State.TickCapture();
	}

	// An active run is routed through the single finalizer so exactly one terminal record is
	// retained, instead of dropping ownership without a persisted result.
	if (State.bRunActive && !State.bFinalizing)
	{
		State.bCancellationRequested = true;
		State.CancellationResult = ServiceError(CortexReplayErrorCodes::InvalidOperation,
			TEXT("Replay service shut down"));
		Finalize(ECortexReplayState::Cancelled, State.CancellationResult);
	}
	else if (State.bFinalizing)
	{
		State.TickFinalization();
	}

	// The finalization ticker (and the session it observes) is retained until the owned teardown
	// completes; only then is everything released.
	if (!State.bRunActive && !State.bFinalizing && State.CapturePhase == FImpl::ECapturePhase::None)
	{
		State.DetachTickerAndSession();
	}
}
