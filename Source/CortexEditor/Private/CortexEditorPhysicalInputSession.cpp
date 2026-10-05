#include "CortexEditorPhysicalInputSession.h"

#include "Application/SlateApplicationBase.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputCapture.h"
#include "CortexEditorPhysicalInputDispatch.h"
#include "CortexEditorPhysicalInputGuards.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/PlatformInput.h"
#include "IAssetViewport.h"
#include "Input/DragAndDrop.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "KeyState.h"
#include "LevelEditor.h"
#include "Layout/WidgetPath.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Slate/SGameLayerManager.h"
#include "Slate/SceneViewport.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

#include "CortexEditorModule.h"

#if PLATFORM_WINDOWS
// Declares the real GetAsyncKeyState used for the physical-key snapshot in capture admission.
// UE's wrapper keeps windows.h out of the module's other translation units and off the PCH.
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
/** The production preparation budget is fixed and is not caller-configurable. */
constexpr double CortexOwnedPIEPreparationBudgetSeconds = 30.0;

/** The declared pawn class must be observed on at least this many consecutive polls. */
constexpr int32 CortexStablePawnClassObservationsRequired = 2;

FCortexCommandResult MakeSuccessResult()
{
	FCortexCommandResult Result;
	Result.bSuccess = true;
	return Result;
}

FCortexCommandResult MakeErrorResult(const FString& ErrorCode, const FString& Message)
{
	return FCortexCommandRouter::Error(ErrorCode, Message);
}

FCortexCommandResult MakeInvalidTargetResult(const FString& Message)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, Message);
}

/**
 * Engine-visible identity of a play request. The engine overwrites its queued request
 * wholesale, so a matching fingerprint is the only way to know the queued request is
 * still the exact one this session accepted.
 *
 * DestinationSlateViewport is included only for the queued comparison. The engine keeps
 * it verbatim while the request is still merely queued, so a same-map successor that
 * differs only in its destination viewport is not mistaken for ours. Once the session
 * starts the engine nulls that field on the request it retains
 * (PlayLevel.cpp:3099-3105, reached through the by-reference
 * `PlayInEditorSessionInfo->OriginalRequestParams` at PlayLevel.cpp:1622), so the
 * consumed-session comparison must exclude it.
 */
uint64 ComputeRequestFingerprint(const FRequestPlaySessionParams& Request, bool bIncludeDestinationSlateViewport)
{
	const FString StartLocation = Request.StartLocation.IsSet() ? Request.StartLocation->ToString() : FString();
	const FString StartRotation = Request.StartRotation.IsSet() ? Request.StartRotation->ToString() : FString();
	const FString GameModeOverride = Request.GameModeOverride ? Request.GameModeOverride->GetPathName() : FString();
	const FString ExtraParameters = Request.AdditionalStandaloneCommandLineParameters.Get(TEXT(""));
	const FString DestinationViewport = bIncludeDestinationSlateViewport
		? FString::Printf(TEXT("%d:%p"),
			Request.DestinationSlateViewport.IsSet() ? 1 : 0,
			static_cast<const void*>(Request.DestinationSlateViewport.IsSet()
				? Request.DestinationSlateViewport.GetValue().Pin().Get() : nullptr))
		: FString(TEXT("excluded"));

	const FString Canonical = FString::Printf(
		TEXT("%s|%d|%d|%p|%d|%s|%d|%s|%d|%s|%s|%d|%s"),
		*Request.GlobalMapOverride,
		static_cast<int32>(Request.SessionDestination),
		static_cast<int32>(Request.WorldType),
		static_cast<const void*>(Request.CustomPIEWindow.Pin().Get()),
		Request.StartLocation.IsSet() ? 1 : 0,
		*StartLocation,
		Request.StartRotation.IsSet() ? 1 : 0,
		*StartRotation,
		Request.bAllowOnlineSubsystem ? 1 : 0,
		*ExtraParameters,
		*GameModeOverride,
		Request.SessionPreviewTypeOverride.IsSet() ? static_cast<int32>(Request.SessionPreviewTypeOverride.GetValue()) : -1,
		*DestinationViewport);

	return static_cast<uint64>(GetTypeHash(Canonical));
}
}

FCortexEditorPhysicalInputSession::FCortexEditorPhysicalInputSession()
{
	EnsureTicker();
}

FCortexEditorPhysicalInputSession::~FCortexEditorPhysicalInputSession()
{
	// Never leave this operation's owned synthetic state cached in Slate when the owner is destroyed
	// without Shutdown(), and never leave the capture processor registered.
	if (bBound && FSlateApplication::IsInitialized())
	{
		ReleaseHeldInputs();
	}
	DetachCapture();

	// Never leave an owned PIE session orphaned if the owner is destroyed without Shutdown().
	if (bOwnsPIE && GEditor != nullptr && !IsOwnedPIEEnded())
	{
		RequestOwnedTermination();
	}
	RemoveTicker();
	++Generation;
	bReadyCallbackInvoked = true;
	ReadyCallback = nullptr;
}

// ---------------------------------------------------------------------------
// Ticker / owned-PIE observation
// ---------------------------------------------------------------------------

void FCortexEditorPhysicalInputSession::EnsureTicker()
{
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FCortexEditorPhysicalInputSession::TickInternal),
			0.0f);
	}
}

void FCortexEditorPhysicalInputSession::RemoveTicker()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

bool FCortexEditorPhysicalInputSession::TickInternal(float DeltaTime)
{
	(void)DeltaTime;

	// While replay owns the target, a loss of the selected focus/window/route between events (a
	// wait or idle transition) is interference too, not only a loss observed at the next dispatch.
	if (bReplayInProgress && !bDispatchFrozen
		&& CaptureState.IsValid() && !CaptureState->bInterrupted)
	{
		FString OwnershipReason;
		if (!IsSelectedRouteOwnershipIntact(OwnershipReason))
		{
			NotifyInterruption(MakeErrorResult(CortexErrorCodes::InvalidOperation, OwnershipReason));
		}
	}

	if (OwnedState == EOwnedState::Preparing)
	{
		PollPreparation();
	}
	else if (OwnedState == EOwnedState::Ending)
	{
		// Latch resolution before any teardown action: once IsOwnedPIEEnded() has been observed
		// the session must perform no further engine/teardown work and must stop its ticker.
		if (!bOwnedTerminationResolved && IsOwnedPIEEnded())
		{
			bOwnedTerminationResolved = true;
		}
		if (!bOwnedTerminationResolved)
		{
			PollTeardown();
			if (IsOwnedPIEEnded())
			{
				bOwnedTerminationResolved = true;
			}
		}
		if (bOwnedTerminationResolved)
		{
			OwnedState = EOwnedState::None;
			TickerHandle.Reset();
			return false; // Teardown observed; stop the minimal observer.
		}
	}
	return true;
}

UWorld* FCortexEditorPhysicalInputSession::ResolveOwnedContextWorld() const
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		if (OwnedContextHandle != NAME_None && Context.ContextHandle == OwnedContextHandle)
		{
			return Context.World();
		}
	}
	return nullptr;
}

UWorld* FCortexEditorPhysicalInputSession::FindNewPIEWorld(FName& OutContextHandle, bool& bOutAmbiguous) const
{
	OutContextHandle = NAME_None;
	bOutAmbiguous = false;
	if (GEngine == nullptr)
	{
		return nullptr;
	}

	UWorld* Candidate = nullptr;
	FName CandidateHandle = NAME_None;
	bool bFoundCandidate = false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		if (OwnedRequest.BaselinePIEContextHandles.Contains(Context.ContextHandle))
		{
			continue;
		}
		UWorld* ContextWorld = Context.World();
		// Correlate with the accepted request by map when the world already exists. A PIE
		// context can also exist before its world during deferred login
		// (PlayLevel.cpp:1827-1875), so a world-less context is still a candidate.
		if (ContextWorld != nullptr
			&& !OwnedRequest.RequestedPackagePath.IsEmpty()
			&& UWorld::RemovePIEPrefix(ContextWorld->GetPackage()->GetName()) != OwnedRequest.RequestedPackagePath)
		{
			continue;
		}
		if (bFoundCandidate)
		{
			bOutAmbiguous = true;
			return nullptr;
		}
		bFoundCandidate = true;
		Candidate = ContextWorld;
		CandidateHandle = Context.ContextHandle;
	}
	OutContextHandle = CandidateHandle;
	return Candidate;
}

void FCortexEditorPhysicalInputSession::PollPreparation()
{
	if (GEditor == nullptr)
	{
		CompletePreparationFailure(CortexErrorCodes::EditorNotReady, TEXT("Editor is no longer available"));
		return;
	}

	// Captured so an invalidated generation cannot bind a successor or complete late.
	const uint64 PreparationGeneration = Generation;

	// Refresh the monotonic time before evaluating readiness; game time may be paused.
	if (FPlatformTime::Seconds() >= OwnedRequest.DeadlineSeconds)
	{
		CompletePreparationFailure(CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
			TEXT("Owned PIE preparation exceeded the 30 second production deadline"));
		return;
	}

	UWorld* PIEWorld = nullptr;
	if (!bStartupResolved)
	{
		if (bOwnedRequestOutstanding)
		{
			if (IsOwnedRequestPending())
			{
				return; // Still queued; wait for the engine to start it.
			}
			// No longer queued: it must be the exact session we accepted, otherwise someone
			// replaced our request and we must relinquish it without cancelling theirs.
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			if (!Info.IsSet() || ComputeRequestFingerprint(Info->OriginalRequestParams, false) != SubmittedRequestFingerprint)
			{
				RelinquishOwnedRequest();
				CompletePreparationFailure(CortexErrorCodes::InvalidOperation,
					TEXT("The queued owned play request was replaced before it started"));
				return;
			}
			bOwnedRequestOutstanding = false;
		}

		// The engine consumed the request; the started session must be exactly ours.
		const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
		if (!SessionInfo.IsSet())
		{
			CompletePreparationFailure(CortexErrorCodes::EditorBusy,
				TEXT("The queued owned play request did not start a PIE session"));
			return;
		}
		if (ComputeRequestFingerprint(SessionInfo->OriginalRequestParams, false) != SubmittedRequestFingerprint)
		{
			RelinquishOwnedRequest();
			CompletePreparationFailure(CortexErrorCodes::InvalidOperation,
				TEXT("The owned PIE session was replaced before its target was ready"));
			return;
		}

		FName ContextHandle = NAME_None;
		bool bAmbiguous = false;
		PIEWorld = FindNewPIEWorld(ContextHandle, bAmbiguous);
		if (bAmbiguous)
		{
			CompletePreparationFailure(CortexErrorCodes::InvalidOperation,
				TEXT("Owned PIE created more than one candidate world context"));
			return;
		}
		if (PIEWorld == nullptr)
		{
			return; // The world has not been created yet; keep polling.
		}

		// Pin the exact owned context immediately so teardown can always end it.
		OwnedWorld = PIEWorld;
		OwnedContextHandle = ContextHandle;
		bOwnedWorldObserved = true;
		bOwnedContextObserved = true;
		bOwnedRequestOutstanding = false;
		bStartupResolved = true;
	}
	else
	{
		// Once pinned, never adopt a successor: poll only the captured context.
		PIEWorld = ResolveOwnedContextWorld();
		if (PIEWorld == nullptr)
		{
			CompletePreparationFailure(CortexErrorCodes::InvalidOperation,
				TEXT("The owned PIE context disappeared before its target was ready"));
			return;
		}
	}

	FCortexEditorPhysicalInputTargetBinding ResolvedBinding;
	FCortexEditorPhysicalInputTargetInfo ResolvedInfo;
	FCortexCommandResult ResolutionError;
	if (!ResolveTarget(*PIEWorld, OwnedRequest.LocalPlayerIndex, true, ResolvedBinding, ResolvedInfo, ResolutionError))
	{
		// Keep polling until the exact target is ready or the deadline expires.
		return;
	}

	// Compare a fresh monotonic time again after the readiness work before accepting.
	if (PreparationGeneration != Generation)
	{
		return; // The generation was invalidated during readiness; never bind it.
	}
	if (FPlatformTime::Seconds() >= OwnedRequest.DeadlineSeconds)
	{
		CompletePreparationFailure(CortexEditorPhysicalInputErrorCodes::PreparationTimeout,
			TEXT("Owned PIE preparation exceeded the 30 second production deadline"));
		return;
	}

	Binding = ResolvedBinding;
	TargetInfo = ResolvedInfo;
	BoundPawnClass = ResolvedBinding.Pawn.Get()->GetClass();
	bBound = true;
	bOwnsPIE = true;
	OwnedState = EOwnedState::Ready;
	AttachCaptureToBinding();
	CompletePreparationSuccess();
}

void FCortexEditorPhysicalInputSession::PollTeardown()
{
	// Once this operation's teardown is resolved, no further engine/teardown call is ever made.
	if (bOwnedTerminationResolved)
	{
		return;
	}
	if (!bOwnsPIE)
	{
		// Borrowed/foreign worlds are never observed or ended.
		bStartupResolved = true;
		return;
	}
	if (GEditor == nullptr)
	{
		bStartupResolved = true;
		return;
	}

	if (bOwnedContextGone)
	{
		// The exact captured context was observed gone: never adopt or end a successor PIE.
		bStartupResolved = true;
		return;
	}

	if (!bStartupResolved)
	{
		if (bOwnedRequestOutstanding)
		{
			if (IsOwnedRequestPending())
			{
				// Still queued: nothing of ours was ever created; cancel the exact request.
				GEditor->CancelRequestPlaySession();
				RelinquishOwnedRequest();
				bStartupResolved = true;
				return;
			}
			// No longer queued: it is only ours if the started session is the exact one accepted.
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			if (Info.IsSet() && ComputeRequestFingerprint(Info->OriginalRequestParams, false) == SubmittedRequestFingerprint)
			{
				bOwnedRequestOutstanding = false;
			}
			else
			{
				// Replaced/foreign: never cancel it, and nothing of ours exists to observe.
				RelinquishOwnedRequest();
				bStartupResolved = true;
				return;
			}
		}

		// The engine can hold our PIE context before its world exists during deferred startup
		// (PlayLevel.cpp:1827-1875), so capture the exact context now, world or not, and keep
		// observing it instead of claiming completion for a context that can still appear.
		if (OwnedContextHandle == NAME_None)
		{
			const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
			if (!SessionInfo.IsSet()
				|| ComputeRequestFingerprint(SessionInfo->OriginalRequestParams, false) != SubmittedRequestFingerprint)
			{
				// A different session owns the editor; nothing of ours will be created.
				RelinquishOwnedRequest();
				bStartupResolved = true;
				return;
			}
			FName ContextHandle = NAME_None;
			bool bAmbiguous = false;
			UWorld* Created = FindNewPIEWorld(ContextHandle, bAmbiguous);
			if (ContextHandle != NAME_None)
			{
				// Created may still be null until the deferred world appears; keep observing it.
				OwnedWorld = Created;
				OwnedContextHandle = ContextHandle;
				bOwnedContextObserved = true;
				if (Created != nullptr)
				{
					bOwnedWorldObserved = true;
				}
			}
		}

		if (OwnedContextHandle != NAME_None)
		{
			// The exact owned context is captured; observe it until it is gone.
			bStartupResolved = true;
		}
		else if (FPlatformTime::Seconds() >= OwnedRequest.DeadlineSeconds)
		{
			// A consumed request that never produced any context is positively abandoned.
			RelinquishOwnedRequest();
			bStartupResolved = true;
			return;
		}
	}

	// End only once the captured owned context has a world to tear down, and only once per
	// operation: the engine queues the request and ends the session exactly once, so re-issuing
	// it every tick is neither needed nor safe.
	if (IsOwnedContextWorldPresent() && !bOwnedEndPlayRequested)
	{
		GEditor->RequestEndPlayMap();
		bOwnedEndPlayRequested = true;
	}

	// Bounded diagnostic: an owned teardown should resolve within a frame or two, so a pending
	// teardown lasting a full second is itself a defect worth reporting with its observations.
	// It only prints while the teardown is genuinely unresolved (this function returns early once
	// bOwnedTerminationResolved is set).
	if (FPlatformTime::Seconds() - LastTeardownDiagnosticSeconds >= 1.0)
	{
		LastTeardownDiagnosticSeconds = FPlatformTime::Seconds();
		UWorld* OwnedWorldPtr = OwnedWorld.Get();
		UE_LOG(LogCortexEditor, Log,
			TEXT("Owned PIE teardown still pending: startupResolved=%d requestPending=%d contextPresent=%d worldPresent=%d worldObserved=%d worldTearingDown=%d"),
			bStartupResolved ? 1 : 0, IsOwnedRequestPending() ? 1 : 0,
			IsOwnedContextPresent() ? 1 : 0, IsOwnedContextWorldPresent() ? 1 : 0,
			bOwnedWorldObserved ? 1 : 0,
			(OwnedWorldPtr != nullptr && OwnedWorldPtr->bIsTearingDown) ? 1 : 0);
	}
}

