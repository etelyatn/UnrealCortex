#include "CortexReplayGuardEvaluator.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayErrorCodes.h"

#include "Dom/JsonObject.h"

namespace
{
/** The fixed normalized local-coordinate tolerance for a supported UI identity. */
constexpr double GUARD_UI_LOCAL_TOLERANCE = 0.005;

/**
 * Normalized coordinates are doubles, so a value that is exactly at the fixed 0.005 bound can
 * land a few ULPs above it once the difference is computed (e.g. 0.255 - 0.25 =
 * 0.0050000000000000044). This epsilon absorbs only that representation noise: the bound is
 * still a strict `> 0.005` comparison, and any genuine deviation above 0.005 fails.
 */
constexpr double GUARD_UI_LOCAL_REPRESENTATION_EPSILON = 1.0e-9;

const TCHAR* GuardObservationStateToString(ECortexEditorUIObservationState State)
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

FCortexCommandResult GuardUIError(const TCHAR* Reason,
	const FCortexEditorPhysicalInputWidgetIdentity* Expected,
	const FCortexEditorPhysicalInputUIObservation& UI, double LocalError)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("reason"), FString(Reason));
	Details->SetStringField(TEXT("observation"), FString(GuardObservationStateToString(UI.State)));
	Details->SetNumberField(TEXT("tolerance"), GUARD_UI_LOCAL_TOLERANCE);
	Details->SetNumberField(TEXT("local_error"), LocalError);
	Details->SetNumberField(TEXT("actual_x"), UI.LocalPosition.X);
	Details->SetNumberField(TEXT("actual_y"), UI.LocalPosition.Y);
	if (Expected != nullptr)
	{
		Details->SetStringField(TEXT("expected_identity_sha256"), Expected->IdentitySha256);
	}
	return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayUIGuardFailed),
		FString::Printf(TEXT("Replay UI guard failed (%s)"), GuardObservationStateToString(UI.State)),
		Details);
}

/** True only for a structurally usable expected selector carrying a trustworthy digest. */
bool GuardIsValidSupportedSelector(const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	if (!FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(Identity.IdentitySha256))
	{
		return false;
	}
	switch (Identity.Discriminator)
	{
	case ECortexEditorUIRootDiscriminator::SingletonClass:
		return !Identity.RootClassPath.IsEmpty();
	case ECortexEditorUIRootDiscriminator::RootTag:
		return !Identity.RootTag.IsEmpty() && !Identity.TargetTag.IsEmpty();
	case ECortexEditorUIRootDiscriminator::SavedComponent:
		return !Identity.ActorPath.IsEmpty() && !Identity.ComponentPath.IsEmpty();
	default:
		return false;
	}
}

/**
 * Full structural identity comparison with the digest as a first-class disambiguator: BOTH sides
 * must carry a trustworthy digest and the digests must match, and the declared fields must always
 * match so a digest collision cannot alias two different selectors. A missing or malformed digest
 * on either side is never a match.
 */
bool GuardIdentitiesEqual(const FCortexEditorPhysicalInputWidgetIdentity& A,
	const FCortexEditorPhysicalInputWidgetIdentity& B)
{
	if (!FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(A.IdentitySha256)
		|| !FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(B.IdentitySha256)
		|| !A.IdentitySha256.Equals(B.IdentitySha256, ESearchCase::CaseSensitive))
	{
		return false;
	}
	return A.RootKind == B.RootKind
		&& A.Surface == B.Surface
		&& A.Discriminator == B.Discriminator
		&& A.RootClassPath == B.RootClassPath
		&& A.RootTag == B.RootTag
		&& A.ActorPath == B.ActorPath
		&& A.ComponentPath == B.ComponentPath
		&& A.TargetTag == B.TargetTag
		&& A.WidgetAncestry == B.WidgetAncestry;
}
}

