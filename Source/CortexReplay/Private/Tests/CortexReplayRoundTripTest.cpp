#include "Misc/AutomationTest.h"

#include "CortexReplayLibrary.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace CortexReplayRoundTripTest
{
/** Harness-only failure reporting; the product behaviour itself is asserted by the tests. */
constexpr double ViewportReadyWatchdogSeconds = 45.0;
constexpr double ScriptWatchdogSeconds = 180.0;
constexpr double CaptureReleaseWatchdogSeconds = 90.0;
constexpr double ReplayWatchdogSeconds = 180.0;
constexpr double TeardownWatchdogSeconds = 45.0;

/** Engine frames the capture keeps running after the last scripted frame, so its tick has happened. */
constexpr uint64 TrailingFramesAfterScript = 3;

constexpr double YawToleranceDegrees = 1e-3;
constexpr double LocationToleranceCm = 0.01;

/** Same convention as the lifecycle tests: the alternative to the open editor map. */
FString MakeMapAssetPath()
{
	if (!GEditor)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	return Current == TEXT("/Game/Maps/TestMap") ? TEXT("/Game/Maps/TestRoomMap")
		: TEXT("/Game/Maps/TestMap");
}

bool HasAnyPieWorld()
{
	if (!GEngine)
	{
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			return true;
		}
	}
	return false;
}

bool IsTerminalRunState(const FString& State)
{
	return State == TEXT("Completed") || State == TEXT("Cancelled")
		|| State == TEXT("Interrupted") || State == TEXT("Error");
}

/** Type name of a widget for diagnostics, or "none". */
FString DescribeWidget(const TSharedPtr<SWidget>& Widget)
{
	return Widget.IsValid() ? Widget->GetType().ToString() : FString(TEXT("none"));
}

FString DescribeFocus()
{
	return FSlateApplication::IsInitialized()
		? DescribeWidget(FSlateApplication::Get().GetKeyboardFocusedWidget()) : FString(TEXT("noslate"));
}

/** The widget the cursor user's pointer is currently captured by. */
FString DescribeCaptor()
{
	if (!FSlateApplication::IsInitialized())
	{
		return TEXT("noslate");
	}
	const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(0);
	return User.IsValid() ? DescribeWidget(User->GetCursorCaptor()) : FString(TEXT("nouser"));
}

bool IsHighPrecision()
{
	return FSlateApplication::IsInitialized() && FSlateApplication::Get().IsUsingHighPrecisionMouseMovment();
}

/** One owned-world tick as a gameplay consumer sees it at the end of the tick. */
struct FTickSample
{
	uint64 Frame = 0;
	FVector Location = FVector::ZeroVector;
	FRotator Control = FRotator::ZeroRotator;
	double MouseXRaw = 0.0;
	bool bMouseCapture = false;
	bool bViewportFocus = false;
	bool bHighPrecision = false;
	FString Captor;
	FString Focus;
	/** The replay run's dispatched_events at this tick; INDEX_NONE on the capture leg. */
	int32 DispatchedEvents = INDEX_NONE;
};

/**
 * One test's shared state: the isolated library, the service, the OS-style injector driven from
 * OnBeginFrame (before PumpMessages/OnSamplingInput, where real OS input is processed) and the
 * per-tick sampler driven from OnWorldTickEnd of the PIE world. Hooks, fixed-step settings and the
 * service are all released by the destructor whatever a test did.
 */
struct FRoundTripState
{
	using FScript = TFunction<void(FRoundTripState&, int32)>;

	TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture->GetProjectRoot());

	const bool bSavedFixedStep = FApp::UseFixedTimeStep();
	const double SavedFixedDelta = FApp::GetFixedDeltaTime();

	bool bAbort = false;
	int32 RecordingId = 0;
	FGuid RunId;
	bool bRunTerminalObserved = false;

	// Injector
	FScript Script;
	int32 ScriptFrames = 0;
	int32 ScriptFrame = 0;
	bool bScripting = false;
	bool bScriptDone = false;
	uint64 LastScriptFrameCounter = 0;
	TWeakPtr<SViewport> Viewport;

	// Diagnostics: notes are collected on the game thread from hooks and flushed as AddInfo by steps.
	TArray<FString> Log;
	TWeakObjectPtr<UGameViewportClient> KeyClient;
	FDelegateHandle ViewportKeyHandle;

	void Note(const FString& Line)
	{
		Log.Add(Line);
	}

	void Flush(FAutomationTestBase& Test)
	{
		for (const FString& Line : Log)
		{
			Test.AddInfo(Line);
		}
		Log.Reset();
	}

	/** Gives the owned viewport widget keyboard focus, as a human's click/alt-tab back would. */
	void EnsureViewportFocus()
	{
		const TSharedPtr<SViewport> Widget = Viewport.Pin();
		if (!Widget.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}
		const FString Before = DescribeFocus();
		FSlateApplication::Get().SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
		Note(FString::Printf(TEXT("frame %llu: keyboard focus before=%s after=%s"),
			GFrameCounter, *Before, *DescribeFocus()));
	}

	// Sampler
	bool bSampling = true;
	TFunction<bool()> SampleGate;
	TArray<FTickSample> Samples;
	TArray<FTickSample> CaptureSamples;

	FDelegateHandle BeginFrameHandle;
	FDelegateHandle TickEndHandle;

	FRoundTripState()
	{
		BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddRaw(this, &FRoundTripState::OnBeginFrame);
		TickEndHandle = FWorldDelegates::OnWorldTickEnd.AddRaw(this, &FRoundTripState::OnWorldTickEnd);
	}

	~FRoundTripState()
	{
		RemoveHooks();
		Service->Shutdown();
		FApp::SetUseFixedTimeStep(bSavedFixedStep);
		FApp::SetFixedDeltaTime(SavedFixedDelta);
	}

	void RemoveHooks()
	{
		if (BeginFrameHandle.IsValid())
		{
			FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
			BeginFrameHandle.Reset();
		}
		if (ViewportKeyHandle.IsValid())
		{
			if (UGameViewportClient* Client = KeyClient.Get())
			{
				Client->OnInputKey().Remove(ViewportKeyHandle);
			}
			ViewportKeyHandle.Reset();
		}
		if (TickEndHandle.IsValid())
		{
			FWorldDelegates::OnWorldTickEnd.Remove(TickEndHandle);
			TickEndHandle.Reset();
		}
	}

	// ---- OS-style injection through the Slate platform message handler entry points ----

	FVector2D ViewportCenter() const
	{
		const TSharedPtr<SViewport> Widget = Viewport.Pin();
		if (!Widget.IsValid())
		{
			return FVector2D::ZeroVector;
		}
		const FGeometry Geometry = Widget->GetCachedGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		return Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));
	}

	void LeftClick()
	{
		const FVector2D Center = ViewportCenter();
		const TSharedPtr<SViewport> Widget = Viewport.Pin();
		Note(FString::Printf(
			TEXT("frame %llu: injecting LMB click at (%.1f,%.1f); BEFORE click: viewport capture=%d captor=%s focus=%s highPrecision=%d"),
			GFrameCounter, Center.X, Center.Y, Widget.IsValid() && Widget->HasMouseCapture() ? 1 : 0,
			*DescribeCaptor(), *DescribeFocus(), IsHighPrecision() ? 1 : 0));
		FSlateApplication::Get().OnMouseDown(nullptr, EMouseButtons::Left, Center);
		Note(FString::Printf(TEXT("frame %llu: after LMB down: viewport capture=%d captor=%s focus=%s highPrecision=%d"),
			GFrameCounter, Widget.IsValid() && Widget->HasMouseCapture() ? 1 : 0,
			*DescribeCaptor(), *DescribeFocus(), IsHighPrecision() ? 1 : 0));
		FSlateApplication::Get().OnMouseUp(EMouseButtons::Left, Center);
		Note(FString::Printf(TEXT("frame %llu: after LMB up: viewport capture=%d captor=%s focus=%s highPrecision=%d"),
			GFrameCounter, Widget.IsValid() && Widget->HasMouseCapture() ? 1 : 0,
			*DescribeCaptor(), *DescribeFocus(), IsHighPrecision() ? 1 : 0));
	}

	/**
	 * A real key press reaches Slate through FSlateApplication::OnKeyDown(VK, char, repeat), is offered to
	 * the input preprocessors, then routed to the keyboard-focused widget. For the PIE viewport that is
	 * SViewport -> FSceneViewport::OnKeyDown -> UGameViewportClient::InputKey, which gives
	 * GameViewportInputKeyDelegate (UEditorEngine::ProcessDebuggerCommands, PlayLevel.cpp:1089-1094,
	 * bound at 3092) the first chance on IE_Pressed and runs the Escape StopPlaySession chord
	 * (DebuggerCommands.cpp:358,471) -> RequestEndPlayMap. That needs the viewport to be the
	 * keyboard-focused widget and the viewport client not to ignore input.
	 */
	bool KeyDown(const FKey& Key)
	{
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
		const int32 Vk = KeyCode ? static_cast<int32>(*KeyCode) : 0;
		const uint32 Char = CharCode ? *CharCode : 0;
		UGameViewportClient* Client = KeyClient.Get();
		const bool bHandled = FSlateApplication::Get().OnKeyDown(Vk, Char, false);
		Note(FString::Printf(
			TEXT("frame %llu: OnKeyDown %s (vk=%d char=%u) handled=%d focus=%s ignoreInput=%d endPlayQueued=%d"),
			GFrameCounter, *Key.ToString(), Vk, Char, bHandled ? 1 : 0, *DescribeFocus(),
			Client && Client->IgnoreInput() ? 1 : 0, GEditor && GEditor->ShouldEndPlayMap() ? 1 : 0));
		return bHandled;
	}

	bool KeyUp(const FKey& Key)
	{
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
		const int32 Vk = KeyCode ? static_cast<int32>(*KeyCode) : 0;
		const uint32 Char = CharCode ? *CharCode : 0;
		const bool bHandled = FSlateApplication::Get().OnKeyUp(Vk, Char, false);
		Note(FString::Printf(TEXT("frame %llu: OnKeyUp %s handled=%d"), GFrameCounter, *Key.ToString(),
			bHandled ? 1 : 0));
		return bHandled;
	}

	void RawMouseMove(int32 DeltaX, int32 DeltaY)
	{
		FSlateApplication::Get().OnRawMouseMove(DeltaX, DeltaY);
	}

	void OnBeginFrame()
	{
		if (!bScripting || bScriptDone)
		{
			return;
		}
		if (!Viewport.IsValid() || !FSlateApplication::IsInitialized())
		{
			// The owned PIE viewport is gone (the session ended): nothing left to inject into.
			bScripting = false;
			bScriptDone = true;
			return;
		}
		Script(*this, ScriptFrame);
		LastScriptFrameCounter = GFrameCounter;
		if (++ScriptFrame >= ScriptFrames)
		{
			bScripting = false;
			bScriptDone = true;
		}
	}

	// ---- Per-tick sampling ----

	void OnWorldTickEnd(UWorld* World, ELevelTick, float)
	{
		if (!bSampling || !World || World->WorldType != EWorldType::PIE)
		{
			return;
		}
		if (SampleGate && !SampleGate())
		{
			return;
		}
		APlayerController* Controller = World->GetFirstPlayerController();
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (!Controller || !Pawn)
		{
			return;
		}
		FTickSample Sample;
		Sample.Frame = GFrameCounter;
		Sample.Location = Pawn->GetActorLocation();
		Sample.Control = Controller->GetControlRotation();
		if (const FKeyState* MouseX = Controller->PlayerInput
			? Controller->PlayerInput->GetKeyState(EKeys::MouseX) : nullptr)
		{
			Sample.MouseXRaw = MouseX->RawValue.X;
		}
		// The viewport is resolved per tick from the ticking world: the replay leg runs in a different
		// PIE session than the capture leg, so a cached capture-leg widget would be stale.
		UGameViewportClient* ViewportClient = World->GetGameViewport();
		const TSharedPtr<SViewport> Widget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
		if (Widget.IsValid() && FSlateApplication::IsInitialized())
		{
			Sample.bMouseCapture = Widget->HasMouseCapture();
			Sample.bViewportFocus = FSlateApplication::Get().GetKeyboardFocusedWidget().Get() == Widget.Get();
		}
		Sample.bHighPrecision = IsHighPrecision();
		Sample.Captor = DescribeCaptor();
		Sample.Focus = DescribeFocus();
		if (RunId.IsValid())
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, false);
			if (Run.bSuccess && Run.Data.IsValid() && Run.Data->HasField(TEXT("dispatched_events")))
			{
				Sample.DispatchedEvents = static_cast<int32>(Run.Data->GetNumberField(TEXT("dispatched_events")));
			}
		}
		Samples.Add(Sample);
	}
};