void FCortexEditorPhysicalInputSession::RelinquishOwnedRequest()
{
	bOwnedRequestOutstanding = false;
	SubmittedRequestFingerprint = 0;
	SubmittedQueuedRequestFingerprint = 0;
}

void FCortexEditorPhysicalInputSession::CompletePreparationFailure(const FString& ErrorCode, const FString& Message)
{
	if (bReadyCallbackInvoked)
	{
		return;
	}
	bReadyCallbackInvoked = true;

	// Invalidate the generation before teardown or callbacks so late readiness cannot bind.
	++Generation;
	OwnedState = EOwnedState::Ending;
	RequestOwnedTermination();

	if (ErrorCode == CortexErrorCodes::EditorNotReady)
	{
		// A missing editor is the only genuinely unexpected preparation failure.
		UE_LOG(LogCortexEditor, Warning, TEXT("Owned physical input preparation failed: %s (%s)"), *ErrorCode, *Message);
	}
	else
	{
		// Every other preparation outcome is an explicit, caller-visible rejection.
		UE_LOG(LogCortexEditor, Log, TEXT("Owned physical input preparation failed: %s (%s)"), *ErrorCode, *Message);
	}

	TFunction<void(const FCortexCommandResult&)> Callback = MoveTemp(ReadyCallback);
	ReadyCallback = nullptr;
	if (Callback)
	{
		Callback(MakeErrorResult(ErrorCode, Message));
	}
}

void FCortexEditorPhysicalInputSession::CompletePreparationSuccess()
{
	if (bReadyCallbackInvoked)
	{
		return;
	}
	bReadyCallbackInvoked = true;
	OwnedState = EOwnedState::Ready;

	TFunction<void(const FCortexCommandResult&)> Callback = MoveTemp(ReadyCallback);
	ReadyCallback = nullptr;
	if (Callback)
	{
		Callback(MakeSuccessResult());
	}
}

void FCortexEditorPhysicalInputSession::RequestOwnedTermination()
{
	if (GEditor == nullptr || bOwnedTerminationResolved)
	{
		return;
	}
	// Cancel only the queued request this session accepted while it is still outstanding.
	if (bOwnedRequestOutstanding)
	{
		if (IsOwnedRequestPending())
		{
			// Our request is still queued: cancel it and drop ownership.
			GEditor->CancelRequestPlaySession();
			RelinquishOwnedRequest();
		}
		else
		{
			// Not queued anymore. Keep the fingerprint so teardown can correlate and end
			// the context the engine created for us; relinquish only for foreign requests.
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			if (Info.IsSet() && ComputeRequestFingerprint(Info->OriginalRequestParams, false) == SubmittedRequestFingerprint)
			{
				bOwnedRequestOutstanding = false;
			}
			else
			{
				RelinquishOwnedRequest();
			}
		}
	}
	// End only when the exact captured owned PIE context has a world to tear down, and only once
	// per operation (the engine queues the request and ends the session exactly once). A world-less
	// deferred context is left to PollTeardown, which issues the single request once the world
	// exists; a world-less end request would be dropped by the engine and never retried.
	if (IsOwnedContextWorldPresent() && !bOwnedEndPlayRequested)
	{
		GEditor->RequestEndPlayMap();
		bOwnedEndPlayRequested = true;
	}
}

bool FCortexEditorPhysicalInputSession::IsOwnedRequestPending() const
{
	if (!bOwnedRequestOutstanding || GEditor == nullptr || SubmittedQueuedRequestFingerprint == 0)
	{
		return false;
	}
	// Compare against the engine's queued copy, which still carries DestinationSlateViewport,
	// so a same-map successor that differs only in its destination viewport is not ours.
	const TOptional<FRequestPlaySessionParams> Pending = GEditor->GetPlaySessionRequest();
	return Pending.IsSet()
		&& ComputeRequestFingerprint(Pending.GetValue(), true) == SubmittedQueuedRequestFingerprint;
}

bool FCortexEditorPhysicalInputSession::IsOwnedContextPresent() const
{
	if (bOwnedContextGone || GEngine == nullptr)
	{
		// Once the exact captured context has been observed absent, no later context (including a
		// successor PIE with an identical request) is ever ours again.
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		if (OwnedContextHandle != NAME_None && Context.ContextHandle == OwnedContextHandle)
		{
			if (!bOwnedWorldObserved)
			{
				// Deferred startup: a world-less owned context is still ours and must stay observed
				// until its world appears (PlayLevel deferred startup).
				bOwnedContextObserved = true;
				return true;
			}
			UWorld* ContextWorld = Context.World();
			// Defensive: a context under our handle whose world is a different object is a
			// successor, not ours.
			if (ContextWorld != nullptr && OwnedWorld.IsValid() && ContextWorld != OwnedWorld.Get())
			{
				continue;
			}
			// Once this operation actually drove a world, its context is ours only while that world
			// is still a live, non-tearing-down PIE world. A finished PIE world can stay alive (and
			// renamed by CleanupWorld) until GC, and must not keep the operation open.
			if (ContextWorld != nullptr && !ContextWorld->bIsTearingDown)
			{
				bOwnedContextObserved = true;
				return true;
			}
			continue;
		}
		if (OwnedWorld.IsValid() && Context.World() == OwnedWorld.Get())
		{
			bOwnedContextObserved = true;
			return true;
		}
	}

	// The exact captured context is gone. Latch the loss so a successor can never be adopted or
	// ended and this operation issues no further end-PIE request.
	if (bOwnedContextObserved)
	{
		bOwnedContextGone = true;
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::IsOwnedContextWorldPresent() const
{
	if (bOwnedContextGone || GEngine == nullptr)
	{
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		if (OwnedContextHandle != NAME_None && Context.ContextHandle == OwnedContextHandle)
		{
			UWorld* ContextWorld = Context.World();
			// A successor context under our handle must never be treated as endable by this session.
			if (ContextWorld != nullptr && bOwnedWorldObserved && OwnedWorld.IsValid()
				&& ContextWorld != OwnedWorld.Get())
			{
				continue;
			}
			return ContextWorld != nullptr && !ContextWorld->bIsTearingDown;
		}
		if (OwnedWorld.IsValid() && Context.World() == OwnedWorld.Get())
		{
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Admission helpers
// ---------------------------------------------------------------------------

bool FCortexEditorPhysicalInputSession::HasUnrelatedPIESession() const
{
	if (GEditor == nullptr)
	{
		return true;
	}
	// Anything the engine would consume or tear down before our own request must be
	// rejected, otherwise a fresh session could destroy an unrelated human PIE run.
	if (GEditor->PlayWorld != nullptr)
	{
		return true;
	}
	if (GEditor->GetPlaySessionRequest().IsSet())
	{
		return true;
	}
	if (GEditor->ShouldEndPlayMap())
	{
		return true;
	}
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE)
			{
				return true;
			}
		}
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::ArePlaySettingsSupported(FString& OutReason)
{
	const ULevelEditorPlaySettings* PlaySettings = GetDefault<ULevelEditorPlaySettings>();
	if (PlaySettings == nullptr)
	{
		OutReason = TEXT("Play in editor settings are not available");
		return false;
	}

	bool bRunUnderOneProcess = false;
	PlaySettings->GetRunUnderOneProcess(bRunUnderOneProcess);
	if (!bRunUnderOneProcess)
	{
		OutReason = TEXT("Owned physical input PIE requires RunUnderOneProcess");
		return false;
	}
	if (PlaySettings->bLaunchSeparateServer)
	{
		OutReason = TEXT("Owned physical input PIE does not support a separate server");
		return false;
	}

	int32 ClientCount = 1;
	PlaySettings->GetPlayNumberOfClients(ClientCount);
	if (ClientCount != 1)
	{
		OutReason = TEXT("Owned physical input PIE supports exactly one local client");
		return false;
	}

	EPlayNetMode NetMode = PIE_Standalone;
	PlaySettings->GetPlayNetMode(NetMode);
	if (NetMode != PIE_Standalone)
	{
		OutReason = TEXT("Owned physical input PIE supports the standalone play net mode only");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Owned termination
// ---------------------------------------------------------------------------

void FCortexEditorPhysicalInputSession::ReleaseReadyBinding()
{
	DetachCapture();
	++Generation;
	bBound = false;
	Binding = FCortexEditorPhysicalInputTargetBinding();
	TargetInfo = FCortexEditorPhysicalInputTargetInfo();
	BoundPawnClass = nullptr;
}

void FCortexEditorPhysicalInputSession::BeginOwnedTermination()
{
	// A fully resolved owned teardown is never re-entered.
	if (bOwnedTerminationResolved)
	{
		return;
	}
	// The usable target is invalid the moment termination starts; the separate owned
	// world/context identity is retained to observe teardown.
	ReleaseReadyBinding();

	if (OwnedState == EOwnedState::Preparing)
	{
		// Invalidate readiness before teardown so no late callback can bind or dispatch.
		bReadyCallbackInvoked = true;
		ReadyCallback = nullptr;
	}
	OwnedState = EOwnedState::Ending;
	// The bounded pending-teardown diagnostic window starts when teardown begins.
	LastTeardownDiagnosticSeconds = FPlatformTime::Seconds();
	EnsureTicker();
	RequestOwnedTermination();
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

FCortexCommandResult FCortexEditorPhysicalInputSession::BeginOwnedPIE(
	const FString& MapAssetPath,
	int32 LocalPlayerIndex,
	TFunction<void(const FCortexCommandResult&)>&& OnReady)
{
	if (GEditor == nullptr || GEngine == nullptr)
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady, TEXT("Editor is not available"));
	}
	if (OwnedState == EOwnedState::Preparing)
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy, TEXT("Owned PIE preparation is already in progress"));
	}
	if (OwnedState == EOwnedState::Ending && !IsOwnedPIEEnded())
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy, TEXT("A previous owned PIE session is still ending"));
	}
	if (OwnedState == EOwnedState::Ready || bBound)
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy, TEXT("Session already owns a physical input target"));
	}
	// A fresh owned request would make the engine tear down whatever is already playing.
	if (HasUnrelatedPIESession())
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy,
			TEXT("An unrelated PIE session, play request or pending teardown is already active"));
	}

	FString UnsupportedSettingsReason;
	if (!ArePlaySettingsSupported(UnsupportedSettingsReason))
	{
		return MakeErrorResult(CortexErrorCodes::InvalidOperation, UnsupportedSettingsReason);
	}
	// The neutral-state admission policy applies before preparation as well as again at arming:
	// starting a PIE session from an unknown held state is never admitted.
	FCortexCommandResult PreparationNeutralError;
	if (!IsPhysicalInputNeutral(PreparationNeutralError))
	{
		return PreparationNeutralError;
	}
	if (LocalPlayerIndex < 0)
	{
		return MakeErrorResult(CortexErrorCodes::InvalidValue, TEXT("Local player index must be non-negative"));
	}
	if (MapAssetPath.IsEmpty())
	{
		return MakeErrorResult(CortexErrorCodes::InvalidValue, TEXT("Map asset path must not be empty"));
	}

	// Validate the map/package before requesting so unsaved editor state is never discarded.
	const FString PackageName = FPackageName::ObjectPathToPackageName(MapAssetPath);
	if (PackageName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName))
	{
		return MakeErrorResult(CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Map asset path '%s' is not a valid package path"), *MapAssetPath));
	}
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		return MakeErrorResult(CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Map package '%s' does not exist"), *PackageName));
	}

	// The in-process destination needs an explicit, unambiguous selected Slate viewport.
	FLevelEditorModule* LevelEditorModule =
		FModuleManager::Get().GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditorModule == nullptr)
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady,
			TEXT("The level editor module is not available for the owned PIE session"));
	}
	const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
	if (!ActiveViewport.IsValid())
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady,
			TEXT("No active level editor viewport is available for the owned PIE session"));
	}

	FRequestPlaySessionParams Request;
	Request.SessionDestination = EPlaySessionDestinationType::InProcess;
	Request.WorldType = EPlaySessionWorldType::PlayInEditor;
	Request.GlobalMapOverride = MapAssetPath;
	// StartLocation/StartRotation stay unset so the normal player start is used.
	Request.DestinationSlateViewport = ActiveViewport;

	// Record the request generation, deadline and baseline before the request is queued.
	OwnedRequest = FOwnedRequest();
	OwnedRequest.MapAssetPath = MapAssetPath;
	OwnedRequest.RequestedPackagePath = PackageName;
	OwnedRequest.LocalPlayerIndex = LocalPlayerIndex;
	OwnedRequest.DeadlineSeconds = FPlatformTime::Seconds() + CortexOwnedPIEPreparationBudgetSeconds;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			OwnedRequest.BaselinePIEContextHandles.Add(Context.ContextHandle);
		}
	}

	ReadyCallback = MoveTemp(OnReady);
	bReadyCallbackInvoked = false;
	bOwnsPIE = true;
	bOwnedRequestOutstanding = true;
	bStartupResolved = false;
	SubmittedRequestFingerprint = 0;
	SubmittedQueuedRequestFingerprint = 0;
	OwnedState = EOwnedState::Preparing;
	OwnedContextHandle = NAME_None;
	OwnedWorld = nullptr;
	bOwnedWorldObserved = false;
	bOwnedContextObserved = false;
	bOwnedContextGone = false;
	bOwnedTerminationResolved = false;
	bOwnedEndPlayRequested = false;
	LastTeardownDiagnosticSeconds = 0.0;
	BoundPawnClass = nullptr;
	LastObservedPawnClass = nullptr;
	StablePawnClassObservations = 0;
	++Generation;

	EnsureTicker();
	GEditor->RequestPlaySession(Request);

	// Capture the identity of the request the engine actually queued in this same tick.
	// DestinationSlateViewport is still present on the queued copy, so the queued identity
	// distinguishes a same-map successor that differs only in its destination viewport. The
	// consumed identity excludes it because the engine nulls the field once the session starts.
	if (const TOptional<FRequestPlaySessionParams> Queued = GEditor->GetPlaySessionRequest())
	{
		SubmittedRequestFingerprint = ComputeRequestFingerprint(Queued.GetValue(), false);
		SubmittedQueuedRequestFingerprint = ComputeRequestFingerprint(Queued.GetValue(), true);
	}

	// The request is accepted; the session does not own a target until readiness succeeds.
	return MakeSuccessResult();
}

