#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"

class SWidget;

/**
 * Data-only foundation for the CortexEditor physical input session.
 *
 * These value types describe physical keyboard/mouse input, the player pose it was
 * captured against, stable runtime-UI selectors and the target prerequisites needed
 * to reconstruct a session. They carry no engine handles and no session behavior;
 * the session class and its TargetBinding live in CortexEditorPhysicalInputSession.h.
 */
enum class ECortexEditorPhysicalInputKind : uint8
{
	KeyDown, KeyUp, PointerMove, RelativeMove,
	PointerDown, PointerUp, DoubleClick, Wheel
};

struct FCortexEditorPhysicalInputEvent
{
	ECortexEditorPhysicalInputKind Kind;
	FKey Key;
	FModifierKeysState Modifiers;
	bool bRepeat = false;
	FVector2D ViewportPosition = FVector2D::ZeroVector;
	FVector2D Delta = FVector2D::ZeroVector;
	float WheelDelta = 0.0f;
};

struct FCortexEditorPhysicalInputPlayerPose
{
	FTransform PawnTransform = FTransform::Identity;
	FRotator ControlRotation = FRotator::ZeroRotator;
};

enum class ECortexEditorUIRootKind : uint8 { UMG, Slate };

enum class ECortexEditorUISurface : uint8 { Viewport, WorldComponent };

enum class ECortexEditorUIRootDiscriminator : uint8 { SingletonClass, RootTag, SavedComponent };

struct FCortexEditorPhysicalInputWidgetIdentity
{
	ECortexEditorUIRootKind RootKind = ECortexEditorUIRootKind::UMG;
	ECortexEditorUISurface Surface = ECortexEditorUISurface::Viewport;
	ECortexEditorUIRootDiscriminator Discriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
	FString RootClassPath, RootTag, ActorPath, ComponentPath, TargetTag;
	TArray<FName> WidgetAncestry;
	FString IdentitySha256;
};

enum class ECortexEditorUICoverage : uint8 { NotApplicable, Supported, Unavailable };

enum class ECortexEditorUIUnavailableReason : uint8
{
	None, MissingAuthoredDiscriminator, DynamicInstance,
	UnobservablePointerRoute, AnonymousSlate
};

enum class ECortexEditorUIObservationState : uint8
{
	Ready, PointerPending, LayoutPending, TargetMissing,
	TargetDisabled, TargetNotHitTestable, WrongTarget, Ambiguous, Unavailable
};

struct FCortexEditorPhysicalInputUIObservation
{
	ECortexEditorUIObservationState State = ECortexEditorUIObservationState::PointerPending;
	FVector2D LocalPosition = FVector2D::ZeroVector;
	uint64 MotionGeneration = 0;
	uint64 LayoutGeneration = 0;
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> ActualTarget;
};

struct FCortexEditorPhysicalInputCaptureContext
{
	uint64 FrameNumber = 0;
	double WorldTimeSeconds = 0.0;
	bool bWorldPaused = false;
	bool bTargetOwnsPointerCapture = false;
	TOptional<FCortexEditorPhysicalInputPlayerPose> PressPose;
	ECortexEditorUICoverage UICoverage = ECortexEditorUICoverage::NotApplicable;
	ECortexEditorUIUnavailableReason UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> UITarget;
	FVector2D UILocalPosition = FVector2D::ZeroVector;
};

struct FCortexEditorPhysicalInputTargetInfo
{
	FString MapAssetPath;
	FString PawnClassPath;
	int32 LocalPlayerIndex = 0;
	FIntPoint ViewportSize = FIntPoint::ZeroValue;
	double DpiScale = 1.0;
};

/**
 * Builds and hashes the portable widget selector.
 *
 * There is exactly ONE canonical selector representation and ONE lower-case 64-hex SHA-256
 * digest implementation, shared by live capture (this type), persisted loading and live
 * resolution. The digest is always SHA-256 of the UTF-8 bytes of CanonicalizeSelector; a
 * separate SHA-1 or a differently canonicalized digest must never be produced for an identity.
 */
class CORTEXEDITOR_API FCortexEditorPhysicalInputSelectorBuilder
{
public:
	/**
	 * RootTag/TargetTag come from SWidget::GetTag for distinct root and control widgets. An
	 * empty root/target tag is not a supported selector and must surface as unavailable, never
	 * as an anonymous fallback.
	 */
	static FCortexEditorPhysicalInputWidgetIdentity BuildSlateIdentity(
		const SWidget& RootWidget, const SWidget& TargetWidget);

	/**
	 * Canonical, digest-free selector text for the identity. The exact bytes returned here are
	 * the only input to ComputeIdentitySha256, in capture, load and live resolution alike.
	 */
	static FString CanonicalizeSelector(const FCortexEditorPhysicalInputWidgetIdentity& Identity);

	/**
	 * Lower-case 64-hex SHA-256 of the UTF-8 canonical selector. Returns an empty string when the
	 * platform hashing provider is unavailable; callers must treat empty as a failure, never as
	 * a matchable digest.
	 */
	static FString ComputeIdentitySha256(const FCortexEditorPhysicalInputWidgetIdentity& Identity);
};
