#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SWidget;
class UWorld;

/** Shared native byte identity: lower-case SHA-256, empty when the provider is unavailable. */
FString CortexEditorPhysicalInputSha256(const uint8* Data, int64 Size);

/**
 * Weak selector resolution and freshness state for the uniquely tagged runtime Slate route.
 *
 * The UMG/CommonUI/world-component branches are intentionally absent from this unit: it
 * resolves only tagged Slate roots/controls, retains weak live objects and invalidates on root
 * rebuild/target destruction. It never reads the OS desktop cursor or a stale cached hover.
 */
struct FCortexEditorPhysicalInputGuardState
{
	/** Normal-route pointer motion this session processed; the freshness barrier for hits. */
	uint64 MotionGeneration = 0;
	/** Layout generation of the resolved tagged root; bumps on root rebuild/resize. */
	uint64 LayoutGeneration = 0;
	TWeakPtr<SWidget> TaggedRoot;
	TWeakPtr<SWidget> TaggedTarget;
	TWeakObjectPtr<UWorld> TaggedWorld;
	/** Selected viewport widget used to convert viewport-local coordinates back to screen space. */
	TWeakPtr<SWidget> ViewportWidget;
	int32 SlateUserIndex = INDEX_NONE;
	/** Last viewport-local pointer position observed through normal routing. */
	FVector2D LastViewportPointerPosition = FVector2D::ZeroVector;
	/**
	 * Screen-space pointer position the engine consumes: the exact (fractional) position of the
	 * last observed or replayed pointer, always inside the coordinate-root viewport for replayed
	 * relative motion. Relative motion advances RelativeMotionAccumulator, not this value.
	 */
	FVector2D LastScreenPointerPosition = FVector2D::ZeroVector;
	/**
	 * True once LastScreenPointerPosition reflects a real location. Until then the first relative
	 * move seeds the accumulator from LastViewportPointerPosition, exactly as the previous
	 * viewport-derived accumulation did.
	 */
	bool bHasScreenPointerPosition = false;
	/**
	 * Unclamped exact sum of the replayed relative deltas. The whole-unit delta each event presents
	 * is round(accumulator + delta) - round(accumulator), so the recorded total transfers exactly
	 * even where the presented position is held at the viewport edge. Reseeded from
	 * LastScreenPointerPosition by every observed or absolute position.
	 */
	FVector2D RelativeMotionAccumulator = FVector2D::ZeroVector;
	bool bHasRelativeMotionAccumulator = false;
};

/**
 * The portable selector builder (BuildSlateIdentity/CanonicalizeSelector/ComputeIdentitySha256)
 * lives in the public CortexEditorPhysicalInput.h so capture and load share exactly one
 * canonical selector + SHA-256 implementation.
 */

/** Resolves normal-route evidence for the exact selected tagged Slate route. */
class FCortexEditorPhysicalInputUIResolver
{
public:
	/**
	 * Resolves the actual tagged target at one viewport-local coordinate on the normal hit path,
	 * restricted to the selected runtime scope (State.ViewportWidget). Root uniqueness is counted
	 * within that scope and target uniqueness within the resolved root's own subtree.
	 *
	 * Returns false when no trustworthy tagged root/control can be observed. On false,
	 * bOutIdentityFault is true only for a systemic identity fault (a resolved selector whose
	 * digest is missing or malformed); an ordinary unresolved/pending hit leaves it false.
	 */
	static bool ResolveActualSlateTarget(const FVector2D& ViewportPosition,
		const FCortexEditorPhysicalInputGuardState& State,
		FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal,
		bool& bOutIdentityFault);

	/**
	 * Non-blocking: returns Ready with the actual normalized local coordinates only when the
	 * current normal-route hit is the expected tagged control. PointerPending/LayoutPending
	 * precede any WrongTarget verdict, and no widget callback is ever invoked.
	 */
	static ECortexEditorUIObservationState ResolveSlateObservation(
		const FCortexEditorPhysicalInputEvent& Press,
		const FCortexEditorPhysicalInputWidgetIdentity& Expected,
		const FCortexEditorPhysicalInputGuardState& State,
		FCortexEditorPhysicalInputUIObservation& Out);
};