FCortexCommandResult FCortexEditorPhysicalInputSession::BindTarget(UWorld& World, int32 LocalPlayerIndex)
{
	if (GEditor == nullptr)
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady, TEXT("Editor is not available"));
	}
	if (OwnedState == EOwnedState::Preparing || OwnedState == EOwnedState::Ready)
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy, TEXT("Session already owns a physical input target"));
	}
	if (bBound)
	{
		return MakeErrorResult(CortexErrorCodes::EditorBusy, TEXT("Session is already bound to a physical input target"));
	}

	FCortexEditorPhysicalInputTargetBinding ResolvedBinding;
	FCortexEditorPhysicalInputTargetInfo ResolvedInfo;
	FCortexCommandResult ResolutionError;
	if (!ResolveTarget(World, LocalPlayerIndex, false, ResolvedBinding, ResolvedInfo, ResolutionError))
	{
		return ResolutionError;
	}

	Binding = ResolvedBinding;
	TargetInfo = ResolvedInfo;
	BoundPawnClass = ResolvedBinding.Pawn.Get()->GetClass();
	bBound = true;
	bOwnsPIE = false;
	++Generation;
	AttachCaptureToBinding();
	return MakeSuccessResult();
}

bool FCortexEditorPhysicalInputSession::ValidateTarget(FCortexCommandResult& OutError) const
{
	if (!bBound)
	{
		OutError = MakeInvalidTargetResult(TEXT("No physical input target is bound"));
		return false;
	}

	UWorld* World = Binding.World.Get();
	if (World == nullptr || World->WorldType != EWorldType::PIE)
	{
		OutError = MakeInvalidTargetResult(TEXT("Bound world is no longer a running PIE world"));
		return false;
	}

	APlayerController* Controller = Binding.Controller.Get();
	APawn* Pawn = Binding.Pawn.Get();
	if (!IsValid(Controller) || !IsValid(Pawn))
	{
		OutError = MakeInvalidTargetResult(TEXT("Bound controller or pawn is no longer valid"));
		return false;
	}
	if (Controller->GetPawn() != Pawn)
	{
		OutError = MakeInvalidTargetResult(TEXT("Bound pawn was replaced instead of being adopted"));
		return false;
	}
	if (BoundPawnClass.IsValid() && Pawn->GetClass() != BoundPawnClass.Get())
	{
		OutError = MakeInvalidTargetResult(TEXT("Bound pawn class changed"));
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	ULocalPlayer* LocalPlayer = GameInstance != nullptr
		? GameInstance->GetLocalPlayerByIndex(TargetInfo.LocalPlayerIndex)
		: nullptr;
	if (LocalPlayer == nullptr || LocalPlayer->GetPlayerController(World) != Controller)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected local player no longer owns the bound controller"));
		return false;
	}

	UGameViewportClient* ViewportClient = World->GetGameViewport();
	if (ViewportClient == nullptr
		|| ViewportClient->GetGameViewport() == nullptr
		|| !ViewportClient->GetGameLayerManager().IsValid())
	{
		OutError = MakeInvalidTargetResult(TEXT("Bound viewport is no longer registered with its viewport client"));
		return false;
	}

	return true;
}

const FCortexEditorPhysicalInputTargetInfo& FCortexEditorPhysicalInputSession::GetTargetInfo() const
{
	return TargetInfo;
}

const FCortexEditorPhysicalInputTargetBinding& FCortexEditorPhysicalInputSession::GetTargetBinding() const
{
	return Binding;
}

FCortexCommandResult FCortexEditorPhysicalInputSession::ReadPlayerPose(FCortexEditorPhysicalInputPlayerPose& Out) const
{
	FCortexCommandResult TargetError;
	if (!ValidateTarget(TargetError))
	{
		return TargetError;
	}

	APawn* Pawn = Binding.Pawn.Get();
	APlayerController* Controller = Binding.Controller.Get();
	Out.PawnTransform = Pawn->GetActorTransform();
	Out.ControlRotation = Controller->GetControlRotation();

	if (!IsPoseUsable(Out))
	{
		return MakeErrorResult(CortexErrorCodes::InvalidValue, TEXT("Read player pose is not finite or is degenerate"));
	}
	return MakeSuccessResult();
}

FCortexCommandResult FCortexEditorPhysicalInputSession::ComparePlayerPose(
	const FCortexEditorPhysicalInputPlayerPose& Expected,
	const FCortexEditorPhysicalInputPlayerPose& Actual)
{
	if (!IsPoseUsable(Expected) || !IsPoseUsable(Actual))
	{
		return MakeErrorResult(CortexErrorCodes::InvalidValue, TEXT("Player pose is not finite or is degenerate"));
	}

	constexpr double PositionToleranceCm = 0.5;
	constexpr double RotationToleranceDegrees = 0.5;
	constexpr double ScaleTolerance = 0.001;

	const double PositionDelta = FVector::Dist(
		Expected.PawnTransform.GetLocation(), Actual.PawnTransform.GetLocation());
	const double PawnRotationDeltaDegrees = FMath::RadiansToDegrees(
		Expected.PawnTransform.GetRotation().AngularDistance(Actual.PawnTransform.GetRotation()));
	const double ControlRotationDeltaDegrees = FMath::RadiansToDegrees(
		FQuat(Expected.ControlRotation).AngularDistance(FQuat(Actual.ControlRotation)));

	const FVector ExpectedScale = Expected.PawnTransform.GetScale3D();
	const FVector ActualScale = Actual.PawnTransform.GetScale3D();
	const double MaxScaleDelta = FMath::Max3(
		FMath::Abs(ExpectedScale.X - ActualScale.X),
		FMath::Abs(ExpectedScale.Y - ActualScale.Y),
		FMath::Abs(ExpectedScale.Z - ActualScale.Z));

	if (PositionDelta <= PositionToleranceCm
		&& PawnRotationDeltaDegrees <= RotationToleranceDegrees
		&& ControlRotationDeltaDegrees <= RotationToleranceDegrees
		&& MaxScaleDelta <= ScaleTolerance)
	{
		return MakeSuccessResult();
	}

	FCortexCommandResult Mismatch = MakeErrorResult(CortexEditorPhysicalInputErrorCodes::PoseMismatch,
		TEXT("Player pose deviates beyond the fixed guard tolerances"));
	Mismatch.AddContext(TEXT("position_delta_cm"), PositionDelta);
	Mismatch.AddContext(TEXT("pawn_rotation_delta_degrees"), PawnRotationDeltaDegrees);
	Mismatch.AddContext(TEXT("control_rotation_delta_degrees"), ControlRotationDeltaDegrees);
	Mismatch.AddContext(TEXT("max_scale_delta"), MaxScaleDelta);
	return Mismatch;
}

FCortexCommandResult FCortexEditorPhysicalInputSession::RestorePlayerPose(
	const FCortexEditorPhysicalInputPlayerPose& Pose,
	const FString& RecordedPawnClassPath)
{
	FCortexCommandResult TargetError;
	if (!ValidateTarget(TargetError))
	{
		return TargetError;
	}

	APawn* Pawn = Binding.Pawn.Get();
	APlayerController* Controller = Binding.Controller.Get();

	// Verify the recorded class through a guarded load; never compare class-name strings.
	UClass* RecordedClass = ResolveRecordedPawnClass(RecordedPawnClassPath);
	if (RecordedClass == nullptr
		|| !RecordedClass->IsChildOf(APawn::StaticClass())
		|| !Pawn->IsA(RecordedClass))
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Recorded pawn class does not match the bound pawn"));
	}

	// Reject unusable recorded data before any mutation; only a usable mismatch restores.
	if (!IsPoseUsable(Pose))
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Recorded player pose is not finite or is degenerate"));
	}

	FCortexEditorPhysicalInputPlayerPose CurrentPose;
	if (!ReadPlayerPose(CurrentPose).bSuccess)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Unable to read the current player pose before restoration"));
	}

	// Mutate only on an actual mismatch; an exact match is already restored.
	if (!ComparePlayerPose(Pose, CurrentPose).bSuccess)
	{
		if (!Pawn->SetActorTransform(Pose.PawnTransform, false, nullptr, ETeleportType::TeleportPhysics))
		{
			return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
				TEXT("The bound pawn rejected the recorded transform (no movable root)"));
		}
		Controller->SetControlRotation(Pose.ControlRotation);
	}

	// Revalidate identities after the virtual calls; a replaced pawn is not adopted.
	FCortexCommandResult RevalidateError;
	if (!ValidateTarget(RevalidateError))
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Target changed while restoring the initial player pose"));
	}

	FCortexEditorPhysicalInputPlayerPose Readback;
	if (!ReadPlayerPose(Readback).bSuccess)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Unable to read back the restored player pose"));
	}
	if (!ComparePlayerPose(Pose, Readback).bSuccess)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed,
			TEXT("Restored player pose does not match the recorded pose"));
	}
	return MakeSuccessResult();
}

void FCortexEditorPhysicalInputSession::EndOwnedPIE()
{
	if (!bOwnsPIE)
	{
		// Already-ended / non-owned sessions report complete without touching foreign PIE.
		OwnedState = EOwnedState::Ending;
		return;
	}
	BeginOwnedTermination();
}

bool FCortexEditorPhysicalInputSession::IsOwnedPIEEnded() const
{
	if (!bOwnsPIE)
	{
		return true;
	}
	// Startup must be resolved before completion can be claimed: a request the engine
	// already consumed must not be mistaken for "nothing was ever created".
	if (!bStartupResolved)
	{
		return false;
	}
	if (IsOwnedRequestPending())
	{
		return false;
	}
	// Complete only once the exact captured owned context is absent too.
	return !IsOwnedContextPresent();
}

void FCortexEditorPhysicalInputSession::Shutdown()
{
	// Teardown must only clear this operation's own cached state: neutralize owned synthetic input
	// while the binding is still available, so no synthetic key/button stays cached in Slate.
	if (bBound && FSlateApplication::IsInitialized())
	{
		ReleaseHeldInputs();
	}
	LastObservedPawnClass = nullptr;
	StablePawnClassObservations = 0;
	ReleaseReadyBinding();
	bReadyCallbackInvoked = true;
	ReadyCallback = nullptr;

	if (bOwnsPIE && !bOwnedTerminationResolved && !IsOwnedPIEEnded())
	{
		if (OwnedState != EOwnedState::Ending)
		{
			BeginOwnedTermination();
		}
		// Keep the minimal teardown observer alive until the owned context is gone.
		EnsureTicker();
	}
	else
	{
		// Resolved (or never owned): no further teardown work and no ticker.
		OwnedState = EOwnedState::None;
		RemoveTicker();
	}
}

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

bool FCortexEditorPhysicalInputSession::ResolveTarget(
	UWorld& World,
	int32 LocalPlayerIndex,
	bool bRequireStablePawnClass,
	FCortexEditorPhysicalInputTargetBinding& OutBinding,
	FCortexEditorPhysicalInputTargetInfo& OutInfo,
	FCortexCommandResult& OutError)
{
	if (World.WorldType != EWorldType::PIE)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected world is not a running PIE world"));
		return false;
	}
	if (LocalPlayerIndex < 0)
	{
		OutError = MakeErrorResult(CortexErrorCodes::InvalidValue, TEXT("Local player index must be non-negative"));
		return false;
	}

	UGameInstance* GameInstance = World.GetGameInstance();
	if (GameInstance == nullptr)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected world has no game instance"));
		return false;
	}

	ULocalPlayer* LocalPlayer = GameInstance->GetLocalPlayerByIndex(LocalPlayerIndex);
	if (LocalPlayer == nullptr)
	{
		OutError = MakeErrorResult(CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Selected local player %d is not present in the selected PIE world"), LocalPlayerIndex));
		return false;
	}

	APlayerController* Controller = LocalPlayer->GetPlayerController(&World);
	if (Controller == nullptr || Controller->GetWorld() != &World)
	{
		OutError = MakeInvalidTargetResult(
			TEXT("Selected local player has no player controller in the selected PIE world"));
		return false;
	}

	APawn* Pawn = Controller->GetPawn();
	if (!IsValid(Pawn))
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected player controller does not possess a valid pawn"));
		return false;
	}

	UGameViewportClient* ViewportClient = World.GetGameViewport();
	if (ViewportClient == nullptr)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected PIE world has no game viewport client"));
		return false;
	}
	FSceneViewport* SceneViewport = ViewportClient->GetGameViewport();
	if (SceneViewport == nullptr)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected PIE viewport is not registered with its viewport client"));
		return false;
	}
	TSharedPtr<IGameLayerManager> LayerManager = ViewportClient->GetGameLayerManager();
	if (!LayerManager.IsValid())
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected PIE viewport has no initialized game layer manager"));
		return false;
	}

	TSharedPtr<FSlateUser> SlateUser = LocalPlayer->GetSlateUser();
	if (!SlateUser.IsValid())
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected local player has no Slate user"));
		return false;
	}

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId PlatformUserId = SlateUser->GetPlatformUserId();
	FInputDeviceId InputDevice = DeviceMapper.GetPrimaryInputDeviceForUser(PlatformUserId);
	if (!InputDevice.IsValid())
	{
		InputDevice = DeviceMapper.GetDefaultInputDevice();
	}
	if (!InputDevice.IsValid())
	{
		OutError = MakeInvalidTargetResult(TEXT("No keyboard/mouse input device is mapped for the selected user"));
		return false;
	}
	if (DeviceMapper.GetUserForInputDevice(InputDevice) != PlatformUserId)
	{
		OutError = MakeInvalidTargetResult(TEXT("Selected input device ownership is ambiguous"));
		return false;
	}

	// Require the pawn class to remain stable across fully-ready consecutive frames.
	if (bRequireStablePawnClass)
	{
		UClass* PawnClass = Pawn->GetClass();
		if (LastObservedPawnClass.Get() == PawnClass)
		{
			++StablePawnClassObservations;
		}
		else
		{
			LastObservedPawnClass = PawnClass;
			StablePawnClassObservations = 1;
		}
		if (StablePawnClassObservations < CortexStablePawnClassObservationsRequired)
		{
			OutError = MakeInvalidTargetResult(TEXT("Pawn class has not been stable across consecutive frames"));
			return false;
		}
	}

	OutBinding = FCortexEditorPhysicalInputTargetBinding();
	OutBinding.World = &World;
	OutBinding.Controller = Controller;
	OutBinding.Pawn = Pawn;
	// Runtime UI overlays are siblings of the render viewport, so bind the layer manager root.
	OutBinding.InputRoot = LayerManager->AsWidget();
	OutBinding.ViewportWidget = SceneViewport->GetViewportWidget();
	OutBinding.SlateUserIndex = SlateUser->GetUserIndex();
	OutBinding.InputDevice = InputDevice;

	OutInfo = FCortexEditorPhysicalInputTargetInfo();
	OutInfo.MapAssetPath = UWorld::RemovePIEPrefix(World.GetPackage()->GetName());
	OutInfo.PawnClassPath = Pawn->GetClass()->GetPathName();
	OutInfo.LocalPlayerIndex = LocalPlayerIndex;
	OutInfo.ViewportSize = SceneViewport->GetSizeXY();
	if (const TSharedPtr<SWindow> ViewportWindow = ViewportClient->GetWindow())
	{
		OutInfo.DpiScale = ViewportWindow->GetDPIScaleFactor();
	}
	else
	{
		OutInfo.DpiScale = 1.0;
	}
	return true;
}