// ---------------------------------------------------------------------------
// Latent commands
// ---------------------------------------------------------------------------

/** Runs one synchronous step once; skipped once the run aborted unless asked to always run. */
class FStep : public IAutomationLatentCommand
{
public:
	FStep(FAutomationTestBase* InTest, TSharedRef<FRoundTripState> InState,
		TFunction<void(FAutomationTestBase&, FRoundTripState&)> InAction, bool bInAlways = false)
		: Test(InTest), State(MoveTemp(InState)), Action(MoveTemp(InAction)), bAlways(bInAlways) {}

	bool Update() override
	{
		if (!bAlways && State->bAbort)
		{
			return true;
		}
		Action(*Test, *State);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FRoundTripState> State;
	TFunction<void(FAutomationTestBase&, FRoundTripState&)> Action;
	bool bAlways;
};

/** Polls a condition with a harness watchdog; a timeout reports an error and aborts the run. */
class FWait : public IAutomationLatentCommand
{
public:
	FWait(FAutomationTestBase* InTest, TSharedRef<FRoundTripState> InState, FString InWhat,
		TFunction<bool(FRoundTripState&)> InDone, double InWatchdogSeconds, bool bInAlways = false)
		: Test(InTest), State(MoveTemp(InState)), What(MoveTemp(InWhat)), Done(MoveTemp(InDone))
		, WatchdogSeconds(InWatchdogSeconds), bAlways(bInAlways) {}

