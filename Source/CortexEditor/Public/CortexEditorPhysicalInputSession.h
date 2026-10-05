#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexTypes.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class APawn;
class APlayerController;
class FCortexEditorPhysicalInputCaptureProcessor;
class FDragDropOperation;
class FSlateApplication;
class SWidget;
class SViewport;
class UClass;
class UPlayerInput;
class UWorld;
struct FCortexEditorPhysicalInputCaptureState;
struct FCortexEditorPhysicalInputDispatchContext;
struct FCortexEditorPhysicalInputGuardState;
struct FPointerEvent;

/**
 * Shared CortexEditor physical input error codes.
 *
 * These are consumed by the physical input session and by Replay without taking a
 * dependency on the Replay module. Task 3 extends this namespace with the UI
 * observation and cleanup codes.
 */
namespace CortexEditorPhysicalInputErrorCodes
{
	/** Production preparation deadline (30s) elapsed before the owned target was ready. */
	constexpr TCHAR PreparationTimeout[] = TEXT("INPUT_PREPARATION_TIMEOUT");

	/** A captured pose and the observed pose differ beyond the fixed tolerances. */
	constexpr TCHAR PoseMismatch[] = TEXT("INPUT_POSE_MISMATCH");

	/** The original player pose could not be restored on the captured target. */
	constexpr TCHAR InitialPoseRestoreFailed[] = TEXT("INITIAL_POSE_RESTORE_FAILED");

	/**
	 * A required UI identity/observation could not be produced on the exact selected route
	 * (malformed expected selector, unobservable route or a fresh wrong target).
	 */
	constexpr TCHAR UIObservationFailed[] = TEXT("INPUT_UI_OBSERVATION_FAILED");

	/**
	 * Owned synthetic input or the original target could not be safely neutralized during
	 * release; the terminal cleanup state is not claimed as completed.
	 */
	constexpr TCHAR CleanupFailed[] = TEXT("INPUT_CLEANUP_FAILED");
}

/**
 * The exact physical input target a session is bound to.
 *
 * Every handle is weak: the session never keeps a world, controller, pawn or Slate
 * widget alive on its own. Only the exact captured target is usable; a destroyed or
 * replaced pawn invalidates the binding instead of being adopted.
 */
struct FCortexEditorPhysicalInputTargetBinding
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<APawn> Pawn;
	TWeakPtr<SWidget> InputRoot;
	TWeakPtr<SViewport> ViewportWidget;
	int32 SlateUserIndex = INDEX_NONE;
	FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
};

/**
 * Owns the exact editor physical input target used for recording and playback.
 *
 * The session either creates and owns a PIE session for a map (BeginOwnedPIE) or binds
 * to an already running world (BindTarget). Readiness is validated against the exact
 * selected local player and the world's own viewport, never against a first global
 * world, user or device. Owned PIE preparation carries a production 30-second deadline
 * that fails through the readiness callback.
 *
 * Task 2 declared the target/pose surface; Task 3 adds capture, UI observation and normal
 * dispatch. Capture/dispatch behavior lives in the private helper units
 * (CortexEditorPhysicalInputCapture/Dispatch/Guards); this header stays free of UMG headers.
 */
class CORTEXEDITOR_API FCortexEditorPhysicalInputSession
{
public:
	FCortexEditorPhysicalInputSession();
	~FCortexEditorPhysicalInputSession();

	/**
	 * Requests an owned in-process PIE session for MapAssetPath and completes OnReady
	 * exactly once on the Game Thread. Success requires the exact selected controller to
	 * possess a valid pawn with a stable class and an initialized world/viewport.
	 * Invalid prerequisites fail explicitly; the 30-second production preparation
	 * deadline fails once with INPUT_PREPARATION_TIMEOUT. Never ends pre-existing PIE.
	 */
	FCortexCommandResult BeginOwnedPIE(const FString& MapAssetPath,
		int32 LocalPlayerIndex,
		TFunction<void(const FCortexCommandResult&)>&& OnReady);