bool FCortexEditorPhysicalInputSession::IsPoseUsable(const FCortexEditorPhysicalInputPlayerPose& Pose)
{
	const FTransform& Transform = Pose.PawnTransform;
	if (Transform.ContainsNaN() || Pose.ControlRotation.ContainsNaN())
	{
		return false;
	}

	const FQuat& Rotation = Transform.GetRotation();
	if (!FMath::IsFinite(Rotation.W)
		|| !FMath::IsFinite(Rotation.X)
		|| !FMath::IsFinite(Rotation.Y)
		|| !FMath::IsFinite(Rotation.Z))
	{
		return false;
	}
	if (Rotation.SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
	{
		return false; // Degenerate zero quaternion.
	}

	const FVector Scale = Transform.GetScale3D();
	if (!FMath::IsFinite(Scale.X) || !FMath::IsFinite(Scale.Y) || !FMath::IsFinite(Scale.Z))
	{
		return false;
	}
	return true;
}

UClass* FCortexEditorPhysicalInputSession::ResolveRecordedPawnClass(const FString& RecordedPawnClassPath)
{
	if (RecordedPawnClassPath.IsEmpty())
	{
		return nullptr;
	}

	// Prefer an already loaded object; /Script classes resolve from loaded objects only.
	if (UClass* Loaded = FindObject<UClass>(nullptr, *RecordedPawnClassPath))
	{
		return Loaded;
	}

	// Guard Blueprint class package loading to avoid SkipPackage warnings.
	const FString PackageName = FPackageName::ObjectPathToPackageName(RecordedPawnClassPath);
	if (PackageName.IsEmpty())
	{
		return nullptr;
	}
	if (FindPackage(nullptr, *PackageName) == nullptr && !FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	return LoadObject<UClass>(nullptr, *RecordedPawnClassPath);
}

// ---------------------------------------------------------------------------
// Task 3: non-consuming capture, tagged-route UI observation, normal dispatch and
// original-target cleanup.
// ---------------------------------------------------------------------------

namespace
{
	/** Bit set in the short returned by GetAsyncKeyState when the key is currently down. */
	constexpr short CortexPhysicalKeyDownBit = static_cast<short>(0x8000);

#if PLATFORM_WINDOWS && WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test-support override for the physical-key snapshot. Null in production, where the real
	 * GetAsyncKeyState high-bit state is used. Compiled only into automation-enabled builds.
	 */
	TFunction<bool(const FKey&)> GPhysicalKeySnapshotResolver;
#endif

	struct FCortexPhysicalSupportedKey
	{
		FKey Key;
		int32 VirtualKey;
	};

	/** Supported key identities the snapshot cannot resolve; empty on a complete domain. */
	TArray<FString>& GetUnresolvedSupportedKeys()
	{
		static TArray<FString> Keys;
		return Keys;
	}

	/**
	 * The complete supported keyboard/mouse domain, built once and cached as (FKey, Windows
	 * virtual-key) pairs deduplicated by FKey identity. The capture path can record any key the OS
	 * reports as a key event, so admission must resolve every supported identity from the OS
	 * high-bit snapshot.
	 *
	 * The engine splits the domain across two platform maps: `GetKeyMap` provides the
	 * non-printable/navigation/mouse keys keyed by virtual key, while `GetCharKeyMap` provides the
	 * printable keys keyed by character code. Both are merged; the virtual key is the engine's own
	 * (`FInputKeyManager`), and the printable character code's virtual key comes from the
	 * platform's `VkKeyScanW`. Deduplication is by FKey, not by virtual key, so distinct shifted
	 * identities that share one virtual key (Semicolon/Colon, Apostrophe/Quote) each remain
	 * resolvable. A printable code that has no virtual key on the current layout is not part of
	 * the supported set; a supported identity that cannot be resolved is recorded and makes
	 * `CanResolvePhysicalKeyState` fail rather than silently reporting neutral.
	 */
	const TArray<FCortexPhysicalSupportedKey>& GetSupportedPhysicalKeys()
	{
#if PLATFORM_WINDOWS
		static const TArray<FCortexPhysicalSupportedKey> Keys = []()
		{
			constexpr uint32 MaxMappings = 256;
			uint32 KeyCodes[MaxMappings];
			FString KeyNames[MaxMappings];
			const uint32 KeyCount = FPlatformInput::GetKeyMap(KeyCodes, KeyNames, MaxMappings);
			uint32 CharCodes[MaxMappings];
			FString CharKeyNames[MaxMappings];
			const uint32 CharCount = FPlatformInput::GetCharKeyMap(CharCodes, CharKeyNames, MaxMappings);

			TArray<FCortexPhysicalSupportedKey> Mappings;
			TSet<FKey> SeenKeys;
			TArray<FString>& Unresolved = GetUnresolvedSupportedKeys();
			Unresolved.Reset();
			auto AddEntry = [&Mappings, &SeenKeys](const FKey& Key, uint32 VirtualKey)
			{
				if (!Key.IsValid() || VirtualKey == 0 || VirtualKey > 0xFF || SeenKeys.Contains(Key))
				{
					return;
				}
				SeenKeys.Add(Key);
				FCortexPhysicalSupportedKey Mapping;
				Mapping.Key = Key;
				Mapping.VirtualKey = static_cast<int32>(VirtualKey);
				Mappings.Add(Mapping);
			};

			// The platform key map names every OS-reported non-printable/mouse key by virtual key.
			for (uint32 Index = 0; Index < KeyCount; ++Index)
			{
				const uint32 VirtualKey = KeyCodes[Index];
				if (VirtualKey == 0 || VirtualKey > 0xFF)
				{
					continue;
				}
				const FKey Key = FInputKeyManager::Get().GetKeyFromCodes(VirtualKey, VirtualKey);
				if (!Key.IsValid())
				{
					Unresolved.Add(FString::Printf(TEXT("virtual_key_0x%04X"), VirtualKey));
					continue;
				}
				AddEntry(Key, VirtualKey);
			}
			// The printable map is keyed by character code; its virtual key comes from the
			// platform's own character->virtual-key mapping.
			for (uint32 Index = 0; Index < CharCount; ++Index)
			{
				const uint32 CharCode = CharCodes[Index];
				if (CharCode == 0)
				{
					continue;
				}
				const SHORT Scanned = VkKeyScanW(static_cast<wchar_t>(CharCode));
				if (Scanned == -1)
				{
					// No virtual key on this layout: not part of the supported domain.
					UE_LOG(LogCortexEditor, Verbose,
						TEXT("Physical key snapshot: printable code 0x%04X has no virtual key on this layout; not supported"),
						CharCode);
					continue;
				}
				const uint32 VirtualKey = static_cast<uint32>(static_cast<uint16>(Scanned)) & 0xFF;
				const FKey Key = FInputKeyManager::Get().GetKeyFromCodes(0, CharCode);
				if (!Key.IsValid())
				{
					Unresolved.Add(FString::Printf(TEXT("printable_0x%04X"), CharCode));
					continue;
				}
				AddEntry(Key, VirtualKey);
			}
			// OEM punctuation virtual keys that neither map names are supported OS keys and are
			// added directly so no punctuation key is left unresolvable.
			static const uint32 OemVirtualKeys[] = {
				VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
				VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_OEM_8, VK_OEM_102 };
			for (const uint32 VirtualKey : OemVirtualKeys)
			{
				const FKey Key = FInputKeyManager::Get().GetKeyFromCodes(VirtualKey, VirtualKey);
				if (!Key.IsValid())
				{
					Unresolved.Add(FString::Printf(TEXT("virtual_key_0x%04X"), VirtualKey));
					continue;
				}
				AddEntry(Key, VirtualKey);
			}
			return Mappings;
		}();
		return Keys;
#else
		static const TArray<FCortexPhysicalSupportedKey> Keys;
		return Keys;
#endif
	}

	/**
	 * True only when the snapshot is genuinely resolvable: a non-empty domain with no supported
	 * identity left unresolved. An incomplete domain must FAIL rather than report neutral.
	 */
	bool CanResolvePhysicalKeyState()
	{
#if PLATFORM_WINDOWS
		return GetSupportedPhysicalKeys().Num() > 0 && GetUnresolvedSupportedKeys().Num() == 0;
#else
		return false;
#endif
	}

	/** The raw async high-bit state for one supported key (0 when unresolvable). */
	short PhysicalKeyAsyncState(const FCortexPhysicalSupportedKey& Supported)
	{
#if PLATFORM_WINDOWS
#if WITH_DEV_AUTOMATION_TESTS
		if (GPhysicalKeySnapshotResolver)
		{
			return GPhysicalKeySnapshotResolver(Supported.Key) ? CortexPhysicalKeyDownBit : 0;
		}
#endif
		return ::GetAsyncKeyState(Supported.VirtualKey);
#else
		return 0;
#endif
	}

	/**
	 * Slate applies a reply's mouse capture, mouse position and mouse lock only while the
	 * application is active (SlateApplication.cpp:3494 gates on
	 * `bHandleDeviceInputWhenApplicationNotActive || bAppIsActive || bIsVirtualInteraction`).
	 * A programmatic dispatch must behave identically whether or not the editor window currently
	 * holds OS focus, so inactive-input handling is enabled for the routing window and restored.
	 */
	class FCortexScopedInactivePhysicalInput : private FNoncopyable
	{
	public:
		explicit FCortexScopedInactivePhysicalInput(FSlateApplication& InSlate)
			: Slate(InSlate)
			, bWasEnabled(InSlate.GetHandleDeviceInputWhenApplicationNotActive())
		{
			Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
		}

		~FCortexScopedInactivePhysicalInput()
		{
			Slate.SetHandleDeviceInputWhenApplicationNotActive(bWasEnabled);
		}

	private:
		FSlateApplication& Slate;
		bool bWasEnabled = false;
	};

	/**
	 * Resolves the real hit path for one screen coordinate inside the selected viewport's own
	 * window, using the same public hit-test route Slate itself uses for mouse routing.
	 */
	bool ResolveSelectedRoutePath(FSlateApplication& Slate, int32 SlateUserIndex,
		const TSharedPtr<SWidget>& Anchor, const FVector2D& ScreenSpacePosition, FWidgetPath& OutPath)
	{
		OutPath = FWidgetPath();
		if (!Anchor.IsValid())
		{
			return false;
		}
		const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Anchor.ToSharedRef());
		if (!Window.IsValid())
		{
			return false;
		}
		TArray<TSharedRef<SWindow>> SelectedWindows;
		SelectedWindows.Add(Window.ToSharedRef());
		OutPath = Slate.LocateWindowUnderMouse(
			ScreenSpacePosition, SelectedWindows, /*bIgnoreEnabledStatus*/ false, SlateUserIndex);
		return OutPath.IsValid();
	}

	/**
	 * Cleanup-only input shield. It consumes exactly the tagged synthetic cleanup Up this
	 * operation generates before normal widget routing, and nothing else; the capture processor
	 * stays non-consuming for every event.
	 */
	class FCortexPhysicalInputCleanupShield : public IInputProcessor
	{
	public:
		virtual void Tick(const float /*DeltaTime*/, FSlateApplication& /*SlateApp*/,
			TSharedRef<ICursor> /*Cursor*/) override
		{
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& /*SlateApp*/,
			const FPointerEvent& MouseEvent) override
		{
			if (bConsumed || MouseEvent.GetEffectingButton() != ExpectedButton)
			{
				return false;
			}
			if (MouseEvent.GetInputDeviceId() != ExpectedDevice)
			{
				return false;
			}
			bConsumed = true;
			return true;
		}

		FKey ExpectedButton;
		FInputDeviceId ExpectedDevice = INPUTDEVICEID_NONE;
		bool bConsumed = false;
	};
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
	TFunction<bool(const FKey&)>&& Resolver)
{
#if PLATFORM_WINDOWS
	GPhysicalKeySnapshotResolver = MoveTemp(Resolver);
#else
	(void)Resolver;
#endif
}

void FCortexEditorPhysicalInputSession::ClearPhysicalKeySnapshotResolver()
{
#if PLATFORM_WINDOWS
	GPhysicalKeySnapshotResolver = nullptr;
#endif
}

TArray<FString> FCortexEditorPhysicalInputSession::GetUnresolvedSupportedKeyNames()
{
	// Force the domain build so the unresolved set reflects the current platform maps.
	(void)GetSupportedPhysicalKeys();
	return GetUnresolvedSupportedKeys();
}
#endif // WITH_DEV_AUTOMATION_TESTS

void FCortexEditorPhysicalInputSession::AttachCaptureToBinding()
{
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}
	if (!DispatchContext.IsValid())
	{
		DispatchContext = MakeShared<FCortexEditorPhysicalInputDispatchContext>();
	}
	if (!GuardState.IsValid())
	{
		GuardState = MakeShared<FCortexEditorPhysicalInputGuardState>();
	}
	// Retain the exact PlayerInput instance this binding was acquired with; cleanup never touches a
	// replacement.
	BoundPlayerInput = Binding.Controller.IsValid() ? Binding.Controller->PlayerInput : nullptr;
	if (CaptureProcessor.IsValid())
	{
		return;
	}

	CaptureProcessor = MakeShared<FCortexEditorPhysicalInputCaptureProcessor>(*this);
	if (FSlateApplication::IsInitialized())
	{
		// CommonUI/CommonInput consume input from the PreGame bucket
		// (CommonInputSubsystem.cpp registers there), so observe from the earlier PreEngine bucket
		// to guarantee this non-consuming processor always runs first.
		FSlateApplication::Get().RegisterInputPreProcessor(
			CaptureProcessor, EInputPreProcessorType::PreEngine);
	}
}

