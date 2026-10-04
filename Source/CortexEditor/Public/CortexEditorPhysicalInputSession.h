#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexTypes.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class APawn;
class APlayerController;
class SWidget;
class SViewport;
class UClass;
class UWorld;

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
 * Task 2 declares and implements only the target/pose surface below; UI observation,
 * capture and dispatch belong to Task 3 and must not be declared here yet.
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

private:
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
	TWeakObjectPtr<UClass> LastObservedPawnClass;
	int32 StablePawnClassObservations = 0;

	// ---- binding state ----
	bool bBound = false;
	FCortexEditorPhysicalInputTargetBinding Binding;
	FCortexEditorPhysicalInputTargetInfo TargetInfo;
	TWeakObjectPtr<UClass> BoundPawnClass;

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
