// Owned-PIE lifecycle of FCortexEditorPhysicalInputSession: request admission and fingerprinting,
// preparation polling, owned-world authorization and teardown. Input capture, dispatch and target
// resolution live in CortexEditorPhysicalInputSession.cpp.

#include "CortexEditorPhysicalInputSession.h"

#include "CortexEditorModule.h"
#include "CortexEditorPhysicalInputResults.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "PlayInEditorDataTypes.h"
#include "RenderingThread.h"
#include "Containers/Ticker.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/Package.h"

using CortexEditorPhysicalInputResults::MakeErrorResult;
using CortexEditorPhysicalInputResults::MakeSuccessResult;

namespace
{
/** The production preparation budget is fixed and is not caller-configurable. */
constexpr double CortexOwnedPIEPreparationBudgetSeconds = 30.0;

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
				// Still queued: nothing of ours was ever created; cancel the exact request. The
				// fingerprint check above proves the queued copy is ours, so the public cancel can
				// never discard a successor's request.
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

	RequestOwnedTermination();

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
			// Our request is still queued: cancel it and drop ownership. The fingerprint check
			// proves the queued copy is ours, so the public cancel never discards a successor.
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
		// UEditorEngine processes a queued EndPlayMap after ticking PIE worlds. This exact
		// owned world is terminal, not a playback timing workaround: prevent an unrecorded
		// consumer tick while the engine performs its normal deferred destruction.
		UWorld* EndingWorld = ResolveOwnedContextWorld();
		if (EndingWorld == nullptr)
		{
			EndingWorld = OwnedWorld.Get();
		}
		check(EndingWorld != nullptr);
		EndingWorld->SetShouldTick(false);

		// Stock engines expose only the aggregate end. This session refuses to start while another
		// PIE session is active, so the aggregate teardown is the exact owned session in practice; a
		// late join during teardown is no longer protected from it.
		GEditor->RequestEndPlayMap();
		bOwnedEndPlayRequested = true;
		EnsureOwnedEndFlush();
	}
}

void FCortexEditorPhysicalInputSession::EnsureOwnedEndFlush()
{
	if (OwnedEndFlushHandle.IsValid())
	{
		return;
	}
	OwnedEndFlushHandle = FCoreDelegates::OnBeginFrame.AddLambda([this]()
	{
		if (!bOwnsPIE || !bOwnedEndPlayRequested || IsOwnedPIEEnded())
		{
			RemoveOwnedEndFlush();
			return;
		}
		// The editor destroys the owned floating PIE window on the game thread later in this frame
		// (EndPlayMap runs after the PIE world tick). Waiting for the render thread here guarantees no
		// draw of that window is in flight, so its viewport is never released by the render thread
		// (TD-EDITOR-004).
		FlushRenderingCommands();
	});
}

void FCortexEditorPhysicalInputSession::RemoveOwnedEndFlush()
{
	if (OwnedEndFlushHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(OwnedEndFlushHandle);
		OwnedEndFlushHandle.Reset();
	}
}

namespace
{
/** Sessions kept alive until their owned PIE teardown completes; intentionally leaked at process exit. */
TArray<TSharedPtr<FCortexEditorPhysicalInputSession>>& RetainedOwnedTeardownSessions()
{
	static TArray<TSharedPtr<FCortexEditorPhysicalInputSession>>* Sessions =
		new TArray<TSharedPtr<FCortexEditorPhysicalInputSession>>();
	return *Sessions;
}

FTSTicker::FDelegateHandle GRetainedOwnedTeardownTicker;
}

void FCortexEditorPhysicalInputSession::ShutdownAndRetainUntilOwnedTeardown(
	TSharedPtr<FCortexEditorPhysicalInputSession>&& Session)
{
	if (!Session.IsValid())
	{
		return;
	}
	Session->Shutdown();
	// The caller's callbacks are code the caller may unload next; none may outlive this call.
	Session->DetachCapture();
	if (!Session->bOwnsPIE || Session->IsOwnedPIEEnded())
	{
		Session.Reset();
		return;
	}
	RetainedOwnedTeardownSessions().Add(MoveTemp(Session));
	if (!GRetainedOwnedTeardownTicker.IsValid())
	{
		GRetainedOwnedTeardownTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				TArray<TSharedPtr<FCortexEditorPhysicalInputSession>>& Sessions = RetainedOwnedTeardownSessions();
				Sessions.RemoveAll([](const TSharedPtr<FCortexEditorPhysicalInputSession>& Retained)
				{
					return Retained->IsOwnedPIEEnded();
				});
				if (Sessions.IsEmpty())
				{
					GRetainedOwnedTeardownTicker.Reset();
					return false;
				}
				return true;
			}));
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