void FCortexEditorPhysicalInputSession::DetachCapture()
{
	if (CaptureProcessor.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(CaptureProcessor);
		}
		CaptureProcessor.Reset();
	}
	CaptureCallbackImpl = nullptr;
	InterruptionCallbackImpl = nullptr;
	BoundPlayerInput = nullptr;
	bDispatchFrozen = false;
	bReplayInProgress = false;
	if (DispatchContext.IsValid())
	{
		DispatchContext->SyntheticDepth = 0;
	}
	if (CaptureState.IsValid())
	{
		CaptureState->bArmed = false;
		CaptureState->bInterrupted = false;
		CaptureState->bInconsistentModifiers = false;
		CaptureState->bFaulted = false;
		CaptureState->bOwnedPointerCapture = false;
		CaptureState->OwnedCaptorPath.Reset();
		CaptureState->OwnedPointerEvent = FPointerEvent();
		CaptureState->bOwnedDragDrop = false;
		CaptureState->OwnedDragDropContent.Reset();
		CaptureState->bOwnedHighPrecision = false;
		CaptureState->bOwnedNativeCapture = false;
		CaptureState->OwnedNativeCaptureHandle = nullptr;
		CaptureState->ObservedHeldKeys.Reset();
		CaptureState->ObservedHeldButtons.Reset();
		CaptureState->ObservedModifierKeys.Reset();
		CaptureState->CapturedHeldKeys.Reset();
		CaptureState->CapturedHeldButtons.Reset();
		CaptureState->CapturedModifierKeys.Reset();
		CaptureState->OwnedSyntheticKeys.Reset();
		CaptureState->OwnedSyntheticButtons.Reset();
		CaptureState->ForeignHeldKeys.Reset();
		CaptureState->ForeignHeldButtons.Reset();
		CaptureState->ForeignInFlight.Reset();
		CaptureState->SyntheticFocusOwner.Reset();
	}
}

bool FCortexEditorPhysicalInputSession::IsSelectedUserAndDevice(
	uint32 UserIndex, const FInputDeviceId& Device) const
{
	if (!bBound)
	{
		return false;
	}
	return UserIndex == static_cast<uint32>(Binding.SlateUserIndex) && Device == Binding.InputDevice;
}

FVector2D FCortexEditorPhysicalInputSession::ToViewportPosition(const FVector2D& ScreenSpacePosition) const
{
	const TSharedPtr<SWidget> Viewport = GetCoordinateRootWidget();
	if (!Viewport.IsValid())
	{
		return ScreenSpacePosition;
	}
	return Viewport->GetCachedGeometry().AbsoluteToLocal(ScreenSpacePosition);
}

FVector2D FCortexEditorPhysicalInputSession::ToScreenSpacePosition(const FVector2D& ViewportPosition) const
{
	const TSharedPtr<SWidget> Viewport = GetCoordinateRootWidget();
	if (!Viewport.IsValid())
	{
		return ViewportPosition;
	}
	return Viewport->GetCachedGeometry().LocalToAbsolute(ViewportPosition);
}

TSharedPtr<SWidget> FCortexEditorPhysicalInputSession::GetCoordinateRootWidget() const
{
	// The scene viewport widget normally owns the viewport-local mapping; the layer-manager root
	// covers targets whose scene viewport widget is not exposed.
	if (const TSharedPtr<SWidget> ViewportWidget = Binding.ViewportWidget.Pin())
	{
		return ViewportWidget;
	}
	return Binding.InputRoot.Pin();
}

bool FCortexEditorPhysicalInputSession::IsPhysicalInputNeutral(FCortexCommandResult& OutError) const
{
	static const TCHAR* const ReleaseInstruction =
		TEXT("Release all held keys, mouse buttons and modifiers before starting capture");

	auto Deny = [&OutError](const FString& Detail)
	{
		const FString Message = Detail.IsEmpty()
			? FString(ReleaseInstruction)
			: FString::Printf(TEXT("%s (holding: %s)"), ReleaseInstruction, *Detail);
		UE_LOG(LogCortexEditor, Display, TEXT("Physical input admission rejected: %s"), *Message);
		OutError = MakeErrorResult(CortexErrorCodes::InvalidOperation, Message);
		return false;
	};

	// The observation sets are the authoritative attributable channel: the processor records every
	// selected-user/device down edge (including UI-consumed mouse presses) even before arming.
	if (CaptureState.IsValid())
	{
		TArray<FString> Held;
		TSet<FString> Seen;
		auto AddHeld = [&Held, &Seen](const TSet<FKey>& Keys)
		{
			for (const FKey& Key : Keys)
			{
				const FString Name = Key.ToString();
				if (!Seen.Contains(Name))
				{
					Seen.Add(Name);
					Held.Add(Name);
				}
			}
		};
		AddHeld(CaptureState->ObservedHeldKeys);
		AddHeld(CaptureState->ObservedHeldButtons);
		AddHeld(CaptureState->ObservedModifierKeys);
		if (Held.Num() > 0)
		{
			return Deny(FString::Printf(TEXT("observed[%s]"), *FString::Join(Held, TEXT(","))));
		}
	}
	// A neutral state can only be claimed when the physical snapshot is genuinely resolvable over
	// the complete supported domain; assuming neutrality where a supported identity is unresolved
	// would admit capture from an unknown held state.
	if (!CanResolvePhysicalKeyState())
	{
		const TArray<FString>& Unresolved = GetUnresolvedSupportedKeys();
		OutError = MakeErrorResult(CortexErrorCodes::InvalidOperation,
			Unresolved.Num() > 0
				? FString::Printf(TEXT("Physical keyboard/mouse state cannot be resolved for supported keys: %s"),
					*FString::Join(Unresolved, TEXT(",")))
				: FString(TEXT("Physical keyboard/mouse state cannot be resolved on this platform")));
		return false;
	}
	if (const APlayerController* Controller = Binding.Controller.Get())
	{
		for (const FCortexPhysicalSupportedKey& Supported : GetSupportedPhysicalKeys())
		{
			if (Controller->IsInputKeyDown(Supported.Key))
			{
				return Deny(FString::Printf(TEXT("controller[%s]"), *Supported.Key.ToString()));
			}
		}
	}
	for (const FCortexPhysicalSupportedKey& Supported : GetSupportedPhysicalKeys())
	{
		const short Raw = PhysicalKeyAsyncState(Supported);
		if ((Raw & CortexPhysicalKeyDownBit) == 0)
		{
			continue;
		}
		// Raw-value evidence: the held report names the key, its virtual-key code and the exact
		// GetAsyncKeyState return, so a mapping/variant fault can be told apart from genuine
		// ambient host keyboard input in the run log.
		UE_LOG(LogCortexEditor, Display,
			TEXT("Physical snapshot reports %s held (VK=0x%04X raw=0x%04X)"),
			*Supported.Key.ToString(), static_cast<uint32>(Supported.VirtualKey),
			static_cast<uint32>(static_cast<uint16>(Raw)));
		return Deny(FString::Printf(TEXT("snapshot[%s]"), *Supported.Key.ToString()));
	}
	return true;
}

FCortexCommandResult FCortexEditorPhysicalInputSession::ArmCapture(
	TFunction<void(const FCortexEditorPhysicalInputEvent&, double,
		const FCortexEditorPhysicalInputCaptureContext&)>&& Callback)
{
	FCortexCommandResult Error;
	if (!ValidateTarget(Error))
	{
		return Error;
	}
	if (!IsPhysicalInputNeutral(Error))
	{
		return Error;
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}
	if (!DispatchContext.IsValid())
	{
		DispatchContext = MakeShared<FCortexEditorPhysicalInputDispatchContext>();
	}

	CaptureCallbackImpl = MoveTemp(Callback);
	CaptureState->bArmed = true;
	CaptureState->bInterrupted = false;
	CaptureState->bInconsistentModifiers = false;
	CaptureState->bFaulted = false;
	CaptureState->bOwnedPointerCapture = false;
	CaptureState->OwnedCaptorPath.Reset();
	CaptureState->OwnedPointerEvent = FPointerEvent();
	CaptureState->bOwnedDragDrop = false;
	CaptureState->OwnedDragDropContent.Reset();
	CaptureState->bOwnedHighPrecision = false;
	CaptureState->bOwnedNativeCapture = false;
	CaptureState->OwnedNativeCaptureHandle = nullptr;
	CaptureState->CaptureEpochSeconds = FPlatformTime::Seconds();
	CaptureState->CapturedHeldKeys.Reset();
	CaptureState->CapturedHeldButtons.Reset();
	CaptureState->CapturedModifierKeys.Reset();
	return MakeSuccessResult();
}