	/**
	 * Synchronously binds this session to an already running world's selected local
	 * player. Performs the same target readiness validation without creating or owning
	 * PIE. Fails immediately when the target is missing, ambiguous or unready.
	 */
	FCortexCommandResult BindTarget(UWorld& World, int32 LocalPlayerIndex);

	/** Validates that the current binding still refers to the exact ready target. */
	bool ValidateTarget(FCortexCommandResult& OutError) const;

	/** Prerequisites persisted for a run (map, pawn class, local player, viewport, DPI). */
	const FCortexEditorPhysicalInputTargetInfo& GetTargetInfo() const;

	/** The weak binding handles; never persisted. */
	const FCortexEditorPhysicalInputTargetBinding& GetTargetBinding() const;

	/** Reads the bound pawn's transform and the bound controller's control rotation. */
	FCortexCommandResult ReadPlayerPose(FCortexEditorPhysicalInputPlayerPose& Out) const;

	/**
	 * Allocation-free success-path comparator: position <= 0.5cm, shortest quaternion
	 * angular distance <= 0.5 degrees for pawn and control rotation, maximum absolute
	 * scale-component difference <= 0.001. Rejects non-finite or degenerate poses.
	 */
	static FCortexCommandResult ComparePlayerPose(
		const FCortexEditorPhysicalInputPlayerPose& Expected,
		const FCortexEditorPhysicalInputPlayerPose& Actual);

	/**
	 * Restores the initial player pose on the exact bound target. The recorded pawn class
	 * is verified through a guarded load/IsChildOf before any mutation; a mismatch or a
	 * failed readback is INITIAL_POSE_RESTORE_FAILED with no nearby fallback.
	 */
	FCortexCommandResult RestorePlayerPose(const FCortexEditorPhysicalInputPlayerPose& Pose,
		const FString& RecordedPawnClassPath);

	/**
	 * Replays one captured portable event through normal engine routing on the exact bound
	 * target. Keys use the normal FKeyEvent path, pointer button/down/up/double-click/wheel use
	 * the normal Slate processors and pointer motion uses the normal mouse-move path inside a
	 * reentry guard so Slate drag detection and preprocessors are preserved. Generated events
	 * are never re-captured by this session and never count as human interference. The engine's
	 * handled boolean is not dispatch success; validated admission and the actual invocation
	 * are.
	 */
	FCortexCommandResult Dispatch(const FCortexEditorPhysicalInputEvent& Event);

	/**
	 * Arms replay epoch interference ownership at epoch establishment, before the first event.
	 *
	 * While armed, any loss of the selected focus/window/route is always treated as interference,
	 * including during the idle/wait window before the first dispatch. Real physical input is
	 * treated as interference only when bUnattended is true: an unattended (AI-origin) replay
	 * interrupts on any real key/button/move edge, while an attended (human-origin) replay lets
	 * the human's own input through without interrupting. Does not steal focus and does not enable
	 * inactive-application input handling; a lost route is reported as interference instead.
	 * Requires the exact binding to still be valid; cleared by ReleaseHeldInputs/Shutdown.
	 */
	FCortexCommandResult BeginReplayEpoch(bool bUnattended);

	/** True only while a replay epoch is armed (attended or unattended). */
	bool IsReplayEpochArmed() const;

	/** True only while an armed replay epoch is unattended (AI-origin) interference ownership. */
	bool IsReplayUnattended() const;

	/**
	 * Non-blocking observation of the exact selected route for one guarded press. Returns the
	 * structured ready/pending/mismatch evidence in Out and never invokes a widget callback or
	 * issues a movement command. Pending freshness precedes any wrong-target verdict.
	 */
	FCortexCommandResult ObserveUI(const FCortexEditorPhysicalInputEvent& Press,
		const FCortexEditorPhysicalInputWidgetIdentity& Expected,
		FCortexEditorPhysicalInputUIObservation& Out) const;

	/**
	 * True only while no key/button owned by this session is held and no capture or drag is
	 * active. A UI wait is never permitted from a non-neutral owned state.
	 */
	bool CanWaitForUI() const;

