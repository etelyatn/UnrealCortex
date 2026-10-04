#include "CortexEditorPhysicalInputSession.h"

#include "CortexCommandRouter.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Slate/SGameLayerManager.h"
#include "Slate/SceneViewport.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"

#include "CortexEditorModule.h"

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

	if (OwnedState == EOwnedState::Preparing)
	{
		PollPreparation();
	}
	else if (OwnedState == EOwnedState::Ending)
	{
		// Owned teardown must keep observing startup/context until it is resolved.
		PollTeardown();
	}

	if (OwnedState == EOwnedState::Ending && IsOwnedPIEEnded())
	{
		OwnedState = EOwnedState::None;
		TickerHandle.Reset();
		return false; // Teardown observed; stop the minimal observer.
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
	CompletePreparationSuccess();
}

void FCortexEditorPhysicalInputSession::PollTeardown()
{
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

	// End only once the captured owned context has a world to tear down. A world-less context
	// is still ours and must stay observed: ending it before its deferred world appears is not
	// what resolves the session (its world appears at PlayLevel.cpp:1603-1622).
	if (IsOwnedContextWorldPresent())
	{
		GEditor->RequestEndPlayMap();
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
	if (GEditor == nullptr)
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
	// End only when the exact captured owned PIE context is actually present.
	if (IsOwnedContextPresent())
	{
		GEditor->RequestEndPlayMap();
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
	if (GEngine == nullptr)
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
			return true;
		}
		if (OwnedWorld.IsValid() && Context.World() == OwnedWorld.Get())
		{
			return true;
		}
	}
	return false;
}

bool FCortexEditorPhysicalInputSession::IsOwnedContextWorldPresent() const
{
	if (GEngine == nullptr)
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
			return Context.World() != nullptr;
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
	++Generation;
	bBound = false;
	Binding = FCortexEditorPhysicalInputTargetBinding();
	TargetInfo = FCortexEditorPhysicalInputTargetInfo();
	BoundPawnClass = nullptr;
}

void FCortexEditorPhysicalInputSession::BeginOwnedTermination()
{
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
	LastObservedPawnClass = nullptr;
	StablePawnClassObservations = 0;
	ReleaseReadyBinding();
	bReadyCallbackInvoked = true;
	ReadyCallback = nullptr;

	if (bOwnsPIE)
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
