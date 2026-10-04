#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"

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