	/**
	 * Arms human capture on the selected target. Requires a neutral physical keyboard/mouse
	 * state; a pre-held key, button or modifier (including a UI-consumed press) fails with
	 * INVALID_OPERATION plus a release-inputs instruction and leaves the original human state
	 * untouched. The callback receives the portable event, its monotonic capture time and the
	 * pre-press context.
	 */
	FCortexCommandResult SetCaptureCallback(TFunction<void(const FCortexEditorPhysicalInputEvent&,
		double, const FCortexEditorPhysicalInputCaptureContext&)>&& Callback);

	/**
	 * Registers the one interruption callback. It is invoked when real physical input is observed
	 * during an unattended replay, or when the selected route loses ownership during any replay
	 * (attended or unattended); reentrant callbacks only record state and never re-enter dispatch.
	 * Passing an empty callback clears it.
	 */
	void SetInterruptionCallback(TFunction<void(const FCortexCommandResult&)>&& Callback);

	/**
	 * Releases this operation's owned synthetic and captured state on the original target by
	 * the shared cleanup algorithm. Success requires observed neutralization of this
	 * operation's state, never a handled boolean. Live owned state that cannot be safely
	 * neutralized returns INPUT_CLEANUP_FAILED with bounded original-target diagnostics.
	 */
	FCortexCommandResult ReleaseHeldInputs();

	/**
	 * Requests termination of the matching owned PIE session asynchronously. A void
	 * return is not completion; IsOwnedPIEEnded() is the only completion signal.
	 */
	void EndOwnedPIE();

	/**
	 * True only once the matching pending request is confirmed cancelled and the matching
	 * owned PIE world context is absent. Already-ended or non-owned sessions report
	 * complete without ending borrowed or foreign PIE.
	 */
	bool IsOwnedPIEEnded() const;

	/** Releases this session's target, cancels pending work and ends only owned PIE. */
	void Shutdown();

	/**
	 * Test-support override for the Windows physical-key snapshot (see IsPhysicalInputNeutral).
	 *
	 * Compiled only into automation-enabled builds: production must never be able to replace the
	 * physical snapshot and bypass the neutral-admission guarantee. When no resolver is set, the
	 * real GetAsyncKeyState high-bit snapshot over the complete supported domain is used.
	 */
#if WITH_DEV_AUTOMATION_TESTS
	static void SetPhysicalKeySnapshotResolver(TFunction<bool(const FKey&)>&& Resolver);
	static void ClearPhysicalKeySnapshotResolver();
	/**
	 * Test-support read-only query: supported key identities the snapshot cannot resolve. Empty
	 * means the domain is complete; a non-empty result makes admission fail explicitly naming them.
	 */
	static TArray<FString> GetUnresolvedSupportedKeyNames();
#endif

private:
	/** The non-consuming processor reports every observed engine input back to this session. */
	friend class FCortexEditorPhysicalInputCaptureProcessor;

	/** Owned PIE lifecycle; borrowed bindings leave this at None. */
	enum class EOwnedState : uint8
	{
		None,
		Preparing,
		Ready,
		Ending
	};

	/** Per-request identity captured when the owned request is accepted. */
	struct FOwnedRequest
	{
		FString MapAssetPath;
		FString RequestedPackagePath;
		int32 LocalPlayerIndex = 0;
		double DeadlineSeconds = 0.0;
		TArray<FName> BaselinePIEContextHandles;
	};