	bool Update() override
	{
		if (!bAlways && State->bAbort)
		{
			return true;
		}
		if (StartTime == 0.0)
		{
			StartTime = FPlatformTime::Seconds();
		}
		if (Done(*State))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > WatchdogSeconds)
		{
			Test->AddError(FString::Printf(TEXT("Timed out waiting for: %s"), *What));
			State->bAbort = true;
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FRoundTripState> State;
	FString What;
	TFunction<bool(FRoundTripState&)> Done;
	double WatchdogSeconds;
	bool bAlways;
	double StartTime = 0.0;
};

FString CapturePhase(const FCortexReplayService& Service)
{
	const FCortexCommandResult Current = Service.GetCurrentOperation();
	return Current.bSuccess && Current.Data.IsValid() ? Current.Data->GetStringField(TEXT("state")) : FString();
}

bool IsCaptureIdle(const FCortexReplayService& Service)
{
	const FCortexCommandResult Current = Service.GetCurrentOperation();
	return Current.bSuccess && !Current.Data.IsValid();
}

/**
 * Waits until the owned PIE viewport exists, its window is active and hit-testable at its centre,
 * and gives it keyboard focus, exactly the state a human has before touching the mouse.
 */
bool PrepareViewport(FRoundTripState& State)
{
	UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	if (!PlayWorld || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
	const TSharedPtr<SViewport> Widget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
	if (!Widget.IsValid())
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Widget.ToSharedRef());
	if (!Window.IsValid())
	{
		return false;
	}
	Window->BringToFront();
	Slate.SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
	if (!Slate.IsActive())
	{
		return false;
	}
	const FGeometry Geometry = Widget->GetCachedGeometry();
	const FVector2D Size = Geometry.GetLocalSize();
	if (Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return false;
	}
	const FVector2D Center = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));
	TArray<TSharedRef<SWindow>> Windows;
	Windows.Add(Window.ToSharedRef());
	const FWidgetPath Hit = Slate.LocateWindowUnderMouse(
		Center, Windows, false, static_cast<int32>(Slate.GetUserIndexForMouse()));
	if (!Hit.IsValid() || !Hit.ContainsWidget(Widget.Get()))
	{
		return false;
	}
	State.Viewport = Widget;
	if (!State.ViewportKeyHandle.IsValid())
	{
		State.KeyClient = ViewportClient;
		FRoundTripState* const Raw = &State;
		State.ViewportKeyHandle = ViewportClient->OnInputKey().AddLambda(
			[Raw](const FInputKeyEventArgs& Args)
			{
				Raw->Note(FString::Printf(TEXT("frame %llu: viewport client InputKey %s event=%d"),
					GFrameCounter, *Args.Key.ToString(), static_cast<int32>(Args.Event)));
			});
	}
	return true;
}

/** Starts the owned capture and queues the steps that bring it to a focused, scriptable Recording. */
bool QueueCaptureUntilScriptable(FAutomationTestBase& Test, const TSharedRef<FRoundTripState>& State)
{
	// The capture leg samples exactly the PIE ticks that end with the capture already Recording.
	State->SampleGate = [Raw = &State.Get()]() { return CapturePhase(*Raw->Service) == TEXT("Recording"); };
	const FCortexCommandResult Started = State->Service->StartCapture(MakeMapAssetPath());
	Test.TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FWait(&Test, State, TEXT("capture to reach Recording"),
		[](FRoundTripState& S) { return CapturePhase(*S.Service) == TEXT("Recording"); },
		ViewportReadyWatchdogSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(&Test, State,
		[](FAutomationTestBase& T, FRoundTripState& S)
		{
			const FCortexCommandResult Current = S.Service->GetCurrentOperation();
			if (Current.bSuccess && Current.Data.IsValid())
			{
				S.RecordingId = static_cast<int32>(Current.Data->GetNumberField(TEXT("recording_id")));
			}
			T.TestTrue(TEXT("Recording identity is known"), S.RecordingId > 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(&Test, State,
		TEXT("owned PIE viewport to be active, focused and hit-testable"),
		[](FRoundTripState& S) { return PrepareViewport(S); }, ViewportReadyWatchdogSeconds));
	return true;
}

/** Arms the scripted injector. */
void QueueScriptStart(FAutomationTestBase& Test, const TSharedRef<FRoundTripState>& State,
	int32 ScriptFrames, FRoundTripState::FScript Script)
{
	ADD_LATENT_AUTOMATION_COMMAND(FStep(&Test, State,
		[ScriptFrames, Script = MoveTemp(Script)](FAutomationTestBase&, FRoundTripState& S)
		{
			S.Script = Script;
			S.ScriptFrames = ScriptFrames;
			S.ScriptFrame = 0;
			S.bScriptDone = false;
			S.bScripting = true;
		}));
}

/** Always-run teardown: stops anything still owned, waits for PIE to go and releases the hooks. */
void QueueCleanup(FAutomationTestBase& Test, const TSharedRef<FRoundTripState>& State)
{
	ADD_LATENT_AUTOMATION_COMMAND(FStep(&Test, State,
		[](FAutomationTestBase& T, FRoundTripState& S)
		{
			S.Flush(T);
			S.bScripting = false;
			S.bSampling = false;
			if (!CapturePhase(*S.Service).IsEmpty())
			{
				S.Service->StopCapture(true);
			}
			if (S.RunId.IsValid() && !S.bRunTerminalObserved)
			{
				S.Service->CancelReplay(S.RunId, false);
			}
			if (GEditor && GEditor->PlayWorld)
			{
				GEditor->RequestEndPlayMap();
			}
		}, true));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(&Test, State, TEXT("PIE teardown"),
		[](FRoundTripState&) { return !HasAnyPieWorld(); }, TeardownWatchdogSeconds, true));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(&Test, State,
		[](FAutomationTestBase&, FRoundTripState& S)
		{
			S.RemoveHooks();
			S.Service->Shutdown();
		}, true));
}

// ---------------------------------------------------------------------------
// Sequence analysis
// ---------------------------------------------------------------------------

double TotalYawChange(const TArray<FTickSample>& Sequence, int32 Count)
{
	double Total = 0.0;
	for (int32 Index = 1; Index < Count; ++Index)
	{
		Total += FMath::FindDeltaAngleDegrees(Sequence[Index - 1].Control.Yaw, Sequence[Index].Control.Yaw);
	}
	return FMath::Abs(Total);
}

double MaxDistanceFromStart(const TArray<FTickSample>& Sequence, int32 Count)
{
	double Max = 0.0;
	for (int32 Index = 1; Index < Count; ++Index)
	{
		Max = FMath::Max(Max, (Sequence[Index].Location - Sequence[0].Location).Size());
	}
	return Max;
}

FString DescribeSample(const FTickSample& Sample)
{
	return FString::Printf(
		TEXT("f=%llu loc=(%.3f,%.3f,%.3f) yaw=%.4f mouseX=%.1f capture=%d highPrec=%d captor=%s focus=%s focusIsViewport=%d dispatched=%d"),
		Sample.Frame, Sample.Location.X, Sample.Location.Y, Sample.Location.Z, Sample.Control.Yaw,
		Sample.MouseXRaw, Sample.bMouseCapture ? 1 : 0, Sample.bHighPrecision ? 1 : 0,
		*Sample.Captor, *Sample.Focus, Sample.bViewportFocus ? 1 : 0, Sample.DispatchedEvents);
}

/** Logs a published recording's events and frames; the event's frame is the frame whose range holds it. */
void LogRecording(FAutomationTestBase& Test, const FCortexReplaySnapshot& Snapshot)
{
	for (int32 Index = 0; Index < Snapshot.Events.Num(); ++Index)
	{
		const FCortexReplayEvent& Event = Snapshot.Events[Index];
		int32 Frame = INDEX_NONE;
		for (const FCortexReplayFrame& Candidate : Snapshot.Frames)
		{
			if (Index >= Candidate.FirstSequence && Index < Candidate.FirstSequence + Candidate.EventCount)
			{
				Frame = Candidate.FrameIndex;
				break;
			}
		}
		Test.AddInfo(FString::Printf(
			TEXT("event[%d] seq=%d kind=%d key=%s delta=(%.2f,%.2f) frame=%d"),
			Index, Event.Sequence, static_cast<int32>(Event.Input.Kind),
			*Event.Input.Key.ToString(), Event.Input.Delta.X, Event.Input.Delta.Y, Frame));
	}
	for (const FCortexReplayFrame& Frame : Snapshot.Frames)
	{
		Test.AddInfo(FString::Printf(
			TEXT("frame[%d] appDelta=%.6f worldTick=%s realDelta=%.6f paused=%d first_sequence=%d event_count=%d"),
			Frame.FrameIndex, Frame.AppDeltaSeconds, Frame.WorldTick.IsSet() ? TEXT("yes") : TEXT("no"),
			Frame.WorldTick.IsSet() ? Frame.WorldTick->RealDeltaSeconds : 0.0f,
			Frame.WorldTick.IsSet() && Frame.WorldTick->bPaused ? 1 : 0,
			Frame.FirstSequence, Frame.EventCount));
	}
}

bool AnyMouseCapture(const TArray<FTickSample>& Sequence, int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Sequence[Index].bMouseCapture)
		{
			return true;
		}
	}
	return false;
}
} // namespace CortexReplayRoundTripTest

