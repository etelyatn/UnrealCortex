#include "CortexEditorEngineFrameObserver.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"

namespace
{
FCortexCommandResult NativeFrameError(const TCHAR* Message)
{
	return FCortexCommandRouter::Error(TEXT("REPLAY_TIMING_ERROR"), Message);
}

FName NativeTickTypeName(ELevelTick TickType)
{
	static const FName TimeOnly(TEXT("LEVELTICK_TimeOnly"));
	static const FName ViewportsOnly(TEXT("LEVELTICK_ViewportsOnly"));
	static const FName All(TEXT("LEVELTICK_All"));
	static const FName PauseTick(TEXT("LEVELTICK_PauseTick"));
	FName Name = NAME_None;
	switch (TickType)
	{
	case LEVELTICK_TimeOnly:
		Name = TimeOnly;
		break;
	case LEVELTICK_ViewportsOnly:
		Name = ViewportsOnly;
		break;
	case LEVELTICK_All:
		Name = All;
		break;
	case LEVELTICK_PauseTick:
		Name = PauseTick;
		break;
	default:
		break;
	}
	return Name;
}

bool HasIdenticalNativeFilter(const FCortexEditorNativeMouseFilterState& Left,
	const FCortexEditorNativeMouseFilterState& Right)
{
	return FMemory::Memcmp(Left.ZeroTimeSeconds, Right.ZeroTimeSeconds,
		 sizeof(Left.ZeroTimeSeconds)) == 0
		&& FMemory::Memcmp(Left.SmoothedMouse, Right.SmoothedMouse,
			sizeof(Left.SmoothedMouse)) == 0
		&& Left.SampleCount == Right.SampleCount
		&& FMemory::Memcmp(&Left.SamplingTotalSeconds, &Right.SamplingTotalSeconds,
			sizeof(Left.SamplingTotalSeconds)) == 0
		&& FMemory::Memcmp(&Left.EffectiveTimeDilation, &Right.EffectiveTimeDilation,
			sizeof(Left.EffectiveTimeDilation)) == 0;
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
	FCortexEditorEngineFrameRecord Records[2];
	TWeakObjectPtr<UWorld> SelectedWorld;
	FName SelectedContextHandle = NAME_None;
	uint64 AcceptedGeneration = 0;
	uint64 TotalSelectedTicks = 0;
	double FirstWorldRealSeconds = 0.0;
	double FirstWorldTimeSeconds = 0.0;
	int32 CurrentIndex = 0;
	int32 LastClosedIndex = INDEX_NONE;
	int32 CallbackDepth = 0;
	bool bInstalled = false;
	bool bFrameOpen = false;
	bool bGenerationPinned = false;
	bool bEverSelectedWorld = false;
	bool bWorldTickPending = false;
	bool bSlateWaitInProgress = false;
	bool bClearingCallbacks = false;

	FCortexEditorEngineFrameRecord& Current()
	{
		return Records[CurrentIndex];
	}

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
		Current().bBoundaryObserved = false;
		Current().SelectedWorldTickCount = -1;
		Current().WorldTick.Reset();
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
		CurrentIndex = LastClosedIndex == 0 ? 1 : 0;
		Current() = FCortexEditorEngineFrameRecord();
		Current().CaptureFrameCounter = GFrameCounter;
		Current().FrameBeginSeconds = FPlatformTime::Seconds();
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
		FCortexEditorEngineFrameRecord& Record = Current();
		if (Record.bBoundaryObserved || Record.SelectedWorldTickCount < 0
			|| !IsRequestCurrent() || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		Record.InputBoundarySeconds = FPlatformTime::Seconds();
		Record.AppDeltaSeconds = FApp::GetDeltaTime();
		Record.AppCurrentSeconds = FApp::GetCurrentTime();
		Record.AppLastSeconds = FApp::GetLastTime();
		Record.bBoundaryObserved = true;
		++CallbackDepth;
		OnInputBoundary(Record);
		--CallbackDepth;
		if (bInstalled && (Record.CaptureFrameCounter != GFrameCounter
			|| Record.AppDeltaSeconds != FApp::GetDeltaTime()
			|| Record.AppCurrentSeconds != FApp::GetCurrentTime()
			|| Record.AppLastSeconds != FApp::GetLastTime()))
		{
			InvalidateCurrent();
		}
		ClearCallbacksIfIdle();
	}

	void WorldStart(UWorld* World, ELevelTick TickType, float RealDeltaSeconds)
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
			FirstWorldRealSeconds = World->GetRealTimeSeconds();
			FirstWorldTimeSeconds = World->GetTimeSeconds();
			// Authorization and frame attribution precede ordinary Pawn/readiness.
			Session->BindNativeFramePreparationRoute(*World, ContextHandle);
			if (!bInstalled || !Session || !IsRequestCurrent())
			{
				return;
			}
		}
		FCortexEditorEngineFrameRecord& Record = Current();
		if (!Record.bBoundaryObserved || Record.SelectedWorldTickCount != 0
			|| bWorldTickPending || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		++TotalSelectedTicks;
		Record.SelectedWorldTickCount = 1;
		Record.WorldTick.Emplace();
		FCortexEditorObservedWorldTick& Tick = Record.WorldTick.GetValue();
		Tick.TickType = NativeTickTypeName(TickType);
		Tick.RealDeltaSeconds = RealDeltaSeconds;
		Tick.bPaused = World->IsPaused();
		bWorldTickPending = true;
	}