bool FCortexEditorPhysicalInputSession::IsWorldThisOwnedRequest(UWorld& World, FName ContextHandle) const
{
	if (GEditor == nullptr || GEngine == nullptr || !bOwnsPIE || ContextHandle == NAME_None)
	{
		return false;
	}
	// Only a live owned request (preparing or ready) authorizes a world; a failed preparation or a
	// resolved teardown invalidated the generation that accepted this request.
	if (OwnedState != EOwnedState::Preparing && OwnedState != EOwnedState::Ready)
	{
		return false;
	}
	if (World.WorldType != EWorldType::PIE)
	{
		return false;
	}
	// A context that existed before the request was queued is never ours.
	if (OwnedRequest.BaselinePIEContextHandles.Contains(ContextHandle))
	{
		return false;
	}
	// The world must be the map this operation requested.
	if (!OwnedRequest.RequestedPackagePath.IsEmpty()
		&& UWorld::RemovePIEPrefix(World.GetPackage()->GetName()) != OwnedRequest.RequestedPackagePath)
	{
		return false;
	}
	// Once this operation has pinned its exact identity, only that identity stays ours; a successor
	// context under a different handle is never adopted.
	if (OwnedContextHandle != NAME_None && ContextHandle != OwnedContextHandle)
	{
		return false;
	}
	if (OwnedWorld.IsValid() && &World != OwnedWorld.Get())
	{
		return false;
	}
	// A world cannot exist for a request that is still merely queued.
	if (bOwnedRequestOutstanding && IsOwnedRequestPending())
	{
		return false;
	}
	// The engine's started session must still be the exact request this operation accepted.
	const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
	if (!SessionInfo.IsSet() || SubmittedRequestFingerprint == 0
		|| ComputeRequestFingerprint(SessionInfo->OriginalRequestParams, false) != SubmittedRequestFingerprint)
	{
		return false;
	}
	// Ambiguity: exactly one non-baseline PIE context may exist, and it must be the given one.
	int32 NewContextCount = 0;
	FName OnlyNewContext = NAME_None;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE
			|| OwnedRequest.BaselinePIEContextHandles.Contains(Context.ContextHandle))
		{
			continue;
		}
		++NewContextCount;
		OnlyNewContext = Context.ContextHandle;
	}
	return NewContextCount == 1 && OnlyNewContext == ContextHandle;
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

FCortexCommandResult FCortexEditorPhysicalInputSession::CheckOwnedPIEAdmission(
	const FString& MapAssetPath, int32 LocalPlayerIndex) const
{
	if (GEditor == nullptr || GEngine == nullptr)
	{
		return MakeErrorResult(CortexErrorCodes::EditorNotReady, TEXT("Editor is not available"));
	}
	// Stock engines have no ownership-scoped PIE API. The owned end therefore uses the public
	// aggregate end, and this session refuses to start while any unrelated PIE session, play
	// request or pending teardown is active (checked below) so it never adopts or cancels one.
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
	return MakeSuccessResult();
}

FCortexCommandResult FCortexEditorPhysicalInputSession::BeginOwnedPIE(
	const FString& MapAssetPath,
	int32 LocalPlayerIndex,
	TFunction<void(const FCortexCommandResult&)>&& OnReady)
{
	const FCortexCommandResult Admission = CheckOwnedPIEAdmission(MapAssetPath, LocalPlayerIndex);
	if (!Admission.bSuccess)
	{
		return Admission;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(MapAssetPath);

	// Owned PIE runs in its own floating preview window (in-process, no destination Slate viewport).
	// A stock editor does not guarantee that the level-editor viewport tab is the active, arranged
	// and hit-testable route, and the owned session is automation/replay-facing: its input must
	// reach the game viewport independently of the human's docking layout.
	FRequestPlaySessionParams Request;
	Request.SessionDestination = EPlaySessionDestinationType::InProcess;
	Request.WorldType = EPlaySessionWorldType::PlayInEditor;
	Request.GlobalMapOverride = MapAssetPath;
	// StartLocation/StartRotation stay unset so the normal player start is used.

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
	RemoveOwnedEndFlush();
	BoundPawnClass = nullptr;
	LastObservedPawnClass = nullptr;
	StablePawnClassObservations = 0;
	// The native observer's opt-in pre-baseline monitoring survives this internal reset; only a
	// stale provisional route from an earlier attempt is dropped, and the route is re-resolved at
	// the new owned world's birth.
	bNativePreparationRouteBound = false;
	NativePreparationRoute = FCortexEditorPhysicalInputTargetBinding();
	NativePreparationWorld = nullptr;
	NativePreparationContextHandle = NAME_None;
	++Generation;

	EnsureTicker();

	// Queue the request through the public engine API. Stock builds expose no per-request teardown
	// authority, so the owned end uses the aggregate end; the exact queued copy is fingerprinted
	// below so this session never cancels or adopts a successor.
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