// Tests live in the same named namespace so unity builds never see these helpers next to the
// other test files' same-named anonymous helpers.
namespace CortexReplayRoundTripTest
{

// ---------------------------------------------------------------------------
// Audit C2 / PIF-06: a recording captured from real (non-synthetic) walk-and-look input replays to the
// same per-tick pawn location and ControlRotation. Capture records the raw-mouse stream and the held W
// through the Slate preprocessors and attributes them to engine frames at the observer input
// boundary; replay re-synthesizes the pointer motion (relative-move dispatch) at the same boundary of
// engine frame R+1+k, so the replayed pose matches the captured one frame by frame.
// This test MEASURES the round-trip: both legs are aligned on the first PIE tick whose end observes
// the capture in Recording / the run in Replaying (recorded frame 0) and compared tick by tick over
// the recording's FrameCount.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayRoundTripCapturedWalkAndLookTest,
	"Cortex.Replay.RoundTrip.CapturedWalkAndLookReplaysFrameExact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayRoundTripCapturedWalkAndLookTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FRoundTripState> State = MakeShared<FRoundTripState>();

	if (!QueueCaptureUntilScriptable(*this, State))
	{
		return false;
	}

	// Frame 0: acquire mouse capture like a human (click the viewport centre). Frames 5..40: W held.
	// Frames 10..50: raw mouse dx=+7. 60 scripted frames in total.
	constexpr int32 ScriptFrames = 60;
	QueueScriptStart(*this, State, ScriptFrames,
		[](FRoundTripState& S, int32 Frame)
		{
			if (Frame == 0)
			{
				S.LeftClick();
			}
			if (Frame == 5)
			{
				S.KeyDown(EKeys::W);
			}
			if (Frame == 41)
			{
				S.KeyUp(EKeys::W);
			}
			if (Frame >= 10 && Frame <= 50)
			{
				S.RawMouseMove(7, 0);
			}
		});
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("the scripted input to be injected"),
		[](FRoundTripState& S)
		{
			return S.bScriptDone && GFrameCounter >= S.LastScriptFrameCounter + TrailingFramesAfterScript;
		}, ScriptWatchdogSeconds));

	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, State,
		[](FAutomationTestBase& T, FRoundTripState& S)
		{
			S.bSampling = false;
			S.Flush(T);
			S.CaptureSamples = MoveTemp(S.Samples);
			S.Samples.Reset();
			// Same API the window's Stop button uses.
			const FCortexCommandResult Stopped = S.Service->StopCapture(false);
			T.TestTrue(TEXT("Stopping the capture succeeds"), Stopped.bSuccess);
			if (!Stopped.bSuccess)
			{
				S.bAbort = true;
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("the capture to publish and release"),
		[](FRoundTripState& S) { return IsCaptureIdle(*S.Service); }, CaptureReleaseWatchdogSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("the capture PIE to end"),
		[](FRoundTripState&) { return !HasAnyPieWorld(); }, TeardownWatchdogSeconds));

	// Replay leg set-up: load the published recording, derive the capture's span, start the replay.
	const TSharedRef<TArray<FTickSample>> CaptureSpan = MakeShared<TArray<FTickSample>>();
	const TSharedRef<int32> CaptureFrameCount = MakeShared<int32>(0);
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, State,
		[CaptureSpan, CaptureFrameCount](FAutomationTestBase& T, FRoundTripState& S)
		{
			FCortexReplayLibrary Library(S.Fixture->GetProjectRoot());
			TSharedPtr<const FCortexReplaySnapshot> Snapshot;
			const bool bLoaded = Library.Load(S.RecordingId, false, Snapshot).bSuccess && Snapshot.IsValid();
			T.TestTrue(TEXT("The capture published a loadable recording"), bLoaded);
			if (!bLoaded)
			{
				S.bAbort = true;
				return;
			}
			T.TestTrue(TEXT("The published capture carries input events"), Snapshot->Events.Num() > 0);
			T.AddInfo(FString::Printf(TEXT("Published recording %d: frames=%d events=%d frameCount=%d"),
				S.RecordingId, Snapshot->Frames.Num(), Snapshot->Events.Num(),
				Snapshot->Metadata.Timing.FrameCount));
			LogRecording(T, *Snapshot);
			S.Flush(T);

			// Format 3: frame k of the recording is consumed by the k-th PIE tick whose end observes the
			// operation already in Recording (capture) / Replaying (replay). The sampler gates on exactly
			// that state, so the capture sequence starts at recorded frame 0.
			const int32 FrameCount = Snapshot->Metadata.Timing.FrameCount;
			*CaptureFrameCount = FrameCount;
			T.TestEqual(TEXT("Timing.FrameCount matches the frame list"), FrameCount, Snapshot->Frames.Num());
			for (int32 Index = 0; Index < S.CaptureSamples.Num(); ++Index)
			{
				CaptureSpan->Add(S.CaptureSamples[Index]);
			}
			T.TestTrue(TEXT("The capture span covers the scripted frames"), CaptureSpan->Num() >= 50);

			S.Samples.Reset();
			S.RunId.Invalidate();
			S.bRunTerminalObserved = false;
			S.SampleGate = [&S]()
			{
				if (S.bRunTerminalObserved || !S.RunId.IsValid())
				{
					return false;
				}
				const FCortexCommandResult Run = S.Service->GetRun(S.RunId, false);
				if (!Run.bSuccess || !Run.Data.IsValid())
				{
					return false;
				}
				const FString RunState = Run.Data->GetStringField(TEXT("state"));
				if (IsTerminalRunState(RunState))
				{
					S.bRunTerminalObserved = true;
					return false;
				}
				return RunState == TEXT("Replaying");
			};
			S.bSampling = true;

			const FCortexCommandResult Replay = S.Service->StartReplay(S.RecordingId, ECortexReplayOrigin::Human);
			T.TestTrue(TEXT("The published recording replays"), Replay.bSuccess);
			if (!Replay.bSuccess || !Replay.Data.IsValid())
			{
				S.bAbort = true;
				return;
			}
			FGuid::Parse(Replay.Data->GetStringField(TEXT("run_id")), S.RunId);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("the replay run to reach a terminal state"),
		[](FRoundTripState& S)
		{
			if (S.bRunTerminalObserved)
			{
				return true;
			}
			const FCortexCommandResult Run = S.Service->GetRun(S.RunId, false);
			return Run.bSuccess && Run.Data.IsValid()
				&& IsTerminalRunState(Run.Data->GetStringField(TEXT("state")));
		}, ReplayWatchdogSeconds));

	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, State,
		[CaptureSpan, CaptureFrameCount](FAutomationTestBase& T, FRoundTripState& S)
		{
			S.bSampling = false;
			S.Flush(T);
			const FCortexCommandResult Run = S.Service->GetRun(S.RunId, false);
			T.TestEqual(TEXT("Replay state"),
				Run.Data.IsValid() ? Run.Data->GetStringField(TEXT("state")) : FString(),
				FString(TEXT("Completed")));

			const TArray<FTickSample>& Captured = *CaptureSpan;
			const TArray<FTickSample>& Replayed = S.Samples;
			const int32 Span = Captured.Num();
			T.AddInfo(FString::Printf(TEXT("Capture span=%d ticks, replay observed=%d ticks"),
				Span, Replayed.Num()));
			const int32 FrameCount = *CaptureFrameCount;
			T.TestEqual(TEXT("Replay consumed exactly the recorded frame count of ticks"),
				Replayed.Num(), FrameCount);
			T.TestTrue(TEXT("Capture leg observed at least the recorded frame count of ticks"),
				Span >= FrameCount);
			const int32 Compared = FMath::Min(FMath::Min(Span, Replayed.Num()), FrameCount);

			// Validity of the stimulus itself: the capture really walked, looked and held the mouse.
			T.TestTrue(TEXT("Capture leg: the viewport held mouse capture"), AnyMouseCapture(Captured, Span));
			T.TestTrue(TEXT("Capture leg: total yaw change exceeds 10 degrees"),
				TotalYawChange(Captured, Span) > 10.0);
			T.TestTrue(TEXT("Capture leg: pawn moved more than 50 cm"),
				MaxDistanceFromStart(Captured, Span) > 50.0);

			// Consumer-visible result: the replay did the same.
			T.TestTrue(TEXT("Replay leg: the viewport held mouse capture"), AnyMouseCapture(Replayed, Compared));
			T.TestTrue(TEXT("Replay leg: total yaw change exceeds 10 degrees"),
				TotalYawChange(Replayed, Compared) > 10.0);
			T.TestTrue(TEXT("Replay leg: pawn moved more than 50 cm"),
				MaxDistanceFromStart(Replayed, Compared) > 50.0);

			int32 FirstDivergence = INDEX_NONE;
			double MaxYawError = 0.0;
			double MaxLocationError = 0.0;
			for (int32 Index = 0; Index < Compared; ++Index)
			{
				const double YawError = FMath::Abs(FMath::FindDeltaAngleDegrees(
					Captured[Index].Control.Yaw, Replayed[Index].Control.Yaw));
				const double LocationError = (Captured[Index].Location - Replayed[Index].Location).Size();
				MaxYawError = FMath::Max(MaxYawError, YawError);
				MaxLocationError = FMath::Max(MaxLocationError, LocationError);
				if (FirstDivergence == INDEX_NONE
					&& (YawError > YawToleranceDegrees || LocationError > LocationToleranceCm))
				{
					FirstDivergence = Index;
				}
			}
			const bool bFrameExact = FirstDivergence == INDEX_NONE;
			T.TestTrue(FString::Printf(
				TEXT("Replay is frame exact: per-tick yaw within %.0e deg and location within %.2f cm "
					"(first divergent tick %d, max yaw error %.4f deg, max location error %.4f cm)"),
				YawToleranceDegrees, LocationToleranceCm, FirstDivergence, MaxYawError, MaxLocationError),
				bFrameExact);
			// Always log both legs tick by tick (capture span first, then any extra replay ticks).
			for (int32 Index = 0; Index < FMath::Max(Span, Replayed.Num()); ++Index)
			{
				T.AddInfo(FString::Printf(TEXT("tick %d | capture %s | replay %s"), Index,
					Index < Span ? *DescribeSample(Captured[Index]) : TEXT("-"),
					Index < Replayed.Num() ? *DescribeSample(Replayed[Index]) : TEXT("-")));
			}
		}));

	QueueCleanup(*this, State);
	return true;
}