void FCortexEditorPhysicalInputSession::ObserveProcessorKey(const FKeyEvent& KeyEvent, bool bKeyDown)
{
	if (!IsSelectedUserAndDevice(KeyEvent.GetUserIndex(), KeyEvent.GetInputDeviceId()))
	{
		return;
	}
	// Generated events are this session's own dispatch: never human state, never recorded.
	if (DispatchContext.IsValid() && DispatchContext->SyntheticDepth > 0)
	{
		return;
	}
	const FKey Key = KeyEvent.GetKey();
	if (!Key.IsValid())
	{
		return;
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}

	// Device-wide down state is still observed for capture admission, independent of the route.
	if (bKeyDown)
	{
		CaptureState->ObservedHeldKeys.Add(Key);
	}
	else
	{
		CaptureState->ObservedHeldKeys.Remove(Key);
	}
	if (Key.IsModifierKey())
	{
		if (bKeyDown)
		{
			CaptureState->ObservedModifierKeys.Add(Key);
		}
		else
		{
			CaptureState->ObservedModifierKeys.Remove(Key);
		}
	}

	// A physical edge for a key this operation already owns synthetically (or during unattended
	// replay) is foreign: record that ownership BEFORE notifying interruption so cleanup can never
	// clear or release a foreign key, and clear it only on the matching Up.
	if (bKeyDown)
	{
		if (CaptureState->OwnedSyntheticKeys.Contains(Key) || bReplayInProgress)
		{
			CaptureState->ForeignInFlight.Add(Key);
			CaptureState->ForeignHeldKeys.Add(Key);
		}
	}
	else
	{
		CaptureState->ForeignInFlight.Remove(Key);
		CaptureState->ForeignHeldKeys.Remove(Key);
	}

	if (bReplayInProgress)
	{
		NotifyInterruption(MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Foreign physical input interrupted unattended playback")));
	}
	if (!CaptureState->bArmed)
	{
		return;
	}

	// A matching captured release stays recorded after focus leaves the viewport. Every other new
	// edge must actually be on the selected target route, so ordinary same-user editor input is
	// not captured as gameplay/UI input.
	const bool bMatchingRelease = !bKeyDown && CaptureState->CapturedHeldKeys.Contains(Key);
	if (!bMatchingRelease && !IsKeyboardFocusOnSelectedRoute())
	{
		return;
	}

	FCortexEditorPhysicalInputEvent Event;
	Event.Kind = bKeyDown
		? ECortexEditorPhysicalInputKind::KeyDown : ECortexEditorPhysicalInputKind::KeyUp;
	Event.Key = Key;
	Event.Modifiers = KeyEvent.GetModifierKeys();
	Event.bRepeat = KeyEvent.IsRepeat();
	HandleCapturedEvent(Event);
}

void FCortexEditorPhysicalInputSession::ObserveProcessorMouseButton(
	const FPointerEvent& MouseEvent, ECortexEditorPhysicalInputKind Kind)
{
	if (!IsSelectedUserAndDevice(MouseEvent.GetUserIndex(), MouseEvent.GetInputDeviceId()))
	{
		return;
	}
	if (DispatchContext.IsValid() && DispatchContext->SyntheticDepth > 0)
	{
		return;
	}
	const FKey Button = MouseEvent.GetEffectingButton();
	if (!Button.IsValid())
	{
		return;
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}

	const bool bDownEdge = (Kind == ECortexEditorPhysicalInputKind::PointerDown
		|| Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	const FVector2D ScreenSpacePosition = MouseEvent.GetScreenSpacePosition();
	const FVector2D ViewportPosition = ToViewportPosition(ScreenSpacePosition);
	if (GuardState.IsValid())
	{
		GuardState->LastViewportPointerPosition = ViewportPosition;
	}

	// Device-wide down state for admission, plus foreign ownership recorded BEFORE interruption:
	// a physical edge on a button this operation owns (or during unattended replay) is foreign, and
	// cleanup must never clear or release it. Foreign ownership ends only on the matching Up.
	if (bDownEdge)
	{
		CaptureState->ObservedHeldButtons.Add(Button);
		if (CaptureState->OwnedSyntheticButtons.Contains(Button) || bReplayInProgress)
		{
			CaptureState->ForeignInFlight.Add(Button);
			CaptureState->ForeignHeldButtons.Add(Button);
		}
	}
	else
	{
		CaptureState->ObservedHeldButtons.Remove(Button);
		CaptureState->ForeignInFlight.Remove(Button);
		CaptureState->ForeignHeldButtons.Remove(Button);
	}

	if (bReplayInProgress)
	{
		NotifyInterruption(MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Foreign physical input interrupted unattended playback")));
	}
	if (!CaptureState->bArmed)
	{
		return;
	}

	// A matching captured release survives focus loss; every other new edge must be on the
	// selected route.
	const bool bMatchingRelease = !bDownEdge && CaptureState->CapturedHeldButtons.Contains(Button);
	if (!bMatchingRelease && !IsPointerPositionOnSelectedRoute(ScreenSpacePosition))
	{
		return;
	}

	FCortexEditorPhysicalInputEvent Event;
	Event.Kind = Kind;
	Event.Key = Button;
	Event.Modifiers = MouseEvent.GetModifierKeys();
	Event.ViewportPosition = ViewportPosition;
	Event.Delta = MouseEvent.GetCursorDelta();
	HandleCapturedEvent(Event);
}

void FCortexEditorPhysicalInputSession::ObserveProcessorMouseMove(const FPointerEvent& MouseEvent)
{
	if (!IsSelectedUserAndDevice(MouseEvent.GetUserIndex(), MouseEvent.GetInputDeviceId()))
	{
		return;
	}
	if (DispatchContext.IsValid() && DispatchContext->SyntheticDepth > 0)
	{
		return;
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}

	const FVector2D ScreenSpacePosition = MouseEvent.GetScreenSpacePosition();
	const FVector2D ViewportPosition = ToViewportPosition(ScreenSpacePosition);
	if (GuardState.IsValid())
	{
		GuardState->LastViewportPointerPosition = ViewportPosition;
	}

	if (bReplayInProgress)
	{
		NotifyInterruption(MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Foreign physical input interrupted unattended playback")));
	}
	if (!CaptureState->bArmed)
	{
		return;
	}
	// Movement is attributed to the selected route either through the current hit path or through a
	// live capture that belongs to the selected route: Slate keeps routing a captured drag to the
	// selected consumer after the pointer leaves the viewport, and the replay stream must include
	// that consumer-visible change. Unrelated editor motion has neither.
	if (!IsPointerPositionOnSelectedRoute(ScreenSpacePosition)
		&& !IsPointerCaptureOnSelectedRoute())
	{
		return;
	}

	FCortexEditorPhysicalInputEvent Event;
	Event.Kind = (FSlateApplication::IsInitialized()
		&& FSlateApplication::Get().IsUsingHighPrecisionMouseMovment())
		? ECortexEditorPhysicalInputKind::RelativeMove : ECortexEditorPhysicalInputKind::PointerMove;
	// Canonical pointer-motion key: the portable 2D mouse axis. Absolute UI motion and relative
	// gameplay/camera motion both carry the 2D delta, so both record Mouse2D; replay never re-reads
	// this key for motion (the recorded delta drives it), but the recording must remain portable.
	Event.Key = EKeys::Mouse2D;
	Event.Modifiers = MouseEvent.GetModifierKeys();
	Event.ViewportPosition = ViewportPosition;
	Event.Delta = MouseEvent.GetCursorDelta();
	HandleCapturedEvent(Event);
}

void FCortexEditorPhysicalInputSession::ObserveProcessorMouseWheel(const FPointerEvent& MouseEvent)
{
	if (!IsSelectedUserAndDevice(MouseEvent.GetUserIndex(), MouseEvent.GetInputDeviceId()))
	{
		return;
	}
	if (DispatchContext.IsValid() && DispatchContext->SyntheticDepth > 0)
	{
		return;
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}
	const FVector2D ScreenSpacePosition = MouseEvent.GetScreenSpacePosition();
	if (bReplayInProgress)
	{
		NotifyInterruption(MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Foreign physical input interrupted unattended playback")));
	}
	if (!CaptureState->bArmed)
	{
		return;
	}
	if (!IsPointerPositionOnSelectedRoute(ScreenSpacePosition))
	{
		return;
	}

	FCortexEditorPhysicalInputEvent Event;
	Event.Kind = ECortexEditorPhysicalInputKind::Wheel;
	// Canonical wheel key: the portable mouse wheel axis (never the invalid key).
	Event.Key = EKeys::MouseWheelAxis;
	Event.Modifiers = MouseEvent.GetModifierKeys();
	Event.WheelDelta = MouseEvent.GetWheelDelta();
	Event.ViewportPosition = ToViewportPosition(ScreenSpacePosition);
	HandleCapturedEvent(Event);
}

void FCortexEditorPhysicalInputSession::HandleCapturedEvent(const FCortexEditorPhysicalInputEvent& Event)
{
	if (!CaptureState.IsValid() || !CaptureState->bArmed)
	{
		return;
	}

	// Repeat/up edges are recorded only after a fresh attributed Down in this capture epoch, and
	// a matching captured release survives focus leaving the viewport.
	bool bRecord = true;
	switch (Event.Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
		if (Event.bRepeat && !CaptureState->CapturedHeldKeys.Contains(Event.Key))
		{
			bRecord = false;
		}
		else
		{
			CaptureState->CapturedHeldKeys.Add(Event.Key);
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::DoubleClick:
		CaptureState->CapturedHeldButtons.Add(Event.Key);
		break;
	case ECortexEditorPhysicalInputKind::KeyUp:
		if (!CaptureState->CapturedHeldKeys.Contains(Event.Key))
		{
			bRecord = false;
		}
		else
		{
			CaptureState->CapturedHeldKeys.Remove(Event.Key);
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerUp:
		if (!CaptureState->CapturedHeldButtons.Contains(Event.Key))
		{
			bRecord = false;
		}
		else
		{
			CaptureState->CapturedHeldButtons.Remove(Event.Key);
		}
		break;
	default:
		break;
	}
	if (!bRecord)
	{
		return;
	}

	// Maintain the modifier transitions recorded in this epoch, and require the event's held
	// modifier bits to agree with them: unrecorded modifier influence makes the epoch incomplete
	// rather than silently injecting or masking a modifier.
	if (Event.Key.IsModifierKey()
		&& (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::KeyUp))
	{
		if (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown)
		{
			CaptureState->CapturedModifierKeys.Add(Event.Key);
		}
		else
		{
			CaptureState->CapturedModifierKeys.Remove(Event.Key);
		}
	}

	const FModifierKeysState& Modifiers = Event.Modifiers;
	const bool bCapturedShift = CaptureState->CapturedModifierKeys.Contains(EKeys::LeftShift)
		|| CaptureState->CapturedModifierKeys.Contains(EKeys::RightShift);
	const bool bCapturedControl = CaptureState->CapturedModifierKeys.Contains(EKeys::LeftControl)
		|| CaptureState->CapturedModifierKeys.Contains(EKeys::RightControl);
	const bool bCapturedAlt = CaptureState->CapturedModifierKeys.Contains(EKeys::LeftAlt)
		|| CaptureState->CapturedModifierKeys.Contains(EKeys::RightAlt);
	if (Modifiers.IsShiftDown() != bCapturedShift
		|| Modifiers.IsControlDown() != bCapturedControl
		|| Modifiers.IsAltDown() != bCapturedAlt)
	{
		CaptureState->bInconsistentModifiers = true;
		SignalCaptureFault(TEXT("Captured modifier state disagreed with the recorded modifier transitions"));
	}

	FCortexEditorPhysicalInputCaptureContext Context;
	FCortexCommandResult ContextError;
	if (!BuildCaptureContext(Event, Context, ContextError))
	{
		// A required pre-press observation failed. This is a capture fault, never a downgraded or
		// invented guard; the fault is only signalled here so teardown stays outside this stack.
		SignalCaptureFault(ContextError.ErrorMessage);
		return;
	}
	if (CaptureCallbackImpl)
	{
		CaptureCallbackImpl(Event, FPlatformTime::Seconds(), Context);
	}
}

bool FCortexEditorPhysicalInputSession::BuildCaptureContext(const FCortexEditorPhysicalInputEvent& Event,
	FCortexEditorPhysicalInputCaptureContext& OutContext, FCortexCommandResult& OutError) const
{
	OutContext = FCortexEditorPhysicalInputCaptureContext();
	UWorld* World = Binding.World.Get();
	OutContext.FrameNumber = GFrameCounter;
	OutContext.WorldTimeSeconds = World ? World->GetTimeSeconds() : 0.0;
	OutContext.bWorldPaused = World ? World->IsPaused() : false;
	OutContext.bTargetOwnsPointerCapture = CaptureState.IsValid()
		&& (CaptureState->CapturedHeldButtons.Num() > 0 || CaptureState->OwnedSyntheticButtons.Num() > 0);

	FCortexEditorPhysicalInputPlayerPose Pose;
	const FCortexCommandResult PoseResult = ReadPlayerPose(Pose);
	if (PoseResult.bSuccess)
	{
		OutContext.PressPose = Pose;
	}

	// Every non-repeat press requires the genuinely observed pre-press pose that its durable
	// guard is built from. A failed read is a required-observation fault, never a substitute.
	const bool bRequiredPosePress = !Event.bRepeat
		&& (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::PointerDown
			|| Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	if (bRequiredPosePress && !OutContext.PressPose.IsSet())
	{
		OutError = PoseResult.bSuccess
			? MakeErrorResult(CortexErrorCodes::InvalidValue,
				TEXT("Required pre-press player pose was not observed"))
			: PoseResult;
		return false;
	}

	// Non-repeat keyboard presses are pose-only: only pointer/double-click boundaries carry UI.
	const bool bGuardedPress = (Event.Kind == ECortexEditorPhysicalInputKind::PointerDown
		|| Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick);
	if (!bGuardedPress)
	{
		OutContext.UICoverage = ECortexEditorUICoverage::NotApplicable;
		OutContext.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
		return true;
	}

	FCortexEditorPhysicalInputWidgetIdentity Identity;
	FVector2D Normalized = FVector2D::ZeroVector;
	// Capture claims Supported only for a trustworthy selector digest; a resolver false (ambiguous
	// route, systemic identity fault) or an invalid digest is Unavailable, never a downgraded guard.
	if (ResolveTaggedSlateIdentity(Event.ViewportPosition, Identity, Normalized)
		&& FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(Identity.IdentitySha256))
	{
		OutContext.UICoverage = ECortexEditorUICoverage::Supported;
		OutContext.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
		OutContext.UITarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(MoveTemp(Identity));
		OutContext.UILocalPosition = Normalized;
	}
	else
	{
		OutContext.UICoverage = ECortexEditorUICoverage::Unavailable;
		OutContext.UIUnavailableReason = ECortexEditorUIUnavailableReason::UnobservablePointerRoute;
	}
	return true;
}

bool FCortexEditorPhysicalInputSession::ResolveTaggedSlateIdentity(const FVector2D& ViewportPosition,
	FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal) const
{
	if (!GuardState.IsValid())
	{
		return false;
	}
	// Weak live resolution only: the tagged-runtime Slate route, never UMG/CommonUI/world.
	GuardState->ViewportWidget = GetCoordinateRootWidget();
	GuardState->SlateUserIndex = Binding.SlateUserIndex;
	// Capture maps every unresolved outcome (including a systemic identity fault) to Unavailable,
	// so the fault flag is only distinguished by the live observation path.
	bool bIdentityFault = false;
	return FCortexEditorPhysicalInputUIResolver::ResolveActualSlateTarget(
		ViewportPosition, *GuardState, OutIdentity, OutNormalizedLocal, bIdentityFault);
}

void FCortexEditorPhysicalInputSession::NotifyInterruption(const FCortexCommandResult& Result)
{
	if (!CaptureState.IsValid() || CaptureState->bInterrupted)
	{
		return;
	}
	CaptureState->bInterrupted = true;
	if (InterruptionCallbackImpl)
	{
		InterruptionCallbackImpl(Result);
	}
}

void FCortexEditorPhysicalInputSession::SignalCaptureFault(const FString& Message)
{
	if (!CaptureState.IsValid() || CaptureState->bFaulted)
	{
		return;
	}
	// Signal through the error/interruption channel only; teardown is never performed here and the
	// human event remains non-consuming because the processor always returns false.
	CaptureState->bFaulted = true;
	UE_LOG(LogCortexEditor, Display, TEXT("Physical capture fault signalled: %s"), *Message);
	if (InterruptionCallbackImpl)
	{
		InterruptionCallbackImpl(MakeErrorResult(CortexErrorCodes::InvalidOperation, Message));
	}
}

bool FCortexEditorPhysicalInputSession::IsPointerPositionOnSelectedRoute(
	const FVector2D& ScreenSpacePosition) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWidget> CoordinateRoot = GetCoordinateRootWidget();
	const TSharedPtr<SWidget> InputRoot = Binding.InputRoot.Pin();
	const TSharedPtr<SWidget> Anchor = CoordinateRoot.IsValid() ? CoordinateRoot : InputRoot;
	FWidgetPath Path;
	if (!ResolveSelectedRoutePath(FSlateApplication::Get(), Binding.SlateUserIndex, Anchor,
		ScreenSpacePosition, Path))
	{
		return false;
	}
	for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
	{
		const SWidget* Widget = Path.Widgets[Index].GetWidgetPtr();
		if ((CoordinateRoot.IsValid() && Widget == CoordinateRoot.Get())
			|| (InputRoot.IsValid() && Widget == InputRoot.Get()))
		{
			return true;
		}
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::IsWidgetOnSelectedRoute(
	const TSharedPtr<const SWidget>& Widget) const
{
	const TSharedPtr<SWidget> CoordinateRoot = GetCoordinateRootWidget();
	const TSharedPtr<SWidget> InputRoot = Binding.InputRoot.Pin();
	for (TSharedPtr<const SWidget> Current = Widget; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if ((CoordinateRoot.IsValid() && Current.Get() == CoordinateRoot.Get())
			|| (InputRoot.IsValid() && Current.Get() == InputRoot.Get()))
		{
			return true;
		}
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::IsPointerCaptureOnSelectedRoute() const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(Binding.SlateUserIndex);
	if (!User.IsValid() || !User->HasAnyCapture())
	{
		return false;
	}
	// Slate routes movement to a live captor even when the pointer left its hit area, so a captor
	// that belongs to the selected route still drives a selected consumer; a captor outside the
	// route (ordinary editor input) is never admitted.
	for (const TSharedRef<SWidget>& Captor : User->GetCaptorWidgets())
	{
		if (IsWidgetOnSelectedRoute(Captor))
		{
			return true;
		}
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::IsKeyboardFocusOnSelectedRoute() const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(
		static_cast<uint32>(Binding.SlateUserIndex));
	return IsWidgetOnSelectedRoute(Focused);
}

bool FCortexEditorPhysicalInputSession::IsSelectedRouteOwnershipIntact(FString& OutReason) const
{
	if (!FSlateApplication::IsInitialized())
	{
		OutReason = TEXT("Slate is not initialized");
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	if (!Slate.GetUser(Binding.SlateUserIndex).IsValid())
	{
		OutReason = TEXT("The selected Slate user is no longer available");
		return false;
	}

	// Actual activation, not membership in a retained set: the selected route must be the
	// actually-active top-level window of an active application. An unresolved route window or an
	// unresolved active top-level window is route loss, never intact ownership, and is never a
	// reason to force inactive input.
	const TSharedPtr<SWidget> CoordinateRoot = GetCoordinateRootWidget();
	const TSharedPtr<SWindow> RouteWindow = CoordinateRoot.IsValid()
		? Slate.FindWidgetWindow(CoordinateRoot.ToSharedRef()) : nullptr;
	if (!Slate.IsActive())
	{
		OutReason = TEXT("Replay target route is not the active application");
		return false;
	}
	if (!RouteWindow.IsValid())
	{
		UE_LOG(LogCortexEditor, Display,
			TEXT("Replay route loss: route window unresolvable (appActive=%d activeTopLevelValid=%d)"),
			Slate.IsActive() ? 1 : 0, Slate.GetActiveTopLevelWindow().IsValid() ? 1 : 0);
		OutReason = TEXT("Replay target route window is no longer resolvable");
		return false;
	}
	const TSharedPtr<SWindow> ActiveTopLevel = Slate.GetActiveTopLevelWindow();
	if (!ActiveTopLevel.IsValid() || ActiveTopLevel != RouteWindow)
	{
		UE_LOG(LogCortexEditor, Display,
			TEXT("Replay route loss: route window is not the active top-level window (appActive=%d activeTopLevelValid=%d sameWindow=%d)"),
			Slate.IsActive() ? 1 : 0, ActiveTopLevel.IsValid() ? 1 : 0,
			(ActiveTopLevel.IsValid() && ActiveTopLevel == RouteWindow) ? 1 : 0);
		OutReason = TEXT("Replay target route window is not the active top-level window");
		return false;
	}

	// A foreign keyboard focus (an editor control outside the selected route) would receive every
	// synthetic key. Focus that has never been established is not claimed as a foreign owner.
	const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(
		static_cast<uint32>(Binding.SlateUserIndex));
	if (Focused.IsValid() && !IsWidgetOnSelectedRoute(Focused))
	{
		OutReason = TEXT("Replay lost the selected route to a foreign keyboard focus");
		return false;
	}

	return true;
}

FCortexCommandResult FCortexEditorPhysicalInputSession::SetCaptureCallback(
	TFunction<void(const FCortexEditorPhysicalInputEvent&, double,
		const FCortexEditorPhysicalInputCaptureContext&)>&& Callback)
{
	return ArmCapture(MoveTemp(Callback));
}

void FCortexEditorPhysicalInputSession::SetInterruptionCallback(
	TFunction<void(const FCortexCommandResult&)>&& Callback)
{
	InterruptionCallbackImpl = MoveTemp(Callback);
}

FCortexCommandResult FCortexEditorPhysicalInputSession::ObserveUI(
	const FCortexEditorPhysicalInputEvent& Press,
	const FCortexEditorPhysicalInputWidgetIdentity& Expected,
	FCortexEditorPhysicalInputUIObservation& Out) const
{
	FCortexCommandResult Error;
	if (!ValidateTarget(Error))
	{
		Out = FCortexEditorPhysicalInputUIObservation();
		Out.State = ECortexEditorUIObservationState::Unavailable;
		return Error;
	}
	if (!GuardState.IsValid())
	{
		Out = FCortexEditorPhysicalInputUIObservation();
		Out.State = ECortexEditorUIObservationState::Unavailable;
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::UIObservationFailed,
			TEXT("No UI observation state is available"));
	}

	GuardState->ViewportWidget = GetCoordinateRootWidget();
	GuardState->SlateUserIndex = Binding.SlateUserIndex;
	const ECortexEditorUIObservationState State = FCortexEditorPhysicalInputUIResolver::ResolveSlateObservation(
		Press, Expected, *GuardState, Out);
	if (State == ECortexEditorUIObservationState::Ready
		|| State == ECortexEditorUIObservationState::PointerPending
		|| State == ECortexEditorUIObservationState::LayoutPending)
	{
		return MakeSuccessResult();
	}
	return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::UIObservationFailed,
		TEXT("The expected UI target is not the current normal-route target"));
}

bool FCortexEditorPhysicalInputSession::CanWaitForUI() const
{
	FCortexCommandResult Error;
	if (!ValidateTarget(Error))
	{
		return false;
	}
	if (!CaptureState.IsValid())
	{
		return true;
	}
	// Eligibility comes from live owned held/capture/drag state only, never from whether replay has
	// ever dispatched: a balanced KeyDown/KeyUp (for example a menu open/close) leaves the session
	// able to wait and to dispatch again.
	return !CaptureState->bOwnedPointerCapture
		&& !CaptureState->bOwnedDragDrop
		&& CaptureState->OwnedSyntheticKeys.Num() == 0
		&& CaptureState->OwnedSyntheticButtons.Num() == 0
		&& CaptureState->CapturedHeldKeys.Num() == 0
		&& CaptureState->CapturedHeldButtons.Num() == 0;
}

FCortexCommandResult FCortexEditorPhysicalInputSession::BeginReplayEpoch()
{
	if (bDispatchFrozen)
	{
		return MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Physical input dispatch is frozen while the original target is being cleaned up"));
	}
	FCortexCommandResult Error;
	if (!ValidateTarget(Error))
	{
		return Error;
	}
	if (!FSlateApplication::IsInitialized())
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady, TEXT("Slate is not initialized"));
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}
	if (!DispatchContext.IsValid())
	{
		DispatchContext = MakeShared<FCortexEditorPhysicalInputDispatchContext>();
	}
	if (!GuardState.IsValid())
	{
		GuardState = MakeShared<FCortexEditorPhysicalInputGuardState>();
	}

	// Interference ownership is armed here, at epoch establishment, not at the first Dispatch, so
	// foreign focus/input during the pre-first-event window is detected exactly like interference
	// after dispatch. Focus is never stolen and inactive input is never forced.
	bReplayInProgress = true;
	return MakeSuccessResult();
}

FCortexCommandResult FCortexEditorPhysicalInputSession::Dispatch(const FCortexEditorPhysicalInputEvent& Event)
{
	if (bDispatchFrozen)
	{
		return MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("Physical input dispatch is frozen while the original target is being cleaned up"));
	}
	FCortexCommandResult Error;
	if (!ValidateTarget(Error))
	{
		return Error;
	}
	if (!FSlateApplication::IsInitialized())
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady, TEXT("Slate is not initialized"));
	}
	if (!CaptureState.IsValid())
	{
		CaptureState = MakeShared<FCortexEditorPhysicalInputCaptureState>();
	}
	if (!DispatchContext.IsValid())
	{
		DispatchContext = MakeShared<FCortexEditorPhysicalInputDispatchContext>();
	}
	if (!GuardState.IsValid())
	{
		GuardState = MakeShared<FCortexEditorPhysicalInputGuardState>();
	}

	// When replay ownership is armed, the selected focus/window/route must still own input before
	// any synthetic event is delivered. A foreign focus/window is interference: never deliver the
	// event through global Slate routing to a foreign consumer. No focus is stolen.
	if (bReplayInProgress)
	{
		FString OwnershipReason;
		if (!IsSelectedRouteOwnershipIntact(OwnershipReason))
		{
			const FCortexCommandResult Lost = MakeErrorResult(CortexErrorCodes::InvalidOperation, OwnershipReason);
			NotifyInterruption(Lost);
			return Lost;
		}
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const FInputDeviceId Device = Binding.InputDevice;
	const int32 UserIndex = Binding.SlateUserIndex;
	// Inactive-application input handling is enabled only for capture/setup dispatch, where the
	// editor window may not hold OS focus; it is never enabled during replay. Replay requires the
	// selected route to be the actually-active route (checked above), and forcing input while the
	// application is inactive would deliver into a route the host is not actually driving.
	TUniquePtr<FCortexScopedInactivePhysicalInput> InactiveInputGuard;
	if (!bReplayInProgress && !Slate.IsActive())
	{
		InactiveInputGuard = MakeUnique<FCortexScopedInactivePhysicalInput>(Slate);
	}
	// Snapshot the pre-dispatch state so only capture/high-precision/drag state this operation's own
	// event establishes is ever attributed to it, and a foreign captor or drag is never released.
	const bool bHadUserCaptureBefore = Slate.HasUserMouseCapture(UserIndex);
	const bool bHadHighPrecisionBefore = Slate.IsUsingHighPrecisionMouseMovment();
	const TSharedPtr<GenericApplication> PlatformApplicationBefore = Slate.GetPlatformApplication();
	const void* const NativeCaptureBefore = PlatformApplicationBefore.IsValid()
		? PlatformApplicationBefore->GetCapture() : nullptr;
	const TSharedPtr<FSlateUser> SelectedPreUser = Slate.GetUser(UserIndex);
	const TSharedPtr<FDragDropOperation> DragDropBefore = SelectedPreUser.IsValid()
		? SelectedPreUser->GetDragDropContent() : nullptr;
	TGuardValue<int32> SyntheticGuard(
		DispatchContext->SyntheticDepth, DispatchContext->SyntheticDepth + 1);

	// Stored portable coordinates are viewport-local; every engine event needs screen space.
	FCortexEditorPhysicalInputEvent EngineEvent = Event;
	EngineEvent.ViewportPosition = ToScreenSpacePosition(Event.ViewportPosition);

	switch (Event.Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
	case ECortexEditorPhysicalInputKind::KeyUp:
	{
		FKeyEvent KeyEvent;
		if (!FCortexEditorPhysicalInputEventBuilder::BuildKeyEvent(EngineEvent, Device, UserIndex, KeyEvent))
		{
			return MakeErrorResult(CortexErrorCodes::InvalidOperation,
				TEXT("The captured key event is not a valid key"));
		}
		if (Event.Kind == ECortexEditorPhysicalInputKind::KeyDown)
		{
			Slate.ProcessKeyDownEvent(KeyEvent);
			CaptureState->OwnedSyntheticKeys.Add(Event.Key);
		}
		else
		{
			Slate.ProcessKeyUpEvent(KeyEvent);
			CaptureState->OwnedSyntheticKeys.Remove(Event.Key);
		}
		break;
	}
	case ECortexEditorPhysicalInputKind::PointerMove:
	case ECortexEditorPhysicalInputKind::RelativeMove:
	{
		const FVector2D LastViewportPosition = GuardState->LastViewportPointerPosition;
		FVector2D ScreenSpacePosition;
		if (Event.Kind == ECortexEditorPhysicalInputKind::RelativeMove)
		{
			// Relative gameplay motion advances by the recorded delta in screen space and never
			// becomes an absolute UI coordinate.
			EngineEvent.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			ScreenSpacePosition = ToScreenSpacePosition(LastViewportPosition) + Event.Delta;
		}
		else
		{
			ScreenSpacePosition = ToScreenSpacePosition(Event.ViewportPosition);
		}
		EngineEvent.ViewportPosition = ScreenSpacePosition;
		FPointerEvent PointerEvent;
		if (!FCortexEditorPhysicalInputEventBuilder::BuildPointerMoveEvent(EngineEvent, Device, UserIndex,
			CaptureState->OwnedSyntheticButtons, ScreenSpacePosition, PointerEvent))
		{
			return MakeErrorResult(CortexErrorCodes::InvalidOperation,
				TEXT("The captured pointer motion is not dispatchable"));
		}
		// Normal routing with drag detection: false must not be changed to true, which would skip
		// preprocessors and Slate drag detection.
		Slate.ProcessMouseMoveEvent(PointerEvent, false);
		GuardState->LastViewportPointerPosition = Event.Kind == ECortexEditorPhysicalInputKind::RelativeMove
			? ToViewportPosition(ScreenSpacePosition) : Event.ViewportPosition;
		++DispatchContext->ProcessedMotionGeneration;
		GuardState->MotionGeneration = DispatchContext->ProcessedMotionGeneration;
		RetainOwnedPointerState(Slate, PointerEvent, bHadUserCaptureBefore, bHadHighPrecisionBefore,
			NativeCaptureBefore, DragDropBefore);
		break;
	}
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::PointerUp:
	case ECortexEditorPhysicalInputKind::DoubleClick:
	{
		TSet<FKey> PressedButtons = CaptureState->OwnedSyntheticButtons;
		const bool bDownEdge = (Event.Kind != ECortexEditorPhysicalInputKind::PointerUp);
		if (bDownEdge)
		{
			PressedButtons.Add(Event.Key);
		}
		else
		{
			PressedButtons.Remove(Event.Key);
		}
		FPointerEvent PointerEvent;
		if (!FCortexEditorPhysicalInputEventBuilder::BuildPointerButtonEvent(EngineEvent, Device, UserIndex,
			PressedButtons, PointerEvent))
		{
			return MakeErrorResult(CortexErrorCodes::InvalidOperation,
				TEXT("The captured pointer button is not a valid key"));
		}
		if (Event.Kind == ECortexEditorPhysicalInputKind::PointerDown)
		{
			Slate.ProcessMouseButtonDownEvent(nullptr, PointerEvent);
			CaptureState->OwnedSyntheticButtons.Add(Event.Key);
		}
		else if (Event.Kind == ECortexEditorPhysicalInputKind::DoubleClick)
		{
			Slate.ProcessMouseButtonDoubleClickEvent(nullptr, PointerEvent);
			CaptureState->OwnedSyntheticButtons.Add(Event.Key);
		}
		else
		{
			Slate.ProcessMouseButtonUpEvent(PointerEvent);
			CaptureState->OwnedSyntheticButtons.Remove(Event.Key);
		}
		GuardState->LastViewportPointerPosition = Event.ViewportPosition;

		// Retain the exact captor/drag/high-precision/native-capture state this operation acquired
		// through its own dispatch and reconcile anything the normal routing already released, so
		// cleanup only ever cancels live, genuinely owned state.
		RetainOwnedPointerState(Slate, PointerEvent, bHadUserCaptureBefore, bHadHighPrecisionBefore,
			NativeCaptureBefore, DragDropBefore);
		break;
	}
	case ECortexEditorPhysicalInputKind::Wheel:
	{
		FPointerEvent PointerEvent;
		if (!FCortexEditorPhysicalInputEventBuilder::BuildWheelEvent(EngineEvent, Device, UserIndex,
			CaptureState->OwnedSyntheticButtons, PointerEvent))
		{
			return MakeErrorResult(CortexErrorCodes::InvalidOperation,
				TEXT("The captured wheel event is not dispatchable"));
		}
		Slate.ProcessMouseWheelOrGestureEvent(PointerEvent, nullptr);
		break;
	}
	default:
		return MakeErrorResult(CortexErrorCodes::InvalidOperation,
			TEXT("The captured event kind is not dispatchable"));
	}

	++DispatchContext->OperationGeneration;
	return MakeSuccessResult();
}

void FCortexEditorPhysicalInputSession::RetainOwnedPointerState(FSlateApplication& Slate,
	const FPointerEvent& PointerEvent, bool bHadUserCaptureBefore, bool bHadHighPrecisionBefore,
	const void* NativeCaptureBefore, const TSharedPtr<FDragDropOperation>& DragDropBefore)
{
	if (!CaptureState.IsValid())
	{
		return;
	}
	const int32 UserIndex = Binding.SlateUserIndex;

	// Capture: retain only the exact captor this operation's own event acquired, so a foreign
	// captor is never recorded and never released. Reconcile afterwards so a matching release (for
	// example the slider's own OnMouseButtonUp) does not retain acquisition history until terminal
	// cleanup and wrongly block a UI wait.
	if (Slate.HasUserMouseCapture(UserIndex))
	{
		if (!bHadUserCaptureBefore && !CaptureState->bOwnedPointerCapture)
		{
			FWidgetPath CaptorPath;
			if (ResolveSelectedRoutePath(Slate, UserIndex, GetCoordinateRootWidget(),
				PointerEvent.GetScreenSpacePosition(), CaptorPath))
			{
				CaptureState->OwnedCaptorPath = MakeShared<FWidgetPath>(CaptorPath);
				CaptureState->OwnedPointerEvent = PointerEvent;
				CaptureState->bOwnedPointerCapture = true;
			}
		}
	}
	else if (CaptureState->bOwnedPointerCapture)
	{
		CaptureState->bOwnedPointerCapture = false;
		CaptureState->OwnedCaptorPath.Reset();
		CaptureState->OwnedPointerEvent = FPointerEvent();
	}

	// High-precision raw mouse movement is established together with the capture and, per the engine
	// contract, released by the matching release reply. Track any mode that turns on during this
	// operation's own dispatch as owned (it can only have been requested by this dispatch's reply,
	// FReply::UseHighPrecisionMouseMovement sets MouseCaptor and bUseHighPrecisionMouse together,
	// Reply.h:36-43) and reconcile it when the mode turns off.
	if (Slate.IsUsingHighPrecisionMouseMovment())
	{
		if (!bHadHighPrecisionBefore && !CaptureState->bOwnedHighPrecision)
		{
			CaptureState->bOwnedHighPrecision = true;
		}
	}
	else if (CaptureState->bOwnedHighPrecision)
	{
		CaptureState->bOwnedHighPrecision = false;
	}

	// Native (OS) capture: Slate's ProcessReply establishes it together with a high-precision reply
	// (PlatformApplication->SetCapture(Window->GetNativeWindow()), SlateApplication.cpp:3592-3596),
	// but when a drag starts Slate releases only the Slate captor (bStartingDragDrop at :3398-3400)
	// and leaves OS capture in place. Retain native ownership independently of the live Slate captor
	// so cleanup still releases and verifies it. Reconcile once the captured handle is no longer
	// current (our own capture released, or replaced by a foreign window that is never ours).
	if (const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication())
	{
		const void* const NativeCaptureAfter = PlatformApplication->GetCapture();
		if (NativeCaptureAfter != nullptr && NativeCaptureAfter != NativeCaptureBefore
			&& !CaptureState->bOwnedNativeCapture)
		{
			CaptureState->bOwnedNativeCapture = true;
			CaptureState->OwnedNativeCaptureHandle = NativeCaptureAfter;
		}
		else if (CaptureState->bOwnedNativeCapture
			&& NativeCaptureAfter != CaptureState->OwnedNativeCaptureHandle)
		{
			CaptureState->bOwnedNativeCapture = false;
			CaptureState->OwnedNativeCaptureHandle = nullptr;
		}
	}

	// Drag-drop: Slate releases mouse capture when a drag starts (BeginDragDrop) and keeps the drag
	// content in a separate slot, so the captor path cannot identify it. Retain the exact operation
	// this dispatch established, and reconcile it once the owned drop/cancel has ended it.
	const TSharedPtr<FSlateUser> SelectedUser = Slate.GetUser(UserIndex);
	const TSharedPtr<FDragDropOperation> DragDropAfter = SelectedUser.IsValid()
		? SelectedUser->GetDragDropContent() : nullptr;
	if (!CaptureState->bOwnedDragDrop && DragDropAfter.IsValid() && DragDropAfter != DragDropBefore)
	{
		CaptureState->bOwnedDragDrop = true;
		CaptureState->OwnedDragDropContent = DragDropAfter;
	}
	else if (CaptureState->bOwnedDragDrop
		&& DragDropAfter != CaptureState->OwnedDragDropContent.Pin())
	{
		CaptureState->bOwnedDragDrop = false;
		CaptureState->OwnedDragDropContent.Reset();
	}
}

FCortexCommandResult FCortexEditorPhysicalInputSession::ReleaseHeldInputs()
{
	// Step 1: freeze dispatch once and detach the capture callbacks.
	bDispatchFrozen = true;
	bReplayInProgress = false;
	CaptureCallbackImpl = nullptr;
	if (CaptureState.IsValid())
	{
		CaptureState->bArmed = false;
	}
	if (DispatchContext.IsValid())
	{
		DispatchContext->SyntheticDepth = 0;
	}

	// Validate the saved original identities without requiring current focus.
	APlayerController* Controller = Binding.Controller.Get();
	APawn* Pawn = Binding.Pawn.Get();
	UWorld* World = Binding.World.Get();
	if (!bBound || Controller == nullptr || Pawn == nullptr || World == nullptr)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::CleanupFailed,
			TEXT("The original physical input target is no longer available for cleanup"));
	}
	// The exact PlayerInput retained when this binding acquired ownership is the only instance this
	// operation may release or neutralize; a replacement is never touched.
	UPlayerInput* const OriginalPlayerInput = BoundPlayerInput.Get();
	if (OriginalPlayerInput == nullptr || Controller->PlayerInput != OriginalPlayerInput)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::CleanupFailed,
			TEXT("The original PlayerInput is no longer the bound controller's PlayerInput"));
	}

	TSet<FKey> OwnedKeys;
	TSet<FKey> OwnedButtons;
	TSet<FKey> ForeignHeld;
	TSet<FKey> ForeignHeldKeys;
	if (CaptureState.IsValid())
	{
		OwnedKeys = CaptureState->OwnedSyntheticKeys;
		OwnedButtons = CaptureState->OwnedSyntheticButtons;
		ForeignHeld = CaptureState->ForeignHeldButtons;
		ForeignHeldKeys = CaptureState->ForeignHeldKeys;
		for (const FKey& InFlight : CaptureState->ForeignInFlight)
		{
			if (InFlight.IsMouseButton())
			{
				ForeignHeld.Add(InFlight);
			}
			else
			{
				ForeignHeldKeys.Add(InFlight);
			}
		}
	}

	// Owned digital keys and mouse buttons join the guarded controller release, excluding any key or
	// button a foreign source holds (or is currently routing): a human press on a replay-owned key
	// must never receive a synthetic release or per-key neutralization.
	TSet<FKey> ReleaseKeys;
	for (const FKey& Key : OwnedKeys)
	{
		if (!ForeignHeldKeys.Contains(Key))
		{
			ReleaseKeys.Add(Key);
		}
	}
	for (const FKey& Button : OwnedButtons)
	{
		if (!ForeignHeld.Contains(Button))
		{
			ReleaseKeys.Add(Button);
		}
	}

	// Step 2: release owned digital keys/buttons directly to the still-original controller and apply
	// only these owned keys' neutralization fields. The PlayerInput identity is revalidated before
	// and after every virtual call.
	bool bPlayerInputReplaced = false;
	for (const FKey& Key : ReleaseKeys)
	{
		if (Controller->PlayerInput != OriginalPlayerInput)
		{
			bPlayerInputReplaced = true;
			break;
		}
		Controller->InputKey(FInputKeyEventArgs(nullptr, Binding.InputDevice, Key, IE_Released,
			0.0f, false, FPlatformTime::Cycles64()));
		if (Controller->PlayerInput != OriginalPlayerInput)
		{
			bPlayerInputReplaced = true;
			break;
		}
	}
	if (bPlayerInputReplaced)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::CleanupFailed,
			TEXT("The original PlayerInput was replaced during cleanup"));
	}
	for (const FKey& Key : ReleaseKeys)
	{
		if (FKeyState* State = OriginalPlayerInput->GetKeyState(Key))
		{
			State->RawValue = FVector::ZeroVector;
			State->bDown = false;
			State->bDownPrevious = false;
			State->LastUpDownTransitionTime = static_cast<float>(World->GetRealTimeSeconds());
			State->bWasJustFlushed = true;
		}
	}

	FSlateApplication& Slate = FSlateApplication::Get();

	// Step 3: clear focus only when the exact synthetic keyboard-focus owner still holds it.
	if (CaptureState.IsValid() && CaptureState->SyntheticFocusOwner.IsValid())
	{
		const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(
			static_cast<uint32>(Binding.SlateUserIndex));
		if (Focused.IsValid() && Focused == CaptureState->SyntheticFocusOwner.Pin())
		{
			Slate.ClearUserFocus(static_cast<uint32>(Binding.SlateUserIndex), EFocusCause::SetDirectly);
		}
	}

	// A foreign physical button held (or in flight) means the pointer is owned elsewhere: never
	// release any capture then, and never release a capture this operation does not own.
	const bool bForeignCapturePresent = ForeignHeld.Num() > 0;

	// Step 3.5: cancel this operation's own Slate drag-drop, pointer capture and cursor lock. A
	// cancelled slider keeps responding to physical movement while its capture persists, and a
	// drag-drop established by BeginDragDrop survives a capture-only check (the engine releases
	// capture when the drag starts), so both are cancelled under their exact owned identities.
	const TSharedPtr<FSlateUser> SelectedUser = Slate.GetUser(Binding.SlateUserIndex);
	if (CaptureState.IsValid() && !bForeignCapturePresent)
	{
		// Cancel only the exact drag-drop operation this operation started.
		if (CaptureState->bOwnedDragDrop && SelectedUser.IsValid())
		{
			const TSharedPtr<FDragDropOperation> OwnedDrag = CaptureState->OwnedDragDropContent.Pin();
			if (OwnedDrag.IsValid() && SelectedUser->GetDragDropContent() == OwnedDrag)
			{
				SelectedUser->CancelDragDrop();
			}
		}
		// Normal focus/capture cancellation through the public reply path, not a direct widget
		// callback. The normal release branches also disable high-precision mode and cursor lock.
		if (CaptureState->bOwnedPointerCapture
			&& Slate.HasUserMouseCapture(Binding.SlateUserIndex))
		{
			if (CaptureState->OwnedCaptorPath.IsValid())
			{
				const FReply ReleaseReply = FReply::Handled().ReleaseMouseCapture().ReleaseMouseLock();
				Slate.ProcessReply(*CaptureState->OwnedCaptorPath, ReleaseReply, nullptr,
					&CaptureState->OwnedPointerEvent, static_cast<uint32>(Binding.SlateUserIndex));
			}
			// Fallback for an owned capture whose recorded path is no longer resolvable: release the
			// exact cursor-pointer capture this operation established through the public per-user API
			// (FSlateUser::ReleaseCapture / ReleaseCursorCapture are public in 5.8, SlateUser.h:74-76).
			if (Slate.HasUserMouseCapture(Binding.SlateUserIndex) && SelectedUser.IsValid())
			{
				SelectedUser->ReleaseCapture(CaptureState->OwnedPointerEvent.GetPointerIndex());
			}
		}
	}

	// Step 3.6: an engine-side captor release does not necessarily deactivate high-precision mouse
	// mode. ProcessReply releases the captor when a drag starts but only the matching release reply
	// disables high precision (SlateApplication.cpp:3398-3400 vs :3625-3635), so a drag started from
	// an owned high-precision press can leave the mode on with the captor already gone. Release this
	// operation's own high-precision mode explicitly through the public platform API so the success
	// contract is genuinely verifiable. A foreign/pre-existing mode was never claimed as owned.
	if (CaptureState.IsValid() && CaptureState->bOwnedHighPrecision && !bForeignCapturePresent
		&& Slate.IsUsingHighPrecisionMouseMovment())
	{
		if (const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication())
		{
			const TSharedPtr<SWidget> CoordinateRoot = GetCoordinateRootWidget();
			const TSharedPtr<SWindow> Window = CoordinateRoot.IsValid()
				? Slate.FindWidgetWindow(CoordinateRoot.ToSharedRef()) : nullptr;
			PlatformApplication->SetHighPrecisionMouseMode(false,
				Window.IsValid() ? Window->GetNativeWindow() : nullptr);
		}
	}

	// Step 4: clear this operation's cached mouse-button bits without foreign routing.
	for (const FKey& Button : OwnedButtons)
	{
		if (CaptureState.IsValid()
			&& (CaptureState->ForeignHeldButtons.Contains(Button)
				|| CaptureState->ForeignInFlight.Contains(Button)))
		{
			continue; // Held by a foreign source or its in-flight Down: never generate an Up.
		}
		if (!Slate.GetPressedMouseButtons().Contains(Button))
		{
			continue;
		}
		TSet<FKey> RemainingButtons = Slate.GetPressedMouseButtons();
		RemainingButtons.Remove(Button);
		const TSharedRef<FCortexPhysicalInputCleanupShield> Shield =
			MakeShared<FCortexPhysicalInputCleanupShield>();
		Shield->ExpectedButton = Button;
		Shield->ExpectedDevice = Binding.InputDevice;
		Slate.RegisterInputPreProcessor(Shield, EInputPreProcessorType::PreGame);
		const FVector2D LastViewportPosition = GuardState.IsValid()
			? GuardState->LastViewportPointerPosition : FVector2D::ZeroVector;
		const FVector2D ScreenSpace = ToScreenSpacePosition(LastViewportPosition);
		const FPointerEvent UpEvent(Binding.InputDevice, FSlateApplicationBase::CursorPointerIndex,
			ScreenSpace, ScreenSpace, RemainingButtons, Button, 0.0f, FModifierKeysState(),
			TOptional<int32>(Binding.SlateUserIndex));
		Slate.ProcessMouseButtonUpEvent(UpEvent);
		Slate.UnregisterInputPreProcessor(Shield);
	}

	// Step 5: clear residual native (OS) capture only when this operation genuinely owned it. Native
	// ownership is tracked independently of the live Slate captor because Slate releases only the
	// Slate captor when a drag starts, leaving OS capture in place. A foreign widget's capture (or its
	// in-flight Down) must never be released, and another native window's capture is never touched.
	const bool bOperationOwnedCapture = CaptureState.IsValid() && CaptureState->bOwnedPointerCapture;
	const bool bOperationOwnedNativeCapture = CaptureState.IsValid()
		&& CaptureState->bOwnedNativeCapture;
	// Native mouse capture only ever belongs to the cursor user's physical mouse input (the same
	// condition the engine uses to release high-precision raw input, SlateApplication.cpp:3619).
	const bool bCursorUser =
		Binding.SlateUserIndex == static_cast<int32>(FSlateApplicationBase::CursorUserIndex);
	if ((bOperationOwnedCapture || bOperationOwnedNativeCapture) && !bForeignCapturePresent && bCursorUser)
	{
		if (const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication())
		{
			void* const CurrentCapture = PlatformApplication->GetCapture();
			if (CurrentCapture != nullptr)
			{
				// Release only the exact native capture this operation established, or (while this
				// operation still owns its Slate captor) the selected viewport's own native window.
				bool bMayRelease = bOperationOwnedNativeCapture
					&& CurrentCapture == CaptureState->OwnedNativeCaptureHandle;
				if (!bMayRelease && bOperationOwnedCapture)
				{
					const TSharedPtr<SWidget> Viewport = GetCoordinateRootWidget();
					const TSharedPtr<SWindow> Window = Viewport.IsValid()
						? Slate.FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
					const TSharedPtr<FGenericWindow> NativeWindow = Window.IsValid()
						? Window->GetNativeWindow() : nullptr;
					bMayRelease = NativeWindow.IsValid()
						&& CurrentCapture == NativeWindow->GetOSWindowHandle();
				}
				if (bMayRelease)
				{
					PlatformApplication->SetCapture(nullptr);
				}
			}
		}
	}

	// Step 6: observe owned state after cleanup.
	bool bNeutralized = true;
	for (const FKey& Key : ReleaseKeys)
	{
		if (Controller->IsInputKeyDown(Key))
		{
			bNeutralized = false;
		}
	}
	for (const FKey& Button : OwnedButtons)
	{
		if (Slate.GetPressedMouseButtons().Contains(Button))
		{
			bNeutralized = false;
		}
	}
	// This operation's own capture must be gone; a foreign capture is not ours to neutralize and is
	// left untouched.
	if (CaptureState.IsValid() && CaptureState->bOwnedPointerCapture && !bForeignCapturePresent)
	{
		if (Slate.HasUserMouseCapture(Binding.SlateUserIndex))
		{
			bNeutralized = false;
		}
	}
	// The native (OS) capture this operation established must also be gone: Slate releases only the
	// Slate captor when a drag starts, so this is the only check that catches a leaked OS capture. A
	// foreign window's native capture is not ours and is never reported as a failure.
	if (CaptureState.IsValid() && CaptureState->bOwnedNativeCapture && !bForeignCapturePresent)
	{
		if (const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication())
		{
			if (PlatformApplication->GetCapture() == CaptureState->OwnedNativeCaptureHandle)
			{
				bNeutralized = false;
			}
		}
	}
	// The exact drag-drop operation and high-precision mode this operation established must also be
	// gone before success is claimed; a foreign drag is not ours and is left untouched.
	if (CaptureState.IsValid() && CaptureState->bOwnedDragDrop)
	{
		const TSharedPtr<FDragDropOperation> OwnedDrag = CaptureState->OwnedDragDropContent.Pin();
		const TSharedPtr<FSlateUser> SelectedPostUser = Slate.GetUser(Binding.SlateUserIndex);
		if (OwnedDrag.IsValid() && SelectedPostUser.IsValid()
			&& SelectedPostUser->GetDragDropContent() == OwnedDrag)
		{
			bNeutralized = false;
		}
	}
	if (CaptureState.IsValid() && CaptureState->bOwnedHighPrecision
		&& Slate.IsUsingHighPrecisionMouseMovment())
	{
		bNeutralized = false;
	}
	if (CaptureState.IsValid())
	{
		CaptureState->OwnedSyntheticKeys.Reset();
		CaptureState->OwnedSyntheticButtons.Reset();
		CaptureState->CapturedHeldKeys.Reset();
		CaptureState->CapturedHeldButtons.Reset();
		CaptureState->bOwnedPointerCapture = false;
		CaptureState->OwnedCaptorPath.Reset();
		CaptureState->OwnedPointerEvent = FPointerEvent();
		CaptureState->bOwnedDragDrop = false;
		CaptureState->OwnedDragDropContent.Reset();
		CaptureState->bOwnedHighPrecision = false;
		CaptureState->bOwnedNativeCapture = false;
		CaptureState->OwnedNativeCaptureHandle = nullptr;
	}
	if (!bNeutralized)
	{
		return MakeErrorResult(CortexEditorPhysicalInputErrorCodes::CleanupFailed,
			TEXT("Owned physical input could not be safely neutralized on the original target"));
	}
	return MakeSuccessResult();
}