	// ---- owned PIE state ----
	EOwnedState OwnedState = EOwnedState::None;
	bool bOwnsPIE = false;
	bool bOwnedRequestOutstanding = false;
	bool bStartupResolved = false;
	uint64 Generation = 0;
	/** Identity of the request the engine queued, excluding DestinationSlateViewport (nulled at start). */
	uint64 SubmittedRequestFingerprint = 0;
	/** Identity of the request while still queued, including DestinationSlateViewport. */
	uint64 SubmittedQueuedRequestFingerprint = 0;
	FOwnedRequest OwnedRequest;
	TFunction<void(const FCortexCommandResult&)> ReadyCallback;
	bool bReadyCallbackInvoked = false;
	/** Handle returned by FTSTicker::AddTicker; exact type so RemoveTicker accepts it. */
	FTSTicker::FDelegateHandle TickerHandle;
	FName OwnedContextHandle;
	TWeakObjectPtr<UWorld> OwnedWorld;
	/**
	 * True once this operation actually observed its owned context carrying a live world.
	 *
	 * Before that (deferred startup) a world-less owned context must still count as present, so a
	 * request that has not produced its world yet is never reported as ended. Afterwards, that
	 * world's destruction ends the operation even if the engine leaves the emptied context in its
	 * world-context list.
	 */
	bool bOwnedWorldObserved = false;
	/**
	 * True once the exact captured owned context has been observed present at least once.
	 *
	 * The first absence afterwards is a terminal loss of ownership: a later context (a successor
	 * PIE, which may carry an identical request fingerprint) can never be adopted or ended, and no
	 * further end-PIE request is issued for this operation.
	 */
	mutable bool bOwnedContextObserved = false;
	/** Latched once the exact captured owned context has been observed absent; never cleared mid-run. */
	mutable bool bOwnedContextGone = false;
	/**
	 * Latched once this operation's owned teardown is fully resolved (IsOwnedPIEEnded()).
	 *
	 * From that point the session performs NO further engine/teardown calls, issues no further
	 * end-PIE/CancelRequestPlaySession request (a double end-PIE while the engine is already
	 * tearing the level viewport down asserts), and stops its teardown ticker.
	 */
	bool bOwnedTerminationResolved = false;
	/**
	 * True once this operation has issued its single RequestEndPlayMap for the owned context.
	 *
	 * Exactly one actor issues end-PIE per owned session; the engine queues the request and ends
	 * it once, so re-issuing every tick is neither needed nor safe.
	 */
	bool bOwnedEndPlayRequested = false;
	/** Throttle for the "teardown still pending" diagnostic; 0 means it has not fired yet. */
	double LastTeardownDiagnosticSeconds = 0.0;
	TWeakObjectPtr<UClass> LastObservedPawnClass;
	int32 StablePawnClassObservations = 0;

	// ---- binding state ----
	bool bBound = false;
	FCortexEditorPhysicalInputTargetBinding Binding;
	FCortexEditorPhysicalInputTargetInfo TargetInfo;
	TWeakObjectPtr<UClass> BoundPawnClass;

	// ---- capture/dispatch/guard state (Task 3) ----
	/**
	 * The armed capture callback and its single interruption callback. Both are cleared by
	 * ReleaseHeldInputs/Shutdown; neither owns the session.
	 */
	TFunction<void(const FCortexEditorPhysicalInputEvent&, double,
		const FCortexEditorPhysicalInputCaptureContext&)> CaptureCallbackImpl;
	TFunction<void(const FCortexCommandResult&)> InterruptionCallbackImpl;
	/** Installed before any consuming preprocessor for the selected user/device. */
	TSharedPtr<FCortexEditorPhysicalInputCaptureProcessor> CaptureProcessor;
	/** Owned capture/dispatch/guard bookkeeping; opaque here so the public header stays UMG-free. */
	TSharedPtr<FCortexEditorPhysicalInputCaptureState> CaptureState;
	TSharedPtr<FCortexEditorPhysicalInputDispatchContext> DispatchContext;
	TSharedPtr<FCortexEditorPhysicalInputGuardState> GuardState;
	/** Frozen once cleanup starts so no further generated event can be dispatched. */
	bool bDispatchFrozen = false;
	/** True after this session arms a replay epoch and before cleanup: replay is in progress. */
	bool bReplayInProgress = false;
	/**
	 * True while that replay epoch is unattended (AI-origin): real physical input is interference.
	 * Cleared when the epoch ends. Meaningful only while bReplayInProgress is true.
	 */
	bool bReplayUnattended = false;
	/** True once per epoch so the attended-suppression Display log fires at most once. */
	bool bAttendedSuppressionLogged = false;
	/** Exact PlayerInput instance the binding was acquired with; never a replacement. */
	TWeakObjectPtr<UPlayerInput> BoundPlayerInput;

