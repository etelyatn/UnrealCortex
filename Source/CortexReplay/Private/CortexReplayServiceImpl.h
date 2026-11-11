#pragma once

#include "CortexReplayService.h"

#include "CortexCommandRouter.h"
#include "CortexEditorEngineFrameObserver.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayFrameClock.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayRunStore.h"
#include "CortexReplayScheduler.h"
#include "CortexReplayTypes.h"

#include "Application/ThrottleManager.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SCortexReplayRunOverlay;
class UGameViewportClient;
class UWorld;

/** Z-order of the run overlay in the game viewport's widget stack. */
inline constexpr int32 ReplayOverlayZOrder = 1000;

/** The hands-off interval between a replay run's readiness and its first replayed frame. */
inline constexpr double ReplayCountdownSeconds = 3.0;

/**
 * Helpers shared by the service translation units (facade, capture, run, JSON). Helpers used by a
 * single translation unit stay in that file's anonymous namespace.
 */
namespace CortexReplayServicePrivate
{
inline FString ServiceGuidToString(const FGuid& Id)
{
	return Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

inline const TCHAR* ServiceStateToString(ECortexReplayState State)
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

inline const TCHAR* ServiceOriginToString(ECortexReplayOrigin Origin)
{
	return Origin == ECortexReplayOrigin::AI ? TEXT("ai") : TEXT("human");
}

inline FString ServiceIsoUtc(const FDateTime& Time)
{
	return Time.GetTicks() > 0 ? Time.ToIso8601() : FString();
}

inline FCortexCommandResult ServiceSuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}

inline FCortexCommandResult ServiceError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}

inline FCortexCommandResult ServiceError(const FString& Code, const FString& Message)
{
	return FCortexCommandRouter::Error(Code, Message);
}

TSharedRef<FJsonObject> ServiceGuardCoverageToJson(const FCortexReplayGuardCoverage& Coverage);
TSharedRef<FJsonObject> ServiceMetadataToJson(const FCortexReplayMetadata& Metadata);
/** Bounded waiting/progress object shared by get_run and the human current-operation status. */
TSharedRef<FJsonObject> ServiceWaitingToJson(const FCortexReplayScheduler& Scheduler);
}

/**
 * All mutable service state lives here so the public header stays declaration-only.
 *
 * The finalization ticker captures a shared reference to this state, so an asynchronous owned
 * teardown can finish (and persist its terminal record) after the service object itself is
 * destroyed without any tick touching freed memory.
 *
 * Members and methods are grouped by owner translation unit: shared (CortexReplayService.cpp),
 * capture (CortexReplayServiceCapture.cpp), run and frame clock (CortexReplayServiceRun.cpp).
 */
struct FCortexReplayService::FImpl : public TSharedFromThis<FCortexReplayService::FImpl>
{
	// ---- shared ----
	explicit FImpl(FCortexReplayService* InOwner, const FString& InProjectRoot)
		: Owner(InOwner)
		, ProjectRoot(InProjectRoot)
		, Library(InProjectRoot)
		, RunStore(InProjectRoot)
		, EditorInstanceId(CortexReplayServicePrivate::ServiceGuidToString(FGuid::NewGuid()))
	{
		RunStore.Initialize();
	}

	~FImpl()
	{
		RemoveFrameHooks();
	}

	FCortexReplayService* Owner = nullptr;
	FString ProjectRoot;
	FCortexReplayLibrary Library;
	FCortexReplayRunStore RunStore;
	FString EditorInstanceId;

	TSharedPtr<FCortexEditorPhysicalInputSession> Session;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bShutdown = false;
	/**
	 * Set by ShutdownForModuleUnload: finalization completes now instead of on a later engine frame,
	 * because this module's code is about to be freed (the owned PIE teardown continues in CortexEditor).
	 */
	bool bUnloading = false;
	/** Raised once when a run's terminal record is published; see FCortexReplayService::OnRunTerminal. */
	FOnCortexReplayRunTerminal RunTerminalDelegate;

	bool Tick(float DeltaSeconds);
	void EnsureTicker();
	void DetachTickerAndSession();
	void SetRunState(ECortexReplayState NewState);

