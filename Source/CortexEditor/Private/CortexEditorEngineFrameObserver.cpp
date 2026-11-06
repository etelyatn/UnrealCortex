#include "CortexEditorEngineFrameObserver.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"

namespace
{
FCortexCommandResult NativeFrameError(const TCHAR* Message)
{
	return FCortexCommandRouter::Error(TEXT("REPLAY_TIMING_ERROR"), Message);
}
}

struct FCortexEditorEngineFrameObserver::FState
{
	FCortexEditorPhysicalInputSession* Session = nullptr;
	TFunction<void(const FCortexEditorEngineFrameRecord&)> OnInputBoundary;
	TFunction<void(const FCortexEditorEngineFrameRecord&)> OnFrameClosed;
	FDelegateHandle BeginFrameHandle;
	FDelegateHandle SamplingHandle;
	FDelegateHandle WorldStartHandle;
	FDelegateHandle WorldEndHandle;
	FDelegateHandle EndFrameHandle;
	/** The engine frame in flight; reset at every OnBeginFrame and handed out by reference. */
	FCortexEditorEngineFrameRecord Record;
	TWeakObjectPtr<UWorld> SelectedWorld;
	FName SelectedContextHandle = NAME_None;
	uint64 AcceptedGeneration = 0;
	int32 CallbackDepth = 0;
	bool bInstalled = false;
	bool bFrameOpen = false;
	bool bGenerationPinned = false;
	bool bEverSelectedWorld = false;
	bool bWorldTickPending = false;
	bool bClearingCallbacks = false;

	bool IsRequestCurrent()
	{
		if (!Session)
		{
			return false;
		}
		if (!bGenerationPinned)
		{
			if (Session->bOwnsPIE)
			{
				AcceptedGeneration = Session->Generation;
				bGenerationPinned = true;
			}
			return true; // Installed before the request: no invented successor generation.
		}
		return Session->bOwnsPIE && Session->Generation == AcceptedGeneration;
	}

	void InvalidateCurrent()
	{
		Record.bBoundaryObserved = false;
		Record.SelectedWorldTickCount = -1;
		Record.WorldTick.Reset();
		bWorldTickPending = false;
	}

	void ClearCallbacksIfIdle()
	{
		if (!bInstalled && CallbackDepth == 0 && !bClearingCallbacks)
		{
			TGuardValue<bool> Clearing(bClearingCallbacks, true);
			OnInputBoundary = nullptr;
			OnFrameClosed = nullptr;
		}
	}

	void BeginFrame()
	{
		if (!bInstalled)
		{
			return;
		}
		Record = FCortexEditorEngineFrameRecord();
		Record.CaptureFrameCounter = GFrameCounter;
		bFrameOpen = true;
		bWorldTickPending = false;
		if (!IsRequestCurrent())
		{
			InvalidateCurrent();
		}
	}

	void SampleInput()
	{
		if (!bInstalled || !bFrameOpen)
		{
			return;
		}
		if (Record.bBoundaryObserved || Record.SelectedWorldTickCount < 0
			|| !IsRequestCurrent() || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		Record.AppDeltaSeconds = FApp::GetDeltaTime();
		Record.bBoundaryObserved = true;
		++CallbackDepth;
		OnInputBoundary(Record);
		--CallbackDepth;
		if (bInstalled && (Record.CaptureFrameCounter != GFrameCounter
			|| Record.AppDeltaSeconds != FApp::GetDeltaTime()))
		{
			InvalidateCurrent();
		}
		ClearCallbacksIfIdle();
	}

	void WorldStart(UWorld* World, float RealDeltaSeconds)
	{
		if (!bInstalled || !World || World->WorldType != EWorldType::PIE)
		{
			return;
		}
		if (!bFrameOpen || !IsRequestCurrent() || !bGenerationPinned || !GEngine)
		{
			InvalidateCurrent();
			return;
		}
		FName ContextHandle = NAME_None;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() == World && Context.WorldType == EWorldType::PIE)
			{
				ContextHandle = Context.ContextHandle;
				break;
			}
		}
		if (ContextHandle == NAME_None || !Session->IsWorldThisOwnedRequest(*World, ContextHandle))
		{
			InvalidateCurrent(); // Foreign PIE cannot consume this operation's global clock.
			return;
		}
		if (bEverSelectedWorld)
		{
			if (SelectedWorld.Get() != World || SelectedContextHandle != ContextHandle)
			{
				InvalidateCurrent();
				return;
			}
		}
		else
		{
			SelectedWorld = World;
			SelectedContextHandle = ContextHandle;
			bEverSelectedWorld = true;
			// Authorization and frame attribution precede ordinary Pawn/readiness.
			Session->BindNativeFramePreparationRoute(*World, ContextHandle);
			if (!bInstalled || !Session || !IsRequestCurrent())
			{
				return;
			}
		}
		if (!Record.bBoundaryObserved || Record.SelectedWorldTickCount != 0
			|| bWorldTickPending || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		Record.SelectedWorldTickCount = 1;
		Record.WorldTick.Emplace();
		Record.WorldTick->RealDeltaSeconds = RealDeltaSeconds;
		Record.WorldTick->bPaused = World->IsPaused();
		bWorldTickPending = true;
	}