FCortexCommandResult FCortexReplayGuardEvaluator::CheckPose(
	const FCortexEditorPhysicalInputPlayerPose& Expected,
	const FCortexEditorPhysicalInputPlayerPose& Actual)
{
	const FCortexCommandResult Compared =
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Actual);
	if (Compared.bSuccess)
	{
		return Compared;
	}

	// Only the shared pose-mismatch code is remapped; every other shared failure (non-finite,
	// degenerate) keeps its own code and details.
	if (Compared.ErrorCode == FString(CortexEditorPhysicalInputErrorCodes::PoseMismatch))
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayPoseGuardFailed),
			Compared.ErrorMessage, Compared.ErrorDetails);
	}
	return Compared;
}

FCortexReplayGuardDecision FCortexReplayGuardEvaluator::Evaluate(const FCortexReplayEvent& Event,
	const FCortexEditorPhysicalInputPlayerPose& ActualPose,
	const FCortexEditorPhysicalInputUIObservation& UI, bool bCanWait)
{
	FCortexReplayGuardDecision Decision;
	if (!Event.Guard.IsSet())
	{
		return Decision; // Ready: ordinary releases and motion are never gated.
	}

	const FCortexReplayInteractionGuard& Guard = Event.Guard.GetValue();

	// The pose is compared first and requires a finite observed pose.
	const FCortexCommandResult PoseResult = CheckPose(Guard.ExpectedPose, ActualPose);
	if (!PoseResult.bSuccess)
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = PoseResult;
		return Decision;
	}

	// NotApplicable and Unavailable skip only the UI identity check; the pose guard already ran
	// and the fixed partial warning stays at metadata/run-result level.
	if (Guard.UICoverage != ECortexEditorUICoverage::Supported)
	{
		return Decision; // Ready
	}

	if (!Guard.UITarget.IsValid() || !GuardIsValidSupportedSelector(*Guard.UITarget))
	{
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = GuardUIError(TEXT("missing_expected_selector"), Guard.UITarget.Get(), UI, 0.0);
		return Decision;
	}

	switch (UI.State)
	{
	case ECortexEditorUIObservationState::Ready:
	{
		if (!UI.ActualTarget.IsValid() || !GuardIdentitiesEqual(*Guard.UITarget, *UI.ActualTarget))
		{
			Decision.State = ECortexReplayGuardDecisionState::Error;
			Decision.Error = GuardUIError(TEXT("identity_mismatch"), Guard.UITarget.Get(), UI, 0.0);
			return Decision;
		}

		const double DeltaX = FMath::Abs(UI.LocalPosition.X - Guard.ExpectedLocalPosition.X);
		const double DeltaY = FMath::Abs(UI.LocalPosition.Y - Guard.ExpectedLocalPosition.Y);
		const double LocalError = FMath::Max(DeltaX, DeltaY);
		if (LocalError > GUARD_UI_LOCAL_TOLERANCE + GUARD_UI_LOCAL_REPRESENTATION_EPSILON)
		{
			Decision.State = ECortexReplayGuardDecisionState::Error;
			Decision.Error = GuardUIError(TEXT("local_position_mismatch"), Guard.UITarget.Get(), UI, LocalError);
			return Decision;
		}
		return Decision; // Ready
	}

	case ECortexEditorUIObservationState::WrongTarget:
	case ECortexEditorUIObservationState::Ambiguous:
	case ECortexEditorUIObservationState::Unavailable:
		// Unequivocal mismatches never wait and never click a different target.
		Decision.State = ECortexReplayGuardDecisionState::Error;
		Decision.Error = GuardUIError(TEXT("unequivocal_mismatch"), Guard.UITarget.Get(), UI, 0.0);
		return Decision;

	default:
		// Pending/unready states: wait only when waiting is currently permitted.
		if (bCanWait)
		{
			Decision.State = ECortexReplayGuardDecisionState::Wait;
			Decision.WaitReason = UI.State;
		}
		else
		{
			Decision.State = ECortexReplayGuardDecisionState::Error;
			Decision.Error = GuardUIError(TEXT("wait_not_permitted"), Guard.UITarget.Get(), UI, 0.0);
		}
		return Decision;
	}
}