	// ---- capture ----
	/** The single capture operation's phase; capture ownership spans preparation through teardown. */
	enum class ECapturePhase : uint8 { None, Preparing, Recording, Finalizing };
	ECapturePhase CapturePhase = ECapturePhase::None;
	/** Scopes async capture callbacks to the exact operation that started them. */
	uint64 CaptureOperationGeneration = 0;
	bool bOwnedCapture = false;
	bool bCaptureFaulted = false;
	bool bCapturePublishOnComplete = false;
	FCortexCommandResult CaptureFaultResult;
	int32 CaptureRecordingId = 0;
	FString CaptureMapAssetPath;
	FCortexEditorPhysicalInputTargetInfo CaptureTargetInfo;
	FCortexEditorPhysicalInputPlayerPose CaptureInitialPose;
	TArray<FCortexReplayEvent> CaptureEvents;
	/** Format-3 frames: the engine frames that consumed input after the capture epoch. */
	TArray<FCortexReplayFrame> CaptureFrames;
	/** Attribution stamped at an observed input boundary, until that engine frame closes. */
	struct FCaptureOpenFrame
	{
		uint64 CaptureFrameCounter = 0;
		int32 FirstSequence = 0;
		int32 EventCount = 0;
	};
	TOptional<FCaptureOpenFrame> CaptureOpenFrame;
	/** First event sequence the next observed input boundary attributes. */
	int32 CaptureNextFirstSequence = 0;
	FCortexReplayViewportState CaptureViewport;
	TUniquePtr<FCortexEditorEngineFrameObserver> CaptureFrameObserver;
	/** Holds the Slate throttle disabled for an owned capture; balanced on every release path. */
	struct FThrottleHold
	{
		FThrottleHold() = default;
		FThrottleHold(const FThrottleHold&) = delete;
		FThrottleHold& operator=(const FThrottleHold&) = delete;
		~FThrottleHold() { Release(); }
		void Acquire()
		{
			if (!bHeld)
			{
				FSlateThrottleManager::Get().DisableThrottle(true);
				bHeld = true;
			}
		}
		void Release()
		{
			if (bHeld)
			{
				FSlateThrottleManager::Get().DisableThrottle(false);
				bHeld = false;
			}
		}
		bool bHeld = false;
	};
	FThrottleHold CaptureThrottle;
	/** Throttle for owned-capture publication retries after a storage/validation failure. */
	double LastCapturePublishAttemptSeconds = 0.0;
	/**
	 * Retained capture publication failure, kept separate from the interruption faults
	 * (`bCaptureFaulted`/`CaptureFaultResult`) so it never suppresses the publication retry.
	 */
	bool bCapturePublicationFailed = false;
	FCortexCommandResult CapturePublicationError;
	/**
	 * False only when an external session end sampled the captured stream as ending on a held key or
	 * button. Such an incomplete stream is never published as a playable recording; the ordinary
	 * (widget) Stop path never consults this flag.
	 */
	bool bCaptureStreamBalanced = true;
	/**
	 * Names of the presses still held when an external session end sampled the stream as unbalanced.
	 * Diagnostic and human-visible reason only; never published.
	 */
	TArray<FString> CaptureStreamHeldInputs;
	/**
	 * Latest retained terminal capture outcome for the human window only: the JSON summary of the
	 * last completed capture, or null until one completes. Cleared when the next capture is admitted
	 * and never reported as the active operation.
	 */
	TSharedPtr<FJsonObject> LastCaptureOutcome;


	void TickCapture();
	void ResetCapture();
	/**
	 * Uninstalls and drops the capture's frame observer. It must run before its session is released:
	 * the observer detaches from that exact session when destroyed, and Uninstall clears the link.
	 */
	void ReleaseCaptureFrameObserver();
	void MarkCaptureFaulted(const FCortexCommandResult& Result);
	void HandleCaptureTargetLoss(const FCortexCommandResult& TargetError);
	void BeginCaptureFinalization(bool bPublish);
	void RetainCaptureForPublicationRetry(const FCortexCommandResult& PublishResult);
	void ClearCapturePublicationFailure();
	void CompleteCaptureFinalization();
	/** Retains a completed capture's terminal summary for the human window and clears retry state. */
	void StoreLastCaptureOutcome(bool bPublished, const FCortexCommandResult* PublicationError);
	/** Builds the retained terminal-capture summary JSON (human-UI status only). */
	TSharedRef<FJsonObject> BuildLastCaptureOutcomeJson(bool bPublished,
		const FCortexCommandResult* PublicationError) const;
	void ClearLastCaptureOutcome();
	FCortexCommandResult PublishCaptureSnapshot();
	void OnCaptureInterruption(uint64 Generation, const FCortexCommandResult& Result);
	void OnOwnedCaptureReady(uint64 Generation, const FCortexCommandResult& Ready);
	void OnCaptureEvent(const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
		const FCortexEditorPhysicalInputCaptureContext& Context);
	/** Stamps the attribution of the frame whose input boundary was just observed. */
	void OnCaptureInputBoundary(uint64 Generation, const FCortexEditorEngineFrameRecord& Boundary);
	/** Records the closed engine frame, or faults the capture when the observer invalidated it. */
	void OnCaptureFrameClosed(uint64 Generation, const FCortexEditorEngineFrameRecord& Closed);
	/** Appends one frame (delta bound checked) from the observed record and its stamped attribution. */
	bool AppendCaptureFrame(const FCortexEditorEngineFrameRecord& Observed);
	/**
	 * Freezes the stream's frame set when capture stops: records the open frame when it ticked the
	 * world and drops events after the last recorded boundary. Idempotent.
	 */
	void SealCaptureFrames();
	/** Removes a trailing unreleased press of the editor's stop-play chord from the sealed stream. */
	void DropTrailingStopChordPress();
	/** Stable short name used by capture lifecycle logging and the discard outcome message. */
	static const TCHAR* CapturePhaseToString(ECapturePhase Phase);

	// ---- countdown ----
	/** Service configuration (survives ResetRun): the hands-off interval between readiness and playback. */
	double CountdownSeconds = ReplayCountdownSeconds;

