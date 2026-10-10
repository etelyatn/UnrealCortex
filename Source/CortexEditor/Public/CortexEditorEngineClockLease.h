#pragma once

#include "CoreMinimal.h"
#include "CortexEditorEngineFrameObserver.h"
#include "CortexTypes.h"
#include "Engine/EngineCustomTimeStep.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "CortexEditorEngineClockLease.generated.h"

class UEngine;

/** Opaque owner state; defined only in CortexEditorEngineClockLease.cpp. */
struct FCortexEditorEngineClockLeaseState;

/**
 * Native error codes for the exclusive recorded-frame engine-clock lease.
 *
 * The lease is native CortexEditor infrastructure: it never knows about Replay storage,
 * permissions or command envelopes, so it reports its own truthful codes. The timing-profile
 * refusals below carry the exact contract strings the Replay-facing errors use
 * (INCOMPATIBLE_PREREQUISITES/EDITOR_BUSY) without taking a cross-domain dependency: EDITOR_BUSY
 * is the shared CortexCore code, and the prerequisites string is declared here so CortexEditor
 * stays independent of the Replay module.
 */
namespace CortexEditorClockLeaseErrorCodes
{
	/** Acquire must be called on the Game Thread only. */
	constexpr TCHAR NotGameThread[] = TEXT("CLOCK_LEASE_NOT_GAME_THREAD");
	/** This owner already holds a live lease. */
	constexpr TCHAR AlreadyHeld[] = TEXT("CLOCK_LEASE_ALREADY_HELD");
	/** No live lease exists for the requested transition. */
	constexpr TCHAR NotHeld[] = TEXT("CLOCK_LEASE_NOT_HELD");
	/** The reconstructible normal-clock profile required by the lease is not satisfied. */
	constexpr TCHAR IncompatiblePrerequisites[] = TEXT("INCOMPATIBLE_PREREQUISITES");
	/** The target build has no fixed-time-step support, so no truthful handoff exists. */
	constexpr TCHAR UnsupportedPlatform[] = TEXT("CLOCK_LEASE_UNSUPPORTED");
	/** The engine custom time step did not initialize or did not stay attached. */
	constexpr TCHAR InitializeFailed[] = TEXT("CLOCK_LEASE_INITIALIZE_FAILED");
	/** The actual attached provider is no longer this lease (successor replaced it). */
	constexpr TCHAR AttachmentLost[] = TEXT("CLOCK_LEASE_ATTACHMENT_LOST");
	/** The requested application clock step was not a coherent recorded step. */
	constexpr TCHAR TimingMismatch[] = TEXT("CLOCK_LEASE_TIMING_MISMATCH");
	/** The requested logical time would race ahead of platform wall time. */
	constexpr TCHAR PacingViolation[] = TEXT("CLOCK_LEASE_PACING_VIOLATION");
	/** An owned timing setting was changed by a foreign owner. */
	constexpr TCHAR SettingsChanged[] = TEXT("CLOCK_LEASE_SETTINGS_CHANGED");
	/** The default clock did not resume with a bounded, positive, non-backward frame. */
	constexpr TCHAR HandoffNotObserved[] = TEXT("CLOCK_LEASE_HANDOFF_NOT_OBSERVED");
}

/**
 * Exact numeric contract for the recorded-frame clock lease.
 *
 * These constants are shared with the observers/probes that consume the lease so the
 * preparation, replay and handoff boundaries are decided in exactly one place. There are no
 * tolerance constants: every accepted application step, every restored setting and every
 * handoff observation is compared exactly against native values.
 */
namespace CortexEditorClockLease
{
	/** Application delta of the single engine-owned cleanup frame used for clock handoff. */
	constexpr double CleanupDeltaSeconds = 1.0 / 60.0;

	/**
	 * Defensible upper bound on the first default-clock frame delta after handoff: the existing
	 * 100 ms accidental-lateness allowance plus the cleanup delta. A resumed giant interval is
	 * orders of magnitude larger, while an ordinary scheduling hitch stays inside the bound.
	 */
	constexpr double DefaultFrameMaxDeltaSeconds = 0.100 + CleanupDeltaSeconds;
}

/**
 * Transient engine custom time step that applies caller-prepared application clock steps.
 *
 * Returning false from UpdateTimeStep skips the engine's own clock bookkeeping while the
 * lease owns time; the class never invents a normal, zero or default gameplay frame. It is
 * created and rooted by FCortexEditorEngineClockLease and holds only a weak link back to
 * that owner, so a Shutdown/replacement/reentrant callback can never free an in-flight
 * owner and a destroyed owner can never be reached through a dangling pointer.
 */
UCLASS(Transient, MinimalAPI)
class UCortexEditorRecordedFrameClock : public UEngineCustomTimeStep
{
	GENERATED_BODY()

public:
	//~ UEngineCustomTimeStep
	virtual bool Initialize(UEngine* InEngine) override;
	virtual void Shutdown(UEngine* InEngine) override;
	virtual bool UpdateTimeStep(UEngine* InEngine) override;
	virtual ECustomTimeStepSynchronizationState GetSynchronizationState() const override;
	virtual FString GetDisplayName() const override;

	/** Weak link to the owning lease state; assigned before the clock is ever attached. */
	TWeakPtr<FCortexEditorEngineClockLeaseState> LeaseState;
};