// ---------------------------------------------------------------------------
// Audit C1: ending PIE with the stock Escape chord during an owned capture publishes what was
// recorded. The Escape key-down is captured by the preprocessor and the session ends before its
// key-up, so the capture ends with the stop-chord key held. HandleCaptureTargetLoss drops that
// stop-chord press (DropTrailingStopChordPress, because it lies in the final recorded frame) and the
// stream is publishable; a held non-stop key or an older stop-key hold would instead withhold it as
// INCOMPLETE_RECORDING. The published recording contains the walk and no Escape event.
//
// The Escape key-down is injected through the same Slate path a real press takes, so the capture
// preprocessor records it. The chord itself is not left to fire: the editor routes it through
// UEditorEngine::ProcessDebuggerCommands, bound to UGameViewportClient::OnGameViewportInputKey
// (PlayLevel.cpp:3092), into FPlayWorldCommands::GlobalPlayWorldActions (PlayLevel.cpp:1089-1094),
// where Escape is StopPlaySession (DebuggerCommands.cpp:358). Synthetic Slate input does not reach
// SGlobalPlayWorldActions in an owned floating PIE window, so this test performs the editor's own
// action for that chord, GEditor->RequestEndPlayMap(), in the same engine frame. The editor then
// ends PIE after that frame's play world tick (EditorEngine.cpp:2463,2504-2507), which is the end
// state a human's Escape press produces.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayRoundTripEscapeEndedCaptureTest,
	"Cortex.Replay.RoundTrip.EscapeEndedCapturePublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayRoundTripEscapeEndedCaptureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FRoundTripState> State = MakeShared<FRoundTripState>();

	if (!QueueCaptureUntilScriptable(*this, State))
	{
		return false;
	}

	// Frame 0: click to acquire mouse capture. Frames 5..8: a short W tap. Frame 20: the human
	// presses Escape (the engine's StopPlaySession chord); the editor's own end-play action for that
	// chord is performed in the same frame, and the key is released on the next frame.
	constexpr int32 ScriptFrames = 22;
	QueueScriptStart(*this, State, ScriptFrames,
		[](FRoundTripState& S, int32 Frame)
		{
			if (Frame == 0)
			{
				S.LeftClick();
			}
			if (Frame == 5)
			{
				S.KeyDown(EKeys::W);
			}
			if (Frame == 8)
			{
				S.KeyUp(EKeys::W);
			}
			if (Frame == 20)
			{
				// The human's keyboard focus is on the viewport when the chord is pressed.
				S.EnsureViewportFocus();
				S.KeyDown(EKeys::Escape);

				// The stock chord's own editor action, performed in the same engine frame as the injected
				// key-down (still inside this OnBeginFrame hook, before the frame's world tick). The editor
				// ends PIE later in the same frame, after the play world's tick.
				//
				// Why the chord is not left to fire itself: UEditorEngine::ProcessDebuggerCommands is bound
				// to UGameViewportClient::OnGameViewportInputKey (PlayLevel.cpp:3092) and routes the pressed
				// key to FPlayWorldCommands::GlobalPlayWorldActions (PlayLevel.cpp:1089-1094), where Escape is
				// the StopPlaySession chord (DebuggerCommands.cpp:358). That command list is owned by
				// SGlobalPlayWorldActions, and synthetic Slate input does not reach it in an owned floating
				// PIE window (the diagnostics show the key arriving at the PIE viewport client with
				// ignoreInput=0 while no end-play request is queued). The editor's own action for the chord,
				// RequestEndPlayMap, is therefore performed here so the capture sees exactly the end state a
				// real Escape press produces: the key is recorded, and PIE teardown follows after the frame's
				// play world tick (EditorEngine.cpp:2463,2504-2507).
				if (GEditor)
				{
					GEditor->RequestEndPlayMap();
				}
				S.Note(FString::Printf(
					TEXT("frame %llu: injected Escape and requested end play; endPlayQueued=%d"),
					GFrameCounter, GEditor && GEditor->ShouldEndPlayMap() ? 1 : 0));
			}
			if (Frame == 21)
			{
				S.Note(FString::Printf(TEXT("frame %llu: before Escape up: PIE exists=%d endPlayQueued=%d"),
					GFrameCounter, HasAnyPieWorld() ? 1 : 0, GEditor && GEditor->ShouldEndPlayMap() ? 1 : 0));
				S.KeyUp(EKeys::Escape);
			}
		});
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("the scripted input to be injected"),
		[](FRoundTripState& S) { return S.bScriptDone; }, ScriptWatchdogSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State,
		TEXT("the Escape-ended capture to publish and release"),
		[](FRoundTripState& S) { return IsCaptureIdle(*S.Service); }, CaptureReleaseWatchdogSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(FWait(this, State, TEXT("PIE to end"),
		[](FRoundTripState&) { return !HasAnyPieWorld(); }, TeardownWatchdogSeconds));

	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, State,
		[](FAutomationTestBase& T, FRoundTripState& S)
		{
			S.Flush(T);
			T.TestFalse(TEXT("Escape ended the PIE session"), HasAnyPieWorld());

			const FCortexCommandResult Last = S.Service->GetLastCaptureResult();
			T.TestTrue(TEXT("The terminal capture outcome is queryable"),
				Last.bSuccess && Last.Data.IsValid());
			if (Last.bSuccess && Last.Data.IsValid())
			{
				FString ErrorCode;
				const TSharedPtr<FJsonObject>* PublicationError = nullptr;
				if (Last.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
					&& PublicationError != nullptr && PublicationError->IsValid())
				{
					ErrorCode = (*PublicationError)->GetStringField(TEXT("code"));
				}
				else
				{
					T.AddInfo(TEXT("No publication_error on the last capture outcome"));
				}
				T.TestEqual(TEXT("The capture is not withheld as incomplete"), ErrorCode, FString());
				T.TestTrue(TEXT("The Escape-ended capture published"), Last.Data->GetBoolField(TEXT("published")));
			}

			const FCortexCommandResult Recording = S.Service->GetRecording(S.RecordingId, false);
			T.TestTrue(TEXT("The recording exists in the library"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("The published recording is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
			T.TestFalse(TEXT("The capture no longer owns the record"), S.Service->IsRecordInUse(S.RecordingId));

			FCortexReplayLibrary Library(S.Fixture->GetProjectRoot());
			TSharedPtr<const FCortexReplaySnapshot> Snapshot;
			const bool bLoaded = Library.Load(S.RecordingId, false, Snapshot).bSuccess && Snapshot.IsValid();
			T.TestTrue(TEXT("The published recording reloads"), bLoaded);
			if (!bLoaded)
			{
				return;
			}
			LogRecording(T, *Snapshot);
			int32 EscapeEvents = 0;
			int32 WEvents = 0;
			for (const FCortexReplayEvent& Event : Snapshot->Events)
			{
				EscapeEvents += Event.Input.Key == EKeys::Escape ? 1 : 0;
				WEvents += Event.Input.Key == EKeys::W ? 1 : 0;
			}
			T.TestEqual(TEXT("The published inputs contain no Escape event"), EscapeEvents, 0);
			T.TestEqual(TEXT("The published inputs contain the W tap (down and up)"), WEvents, 2);
		}));

	QueueCleanup(*this, State);
	return true;
}

} // namespace CortexReplayRoundTripTest