	/** Creates the capture/dispatch/guard state and installs the non-consuming processor. */
	void AttachCaptureToBinding();

	/** Removes the processor and clears every capture/dispatch bookkeeping field. */
	void DetachCapture();

	/** Validates neutrality and arms the capture epoch with the given callback. */
	FCortexCommandResult ArmCapture(TFunction<void(const FCortexEditorPhysicalInputEvent&,
		double, const FCortexEditorPhysicalInputCaptureContext&)>&& Callback);

	/**
	 * True only when no physical key/button/modifier is observed down on the selected target.
	 *
	 * Uses the target-attributable signals (the session observation sets, which include
	 * processor-observed keys and mouse buttons for the selected user/device, and the selected
	 * controller's key state) plus the Windows physical-key high-bit snapshot over the complete
	 * cached supported key->virtual-key mapping, which catches a key held before the processor was
	 * installed (including UI-consumed presses the engine never routed). The editor-global Slate
	 * cached button set is deliberately NOT consulted: it retains synthetic bits from this or an
	 * earlier operation's own dispatch and is not attributable to the capture target.
	 */
	bool IsPhysicalInputNeutral(FCortexCommandResult& OutError) const;

	/** Processor callbacks; each filters by the selected user/device before observing. */
	void ObserveProcessorKey(const FKeyEvent& KeyEvent, bool bKeyDown);
	void ObserveProcessorMouseButton(const FPointerEvent& MouseEvent, ECortexEditorPhysicalInputKind Kind);
	void ObserveProcessorMouseMove(const FPointerEvent& MouseEvent);
	void ObserveProcessorMouseWheel(const FPointerEvent& MouseEvent);

	/** Records one already-attributed human event and invokes the capture callback. */
	void HandleCapturedEvent(const FCortexEditorPhysicalInputEvent& Event);

	/**
	 * Samples the pre-press context (frame/time/pause/pose/UI) for one recorded event. Returns
	 * false, with OutError describing the required observation that failed, when a guarded press
	 * cannot carry a genuinely observed pre-press pose; the caller signals a capture fault.
	 */
	bool BuildCaptureContext(const FCortexEditorPhysicalInputEvent& Event,
		FCortexEditorPhysicalInputCaptureContext& OutContext, FCortexCommandResult& OutError) const;

	/** Resolves the tagged-runtime Slate identity at one viewport-local coordinate. */
	bool ResolveTaggedSlateIdentity(const FVector2D& ViewportPosition,
		FCortexEditorPhysicalInputWidgetIdentity& OutIdentity,
		FVector2D& OutNormalizedLocal) const;

	/** Converts a screen-space coordinate into the selected viewport's local space. */
	FVector2D ToViewportPosition(const FVector2D& ScreenSpacePosition) const;

	/** Converts a viewport-local coordinate back into screen space. */
	FVector2D ToScreenSpacePosition(const FVector2D& ViewportPosition) const;

	/** The widget whose geometry maps screen space to the selected viewport-local space. */
	TSharedPtr<SWidget> GetCoordinateRootWidget() const;

	/** True when the engine event belongs to the selected Slate user and input device. */
	bool IsSelectedUserAndDevice(uint32 UserIndex, const FInputDeviceId& Device) const;

	/** Reports a foreign-input interruption through the single interruption callback. */
	void NotifyInterruption(const FCortexCommandResult& Result);

	/**
	 * Logs once per epoch (Display) that real physical input was observed while the replay is
	 * attended, so the interruption was suppressed instead of ending the run.
	 */
	void LogAttendedInputSuppressed(const FKey& Key);

	/** Signals an incomplete capture epoch (for example inconsistent modifier bits). */
	void SignalCaptureFault(const FString& Message);

	/** True when the pointer at this screen coordinate is over the selected viewport route. */
	bool IsPointerPositionOnSelectedRoute(const FVector2D& ScreenSpacePosition) const;