	/** True while the run is Preparing and its hands-off countdown is running. */
	bool IsInCountdown() const;
	/** Seconds of the countdown left, clamped to zero; zero when no countdown is running. */
	double GetCountdownRemainingSeconds() const;

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
	/** Monotonic start of the hands-off countdown; 0 until the run's checks pass and it begins. */
	double CountdownStartedSeconds = 0.0;
	bool bReadySeen = false;
	FCortexCommandResult PreparationResult;
	bool bCancellationRequested = false;
	FCortexCommandResult CancellationResult;
	bool bInterruptionRequested = false;
	FCortexCommandResult InterruptionResult;
	bool bPermissionRevoked = false;
	FCortexCommandResult PermissionRevocationResult;
	/** GFrameCounter of the engine frame whose live permission was last read; one read per engine frame. */
	uint64 PermissionCheckedFrame = TNumericLimits<uint64>::Max();
	ECortexReplayState PendingTerminalState = ECortexReplayState::Cancelled;
	FCortexCommandResult PendingTerminalResult;
	/** Terminal persistence retries wait one second after a failed write. */
	bool bTerminalSaveFailed = false;
	double LastTerminalSaveAttemptSeconds = 0.0;

	// ---- game-view overlay ----
	/** The countdown/banner overlay on the owned PIE's game viewport; null when none is shown. */
	TSharedPtr<SCortexReplayRunOverlay> RunOverlay;
	/** The viewport holding the overlay; weak, so a torn-down world never leaves a dangling pointer. */
	TWeakObjectPtr<UGameViewportClient> OverlayViewport;
	/**
	 * Adds the overlay to the target world's game viewport if it is not there yet. A missing world or
	 * viewport leaves the run without an overlay and never affects the run.
	 */
	void ShowRunOverlay();
	/** Shows the countdown number for the time left; creates the overlay on first use. */
	void UpdateCountdownOverlay();
	/** Switches the overlay to the playing banner; creates the overlay when no countdown ran. */
	void ShowPlayingOverlay();
	/** Removes the overlay from its viewport (when that still exists) and forgets it. */
	void RemoveRunOverlay();

	void TickRun();
	void TickFinalization();
	void ResetRun();
	/**
	 * The one live predicate: a run reports its frame, waiting and progress only while it is replaying
	 * and not yet finalizing. get_run, cancel_replay and active_ai_run all use it.
	 */
	bool IsLiveReplaying() const;
	/** The active run's wire object, built with the live predicate. */
	TSharedRef<FJsonObject> BuildActiveRunData() const;
	/** Adds `phase` (countdown while counting, playback while replaying) and the countdown time left. */
	void AddPhaseFields(FJsonObject& Data) const;
	/**
	 * After readiness and before the epoch: compares the recording's viewport size, DPI, player-input
	 * class, input-config identity and viewport capture/high-precision state with the live owned target.
	 * INCOMPATIBLE_PREREQUISITES names the field and both values.
	 */
	FCortexCommandResult CheckLivePrerequisites() const;
	FCortexReplayGuardDecision EvaluateGuard(const FCortexReplayEvent& Event);
	FCortexCommandResult DispatchEvent(const FCortexReplayEvent& Event);
	FCortexCommandResult DispatchBlockError() const;
	/** Re-reads the live AI grant at most once per engine frame; false once it is revoked. */
	bool CheckLivePermission();

	// ---- frame clock ----
	// Frame-locked clock.
	// Recorded frame k after the epoch is consumed by engine frame R+1+k, where R is the engine frame
	// that established the epoch (it commits nothing). OnBeginFrame paces and fixes the recorded
	// application delta, OnSamplingInput (where capture's real input was processed) dispatches the
	// frame, OnWorldTickStart observes the owned world's tick and TickRun verifies and commits.
	FCortexReplayFrameClock Clock;
	FDelegateHandle BeginFrameHandle;
	FDelegateHandle SamplingInputHandle;
	FDelegateHandle WorldTickStartHandle;
	/** Recorded frame this engine frame consumes, or INDEX_NONE. */
	int32 ArmedFrame = INDEX_NONE;
	/** GFrameCounter of the engine frame that armed ArmedFrame. */
	uint64 ArmedFrameCounter = 0;
	/** True once the armed frame's whole range was dispatched at this engine frame's input boundary. */
	bool bArmedFramePrepared = false;
	/** True once the frame in preparation waited on UI readiness; it is then not delta-verified. */
	bool bCurrentFrameWaited = false;
	/** A failure raised at the frame or input boundary; TickRun finalizes the run with it. */
	bool bBoundaryFailed = false;
	FCortexCommandResult BoundaryFailure;
	void InstallFrameHooks();
	void RemoveFrameHooks();
	/** True while recorded frames are being consumed one per engine frame. */
	bool IsReplayingFrames() const;
	void OnReplayBeginFrame();
	void OnReplayInputBoundary();
	void OnReplayWorldTickStart(const UWorld* World, float RealDeltaSeconds);
	FCortexCommandResult PrepareRecordedFrame(int32 FrameIndex, ECortexReplayFramePreparation& OutPreparation);
};
