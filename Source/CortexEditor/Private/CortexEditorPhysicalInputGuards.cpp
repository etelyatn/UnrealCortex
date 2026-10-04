#include "CortexEditorPhysicalInputGuards.h"

#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/WidgetPath.h"
#include "Misc/SecureHash.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

FCortexEditorPhysicalInputWidgetIdentity FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(
	const SWidget& RootWidget, const SWidget& TargetWidget)
{
	FCortexEditorPhysicalInputWidgetIdentity Identity;
	Identity.RootKind = ECortexEditorUIRootKind::Slate;
	Identity.Surface = ECortexEditorUISurface::Viewport;
	Identity.Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Identity.RootTag = RootWidget.GetTag().ToString();
	Identity.TargetTag = TargetWidget.GetTag().ToString();

	const FString Canonical = CanonicalizeSelector(Identity);
	FSHA1 Hash;
	Hash.UpdateWithString(*Canonical, Canonical.Len());
	Hash.Final();
	uint8 Digest[20];
	Hash.GetHash(Digest);
	Identity.IdentitySha256 = BytesToHex(Digest, 20);
	return Identity;
}

FString FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(
	const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	// A stable, plain-text selector: never transient widget names, addresses or runtime counters.
	return FString::Printf(TEXT("slate|viewport|roottag=%s|targettag=%s"),
		*Identity.RootTag, *Identity.TargetTag);
}

bool FCortexEditorPhysicalInputUIResolver::ResolveActualSlateTarget(
	const FVector2D& ViewportPosition, const FCortexEditorPhysicalInputGuardState& State,
	FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal)
{
	const TSharedPtr<SWidget> Viewport = State.ViewportWidget.Pin();
	if (!Viewport.IsValid())
	{
		return false;
	}

	const FVector2D ScreenSpace = Viewport->GetCachedGeometry().LocalToAbsolute(ViewportPosition);
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
	if (!Window.IsValid())
	{
		return false;
	}

	// LocateWidgetInWindow is protected in UE5.8; LocateWindowUnderMouse is its public counterpart and
	// returns the same deep path. Restricting the window list to the selected viewport's own window
	// keeps this a selected-route hit test with the selected Slate user rather than a global one.
	TArray<TSharedRef<SWindow>> SelectedWindows;
	SelectedWindows.Add(Window.ToSharedRef());
	const FWidgetPath Path = Slate.LocateWindowUnderMouse(
		ScreenSpace, SelectedWindows, /*bIgnoreEnabledStatus*/ false, State.SlateUserIndex);
	if (!Path.IsValid())
	{
		return false;
	}

	// The tagged-runtime route: the deepest tagged widget on the real hit path is the control and
	// the next tagged ancestor is its root. Nothing else is claimed as a supported identity.
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = Path.Widgets.Num() - 1; Index >= 0; --Index)
	{
		if (Path.Widgets[Index].GetWidgetPtr()->GetTag() != NAME_None)
		{
			TargetIndex = Index;
			break;
		}
	}
	if (TargetIndex == INDEX_NONE)
	{
		return false;
	}

	int32 RootIndex = INDEX_NONE;
	for (int32 Index = TargetIndex - 1; Index >= 0; --Index)
	{
		if (Path.Widgets[Index].GetWidgetPtr()->GetTag() != NAME_None)
		{
			RootIndex = Index;
			break;
		}
	}
	if (RootIndex == INDEX_NONE)
	{
		return false;
	}

	const FArrangedWidget& TargetArranged = Path.Widgets[TargetIndex];
	const FArrangedWidget& RootArranged = Path.Widgets[RootIndex];
	OutIdentity = FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(
		*RootArranged.GetWidgetPtr(), *TargetArranged.GetWidgetPtr());

	const FGeometry& TargetGeometry = TargetArranged.Geometry;
	const FVector2D Local = TargetGeometry.AbsoluteToLocal(ScreenSpace);
	const FVector2D Size = TargetGeometry.GetLocalSize();
	OutNormalizedLocal = FVector2D(
		Size.X > 0.0 ? Local.X / Size.X : 0.0,
		Size.Y > 0.0 ? Local.Y / Size.Y : 0.0);
	return true;
}

ECortexEditorUIObservationState FCortexEditorPhysicalInputUIResolver::ResolveSlateObservation(
	const FCortexEditorPhysicalInputEvent& Press, const FCortexEditorPhysicalInputWidgetIdentity& Expected,
	const FCortexEditorPhysicalInputGuardState& State, FCortexEditorPhysicalInputUIObservation& Out)
{
	Out = FCortexEditorPhysicalInputUIObservation();

	if (Expected.RootKind != ECortexEditorUIRootKind::Slate
		|| Expected.Discriminator != ECortexEditorUIRootDiscriminator::RootTag
		|| Expected.RootTag.IsEmpty()
		|| Expected.TargetTag.IsEmpty())
	{
		Out.State = ECortexEditorUIObservationState::Unavailable;
		return Out.State;
	}

	FCortexEditorPhysicalInputWidgetIdentity Actual;
	FVector2D Normalized = FVector2D::ZeroVector;
	if (!ResolveActualSlateTarget(Press.ViewportPosition, State, Actual, Normalized))
	{
		// Unknown freshness is pending, never a wrong-target verdict.
		Out.State = ECortexEditorUIObservationState::PointerPending;
		return Out.State;
	}

	Out.LocalPosition = Normalized;
	Out.MotionGeneration = State.MotionGeneration;
	Out.LayoutGeneration = State.LayoutGeneration;
	Out.ActualTarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(MoveTemp(Actual));
	if (Out.ActualTarget->RootTag != Expected.RootTag
		|| Out.ActualTarget->TargetTag != Expected.TargetTag)
	{
		Out.State = ECortexEditorUIObservationState::WrongTarget;
		return Out.State;
	}

	Out.State = ECortexEditorUIObservationState::Ready;
	return Out.State;
}
