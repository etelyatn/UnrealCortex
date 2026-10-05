#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SWidget;
class UWorld;

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
	 * Resolves the actual tagged target at one viewport-local coordinate on the normal hit path.
	 * Returns false when no tagged root/control can be observed (unknown freshness).
	 */
	static bool ResolveActualSlateTarget(const FVector2D& ViewportPosition,
		const FCortexEditorPhysicalInputGuardState& State,
		FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal);

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