/**
 * Ordinary strong owner of the exclusive recorded-frame engine clock.
 *
 * The owner is created before the owned PIE request, keeps the transient clock rooted with
 * TStrongObjectPtr, and defers every real release through BeginHandoff so a destructor (or a
 * lost editor shutdown) is only a last-resort safety cleanup that never claims success.
 *
 * All observer handles live in per-acquisition state, so they are generation-scoped by
 * construction: an in-flight callback from one acquisition can never remove or mutate a
 * successor acquisition's observers, and the clock only ever holds a weak link back to its
 * own state.
 *
 * Acquire refuses a foreign PIE session, an existing custom time step, benchmarking/fixed
 * application timing, a fixed engine framerate and an explicit delta override; the lease
 * exists to reproduce recorded application/world timing, not to alter the human's settings.
 *
 * Every applied step is the caller's prepared FCortexEditorAppClockStep. The owner verifies it
 * exactly (finite values, positive Delta, strictly positive Current-against-Last span, Last
 * bit-identical to the engine's prior application CurrentTime, Current never beyond platform
 * wall time), applies FApp::UpdateLastTime() plus the requested Current/Delta, then reads all
 * three values back and requires the exact requested values. No tolerance, clamp or
 * normalization is applied anywhere. On success it returns false so the engine advances
 * GameTime without running its own clock.
 *
 * Handoff (called only after the matching owned PIE ended; it refuses while any PIE world is
 * still active or queued) takes exactly the fixed-step settings needed for ONE cleanup frame,
 * detaches only this actual provider and returns true so THAT SAME engine update runs the
 * engine's normal fixed branch. That frame end refreshes the default cached real time and
 * restores only unchanged owned settings; the following default frame must be finite, positive,
 * bounded and non-backward before the state becomes Released. A successor provider or foreign
 * timing edit latches Lost, releases only owned resources and never reinstalls or restores over
 * foreign state. A cleared provider can arm the public one-frame cache reanchor only when no
 * PIE context remains. Clearing it while an owned world is terminal but still awaiting deferred
 * destruction leaves this path unbounded: the sustained native probe observed a 1.308792800 s
 * first default delta. Mandatory native feasibility is blocked pending design review; no
 * completed provider-loss handoff guarantee is claimed.
 *
 * Failure ownership: UpdateTimeStep runs strictly before world ticking. A rejected step or
 * failed caller read latches a typed failure, changes no application time and returns false;
 * the engine's skip path still advances application GameTime by the previous DeltaTime.
 * The lease cannot authorize or terminate a selected world from its operation generation.
 * The caller must terminate its exact owned session at the failure boundary: EndOwnedPIE
 * disables ticking on that verified terminal world before requesting normal deferred PIE
 * destruction. The native producer-failure regression proves no further selected-world tick
 * on this path. Returning an error or queueing an engine-wide end request alone is insufficient.
 * Provider replacement/clear and invalid-step callers require their own termination evidence;
 * this tested producer-failure path is not a blanket guarantee for every loss boundary.
 *
 * A live PIE world also blocks the one-frame cleanup: the cleanup frame is consumed by whichever
 * world is live, and this engine-global lease cannot prove matching-world ownership from its
 * operation generation alone, so it is armed only once no PIE world remains.
 */
class CORTEXEDITOR_API FCortexEditorEngineClockLease
{
public:
	FCortexEditorEngineClockLease();
	~FCortexEditorEngineClockLease();

	FCortexEditorEngineClockLease(const FCortexEditorEngineClockLease&) = delete;
	FCortexEditorEngineClockLease& operator=(const FCortexEditorEngineClockLease&) = delete;

	/**
	 * Attaches the recorded-frame clock to Engine for OperationGeneration.
	 *
	 * ReadNextStep is invoked once per owned engine frame (Game Thread, inside the engine's
	 * update-time callback) and may perform the caller's deadline/app-current pacing before it
	 * returns. It must fill an exact step: finite values, positive Delta, a strictly positive
	 * Current-minus-Last span, Last bit-identical to the prior application CurrentTime and
	 * Current no later than platform wall time. It is stored once and never copied per frame.
	 * Before any selected world exists it simply returns the first recorded frame's preparation
	 * step; the lease has no world dependency and never advances a caller frame cursor itself.
	 * Returns success only after the engine's actual provider readback confirms this exact clock
	 * is attached and initialized.
	 */
	FCortexCommandResult Acquire(UEngine& Engine, uint64 OperationGeneration,
		TFunction<FCortexCommandResult(FCortexEditorAppClockStep&)>&& ReadNextStep);

	/** Transitions Preparing -> Replaying once the replay epoch is established. Idempotent. */
	void SetReplaying();

	/**
	 * Defers the complete native handoff: called only after the matching owned PIE has ended.
	 *
	 * Refuses (EDITOR_BUSY, no side effects) while any PIE world is still active or a play
	 * request is still queued. Duplicate calls are idempotent and never re-run a cleanup frame.
	 */
	FCortexCommandResult BeginHandoff();

	/** True only once the observed default-clock frame completed after handoff. */
	bool IsHandoffComplete() const;

	/** Truthful observed state; never inferred success. */
	ECortexEditorClockState GetState() const;

	/** The latched failure, if any, without allocating or mutating state. */
	FCortexCommandResult GetLastFailure() const;

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test-support override that reproduces the engine's real hazard where Initialize
	 * reports failure while SetCustomTimeStep has already left the clock attached.
	 *
	 * Compiled only into automation-enabled builds: production always initializes normally.
	 * The next Acquire consumes the flag once and must release the partial owned attachment
	 * without disturbing any pre-existing provider.
	 */
	void SetForceInitializeFailureForTests(bool bForce) { bForceInitializeFailureForTests = bForce; }
#endif

private:
	TSharedPtr<FCortexEditorEngineClockLeaseState> State;

#if WITH_DEV_AUTOMATION_TESTS
	bool bForceInitializeFailureForTests = false;
#endif
};