	void WorldEnd(UWorld* World)
	{
		if (!bInstalled || !World || World != SelectedWorld.Get())
		{
			return;
		}
		if (!bFrameOpen || !bWorldTickPending || !Record.WorldTick.IsSet()
			|| !IsRequestCurrent() || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		bWorldTickPending = false;
	}

	void EndFrame()
	{
		if (!bInstalled || !bFrameOpen)
		{
			return;
		}
		if (!Record.bBoundaryObserved || bWorldTickPending || !IsRequestCurrent()
			|| Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
		}
		bFrameOpen = false;
		++CallbackDepth;
		OnFrameClosed(Record);
		--CallbackDepth;
		ClearCallbacksIfIdle();
	}

	void Uninstall()
	{
		if (!bInstalled)
		{
			ClearCallbacksIfIdle();
			return;
		}
		bInstalled = false;
		FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
		FCoreDelegates::OnSamplingInput.Remove(SamplingHandle);
		FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
		FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
		FCortexEditorPhysicalInputSession* PreviousSession = Session;
		Session = nullptr;
		if (PreviousSession)
		{
			PreviousSession->EndNativeFramePreparationMonitoring();
		}
		ClearCallbacksIfIdle();
	}
};

FCortexEditorEngineFrameObserver::FCortexEditorEngineFrameObserver() = default;

FCortexEditorEngineFrameObserver::~FCortexEditorEngineFrameObserver()
{
	Uninstall();
}

FCortexCommandResult FCortexEditorEngineFrameObserver::Install(
	FCortexEditorPhysicalInputSession& Session,
	TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnInputBoundary,
	TFunction<void(const FCortexEditorEngineFrameRecord&)>&& OnFrameClosed)
{
	if (!IsInGameThread() || !FSlateApplication::IsInitialized() || !GEngine)
	{
		return NativeFrameError(TEXT("Native frame observation requires the Game Thread, engine and Slate"));
	}
	if ((State.IsValid() && State->bInstalled) || Session.bOwnsPIE
		|| Session.GetTargetBinding().World.IsValid() || !OnInputBoundary || !OnFrameClosed)
	{
		return NativeFrameError(TEXT("Install the native observer exactly once before the owned PIE request"));
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			return NativeFrameError(TEXT("Native observation cannot adopt an existing PIE context"));
		}
	}
	const TSharedPtr<FState> NewState = MakeShared<FState>();
	NewState->Session = &Session;
	NewState->OnInputBoundary = MoveTemp(OnInputBoundary);
	NewState->OnFrameClosed = MoveTemp(OnFrameClosed);
	NewState->bInstalled = true;
	State = NewState;
	const TWeakPtr<FState> WeakState = NewState;
	NewState->BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddLambda([WeakState]()
	{
		if (const TSharedPtr<FState> Pinned = WeakState.Pin())
		{
			Pinned->BeginFrame();
		}
	});
	NewState->SamplingHandle = FCoreDelegates::OnSamplingInput.AddLambda([WeakState]()
	{
		if (const TSharedPtr<FState> Pinned = WeakState.Pin())
		{
			Pinned->SampleInput();
		}
	});
	NewState->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakState](UWorld* World, ELevelTick, float DeltaSeconds)
		{
			if (const TSharedPtr<FState> Pinned = WeakState.Pin())
			{
				Pinned->WorldStart(World, DeltaSeconds);
			}
		});
	NewState->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
		[WeakState](UWorld* World, ELevelTick, float)
		{
			if (const TSharedPtr<FState> Pinned = WeakState.Pin())
			{
				Pinned->WorldEnd(World);
			}
		});
	NewState->EndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakState]()
	{
		if (const TSharedPtr<FState> Pinned = WeakState.Pin())
		{
			Pinned->EndFrame();
		}
	});
	Session.BeginNativeFramePreparationMonitoring();
	if (!NewState->bInstalled)
	{
		return NativeFrameError(TEXT("Native observation was cancelled during installation"));
	}
	FCortexCommandResult Result;
	Result.bSuccess = true;
	return Result;
}

void FCortexEditorEngineFrameObserver::Uninstall()
{
	check(IsInGameThread());
	const TSharedPtr<FState> Pinned = State;
	if (Pinned.IsValid())
	{
		Pinned->Uninstall();
	}
}

const FCortexEditorEngineFrameRecord* FCortexEditorEngineFrameObserver::GetOpenFrame() const
{
	check(IsInGameThread());
	return State.IsValid() && State->bInstalled && State->bFrameOpen && State->Record.bBoundaryObserved
		? &State->Record : nullptr;
}