	/** True when the selected user's keyboard focus is inside the selected viewport route. */
	bool IsKeyboardFocusOnSelectedRoute() const;

	/**
	 * True when the selected user's current keyboard focus/window still belongs to the selected
	 * route. A foreign focus or a different active window means replay no longer owns input.
	 */
	bool IsSelectedRouteOwnershipIntact(FString& OutReason) const;

	/** True when the given widget (or one of its ancestors) is on the selected viewport route. */
	bool IsWidgetOnSelectedRoute(const TSharedPtr<const SWidget>& Widget) const;

	/**
	 * True when the selected user's live pointer capture belongs to the selected viewport route,
	 * so ongoing movement is routed to a selected consumer even outside its hit area.
	 */
	bool IsPointerCaptureOnSelectedRoute() const;

	/**
	 * Retains the exact captor/drag-drop/high-precision identities this operation's own dispatch
	 * established and reconciles a capture/high-precision the normal dispatch released. Called
	 * after each pointer event so cleanup only ever cancels live, genuinely owned state.
	 */
	void RetainOwnedPointerState(FSlateApplication& Slate, const FPointerEvent& PointerEvent,
		bool bHadUserCaptureBefore, bool bHadHighPrecisionBefore, const void* NativeCaptureBefore,
		const TSharedPtr<FDragDropOperation>& DragDropBefore);

	void EnsureTicker();
	void RemoveTicker();
	bool TickInternal(float DeltaTime);

	/** Polls the owned preparation once per Game-Thread tick. */
	void PollPreparation();

	/** Fails the pending preparation once and starts matching teardown. */
	void CompletePreparationFailure(const FString& ErrorCode, const FString& Message);

	/** Completes the pending preparation once with the resolved target. */
	void CompletePreparationSuccess();

	/** Starts owned termination: releases the usable binding, invalidates readiness, requests end. */
	void BeginOwnedTermination();

	/** Drops the usable binding/target info while retaining the owned-world teardown identity. */
	void ReleaseReadyBinding();

	/** Polls owned startup while ending so a created context can never be missed. */
	void PollTeardown();

	/** Relinquishes ownership of the queued request without cancelling it. */
	void RelinquishOwnedRequest();

	/** True when an unrelated PIE session, queued request or queued teardown already exists. */
	bool HasUnrelatedPIESession() const;

	/** Validates the effective play settings for a single owned in-process PIE session. */
	static bool ArePlaySettingsSupported(FString& OutReason);

	/** Resolves the PIE world of the captured owned context handle. */
	UWorld* ResolveOwnedContextWorld() const;

	/** Cancels/ends only the matching pending request or owned PIE context. */
	void RequestOwnedTermination();

	/** True while the queued play-session request is still the one this session accepted. */
	bool IsOwnedRequestPending() const;

	/** True while the exact captured owned PIE world context is present. */
	bool IsOwnedContextPresent() const;

	/** True while the exact captured owned PIE context is present and already has a world. */
	bool IsOwnedContextWorldPresent() const;

	/** Finds the single new PIE world created for this request, or nullptr. */
	UWorld* FindNewPIEWorld(FName& OutContextHandle, bool& bOutAmbiguous) const;

	/**
	 * Resolves the exact selected local player/controller/pawn/viewport/user/device for a
	 * running PIE world. Nothing is attached when this fails.
	 */
	bool ResolveTarget(UWorld& World, int32 LocalPlayerIndex, bool bRequireStablePawnClass,
		FCortexEditorPhysicalInputTargetBinding& OutBinding,
		FCortexEditorPhysicalInputTargetInfo& OutInfo,
		FCortexCommandResult& OutError);

	/** Rejects non-finite or degenerate poses. */
	static bool IsPoseUsable(const FCortexEditorPhysicalInputPlayerPose& Pose);

	/** Guarded recorded-class resolution (loaded objects first, no class-name strings). */
	static UClass* ResolveRecordedPawnClass(const FString& RecordedPawnClassPath);
};