	void WorldEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds)
	{
		if (!bInstalled || !World || World != SelectedWorld.Get())
		{
			return;
		}
		FCortexEditorEngineFrameRecord& Record = Current();
		if (!bFrameOpen || !bWorldTickPending || !Record.WorldTick.IsSet()
			|| !IsRequestCurrent() || Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
			return;
		}
		FCortexEditorObservedWorldTick& Tick = Record.WorldTick.GetValue();
		Tick.TickType = NativeTickTypeName(TickType);
		Tick.DeltaSeconds = DeltaSeconds;
		Tick.EffectiveTimeDilation = World->GetWorldSettings()->GetEffectiveTimeDilation();
		Tick.RealTimeOffsetSeconds = World->GetRealTimeSeconds() - FirstWorldRealSeconds;
		Tick.TimeOffsetSeconds = World->GetTimeSeconds() - FirstWorldTimeSeconds;
		bWorldTickPending = false;
	}

	void EndFrame()
	{
		if (!bInstalled || !bFrameOpen)
		{
			return;
		}
		FCortexEditorEngineFrameRecord& Record = Current();
		if (!Record.bBoundaryObserved || bWorldTickPending || !IsRequestCurrent()
			|| Record.CaptureFrameCounter != GFrameCounter)
		{
			InvalidateCurrent();
		}
		Record.bEndObserved = true;
		bFrameOpen = false;
		LastClosedIndex = CurrentIndex;
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
		[WeakState](UWorld* World, ELevelTick TickType, float DeltaSeconds)
		{
			if (const TSharedPtr<FState> Pinned = WeakState.Pin())
			{
				Pinned->WorldStart(World, TickType, DeltaSeconds);
			}
		});
	NewState->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
		[WeakState](UWorld* World, ELevelTick TickType, float DeltaSeconds)
		{
			if (const TSharedPtr<FState> Pinned = WeakState.Pin())
			{
				Pinned->WorldEnd(World, TickType, DeltaSeconds);
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

bool FCortexEditorEngineFrameObserver::HasSelectedWorld() const
{
	check(IsInGameThread());
	return State.IsValid() && State->bInstalled && State->SelectedWorld.IsValid();
}

UWorld* FCortexEditorEngineFrameObserver::GetSelectedWorld() const
{
	check(IsInGameThread());
	return HasSelectedWorld() ? State->SelectedWorld.Get() : nullptr;
}

const FCortexEditorEngineFrameRecord* FCortexEditorEngineFrameObserver::GetLastClosedFrame() const
{
	check(IsInGameThread());
	return State.IsValid() && State->LastClosedIndex != INDEX_NONE
		? &State->Records[State->LastClosedIndex] : nullptr;
}

int32 FCortexEditorEngineFrameObserver::GetSelectedWorldTickCount() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->Current().SelectedWorldTickCount : 0;
}

FCortexCommandResult FCortexEditorEngineFrameObserver::RunSlateOnlyWaitWork()
{
	if (!IsInGameThread() || !FSlateApplication::IsInitialized())
	{
		return NativeFrameError(TEXT("Slate-only work requires the Game Thread and Slate"));
	}
	const TSharedPtr<FState> Pinned = State;
	if (!Pinned.IsValid() || !Pinned->bInstalled || !Pinned->Session
		|| !Pinned->SelectedWorld.IsValid() || Pinned->bSlateWaitInProgress
		|| FSlateApplication::Get().IsTicking() || !Pinned->IsRequestCurrent())
	{
		return NativeFrameError(TEXT("Slate-only work requires the exact live owned route and no reentry"));
	}
	FCortexEditorPhysicalInputSession& Session = *Pinned->Session;
	FString RouteReason;
	if (!Session.IsSelectedRouteOwnershipIntact(RouteReason))
	{
		return FCortexCommandRouter::Error(TEXT("REPLAY_TIMING_ERROR"), RouteReason);
	}
	FCortexEditorNativeMouseFilterState FilterBefore;
	const FCortexCommandResult FilterRead = Session.ReadNativeMouseFilterState(FilterBefore);
	if (!FilterRead.bSuccess)
	{
		return FilterRead;
	}
	const TWeakObjectPtr<UWorld> WeakWorld = Pinned->SelectedWorld;
	UWorld* World = WeakWorld.Get();
	const double WorldRealBefore = World->GetRealTimeSeconds();
	const double WorldTimeBefore = World->GetTimeSeconds();
	const float WorldDeltaBefore = World->GetDeltaSeconds();
	const bool bPausedBefore = World->IsPaused();
	const float DilationBefore = World->GetWorldSettings()->GetEffectiveTimeDilation();
	const uint64 CounterBefore = GFrameCounter;
	const uint64 TicksBefore = Pinned->TotalSelectedTicks;
	const double CurrentBefore = FApp::GetCurrentTime();
	const double LastBefore = FApp::GetLastTime();
	const double DeltaBefore = FApp::GetDeltaTime();
	const FCortexEditorPhysicalInputTargetBinding BindingBefore = Session.GetTargetBinding();
	{
		TGuardValue<bool> WaitScope(Pinned->bSlateWaitInProgress, true);
		FSlateApplication::Get().Tick(ESlateTickType::TimeAndWidgets);
	}
	if (!Pinned->bInstalled || !Pinned->Session || !Pinned->IsRequestCurrent()
		|| WeakWorld.Get() != World || !WeakWorld.IsValid())
	{
		return NativeFrameError(TEXT("The owned route was cancelled or replaced during Slate-only work"));
	}
	const FCortexEditorPhysicalInputTargetBinding& BindingAfter = Pinned->Session->GetTargetBinding();
	FCortexEditorNativeMouseFilterState FilterAfter;
	const FCortexCommandResult FilterReadAfter = Pinned->Session->ReadNativeMouseFilterState(FilterAfter);
	if (!FilterReadAfter.bSuccess)
	{
		return FilterReadAfter;
	}
	if (CounterBefore != GFrameCounter || TicksBefore != Pinned->TotalSelectedTicks
		|| CurrentBefore != FApp::GetCurrentTime() || LastBefore != FApp::GetLastTime()
		|| DeltaBefore != FApp::GetDeltaTime() || WorldRealBefore != World->GetRealTimeSeconds()
		|| WorldTimeBefore != World->GetTimeSeconds() || WorldDeltaBefore != World->GetDeltaSeconds()
		|| bPausedBefore != World->IsPaused()
		|| DilationBefore != World->GetWorldSettings()->GetEffectiveTimeDilation()
		|| !HasIdenticalNativeFilter(FilterBefore, FilterAfter)
		|| BindingBefore.World != BindingAfter.World || BindingBefore.Controller != BindingAfter.Controller
		|| BindingBefore.Pawn != BindingAfter.Pawn
		|| BindingBefore.InputRoot != BindingAfter.InputRoot
		|| BindingBefore.ViewportWidget != BindingAfter.ViewportWidget
		|| BindingBefore.SlateUserIndex != BindingAfter.SlateUserIndex
		|| BindingBefore.InputDevice != BindingAfter.InputDevice
		|| !Pinned->Session->IsSelectedRouteOwnershipIntact(RouteReason))
	{
		return NativeFrameError(TEXT("Slate-only work changed native frame, world, filter or route state"));
	}
	FCortexCommandResult Result;
	Result.bSuccess = true;
	return Result;
}
