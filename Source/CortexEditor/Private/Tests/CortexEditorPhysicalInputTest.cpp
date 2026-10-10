// CortexEditor-local physical input tests.
//
// These tests exercise the exact owned/borrowed target binding, the replacement/deadline
// semantics of owned PIE preparation and the original player-pose read/compare/restore
// surface. They intentionally never include CortexReplay private types; the shared value
// types and the session header are the only physical input contracts they consume.
//
// Task 3 extends this file with widget/input behavior; the fixture below already reserves
// the overlay/slider/capture fields that extension needs.

#include "Misc/AutomationTest.h"

#include "CortexEditorPhysicalInput.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexEditorEngineClockLease.h"
#include "CortexEditorEngineFrameObserver.h"
#include "HAL/PlatformProcess.h"
#include "KeyState.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "CortexTypes.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Input/DragAndDrop.h"
#include "Input/Events.h"
#include "InputKeyEventArgs.h"
#include "Misc/PackageName.h"
#include "PlayInEditorDataTypes.h"
#include "Slate/SceneViewport.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

#include <limits>

namespace
{
/** Watchdogs are harness-only failure reporting; product deadlines are asserted separately. */
constexpr double CortexPhysicalInputReadyWatchdogSeconds = 15.0;
constexpr double CortexPhysicalInputProductDeadlineWatchdogSeconds = 45.0;
constexpr double CortexPhysicalInputTeardownWatchdogSeconds = 15.0;

/**
 * Real drag-drop payload for the owned drag cleanup regression. It records the engine's own
 * cancellation callback, so the test can judge cleanup by the operation's observable lifecycle
 * rather than by session bookkeeping.
 */
class FCortexPhysicalInputTestDragDrop : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FCortexPhysicalInputTestDragDrop, FDragDropOperation)

	int32 DropCount = 0;
	bool bLastDropCancelled = false;

	virtual void OnDrop(bool bDropWasHandled, const FPointerEvent& /*MouseEvent*/) override
	{
		++DropCount;
		bLastDropCancelled = !bDropWasHandled;
	}
};

struct FCortexEditorPhysicalInputTestFixture
{
	TSharedRef<FCortexEditorPhysicalInputSession> Session = MakeShared<FCortexEditorPhysicalInputSession>();
	TWeakObjectPtr<UWorld> EditorWorldBefore;
	FString EditorMapBefore;
	TWeakObjectPtr<UWorld> PIEWorld;
	FString RequestedMap;
	bool bOriginalDirty = false;
	bool bReadySeen = false;
	FCortexCommandResult Ready;
	TSharedPtr<SWidget> Overlay;
	TSharedPtr<SSlider> Slider;
	TArray<FCortexEditorPhysicalInputEvent> Captured;
	TArray<FCortexEditorPhysicalInputCaptureContext> CapturedContexts;
	bool bMenuOpen = false;
	float SliderValue = 0.0f;

	// Extensions used by the owned/borrowed/deadline cases. Readiness must publish exactly
	// once per session, so the callback count is tracked in addition to the bool flag.
	int32 ReadyCallbackCount = 0;
	TSharedPtr<FCortexEditorPhysicalInputSession> BorrowedSession;
	TSharedPtr<FCortexEditorPhysicalInputSession> SuccessorSession;
	TWeakObjectPtr<UWorld> SuccessorWorld;
	bool bSuccessorReadySeen = false;
	FCortexCommandResult SuccessorReady;
	int32 SuccessorReadyCallbackCount = 0;

	// Task 3 extensions: capture/dispatch/UI observation state and foreign consumers.
	bool bCaptureArmed = false;
	/**
	 * Slate applies a reply's mouse capture only while the application is active; automation drives
	 * physical input while the editor window may not hold OS focus, so the harness normalizes that
	 * setting for its lifetime and restores the previous value here.
	 */
	bool bSavedInactiveInputHandling = false;
	TArray<double> CapturedTimes;
	int32 InterruptionCount = 0;
	FCortexCommandResult Interruption;
	TSharedPtr<SButton> ForeignButton;
	int32 ForeignButtonClicks = 0;
	TSharedPtr<SWidget> CapturingWidget;
	int32 CapturingWidgetUps = 0;
	TSharedPtr<SWidget> DeltaConsumer;
	FVector2D ConsumerDelta = FVector2D::ZeroVector;
	FVector2D ConsumerScreenPosition = FVector2D::ZeroVector;
	TArray<FVector2D> ConsumerDeltas;
	int32 ConsumerMoveCount = 0;
	FCortexEditorPhysicalInputUIObservation LastObservation;
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> CapturedIdentity;
	FVector2D CapturedLocalPosition = FVector2D::ZeroVector;
	FVector2D CapturedAbsolutePosition = FVector2D::ZeroVector;
	int32 CapturedPressIndex = INDEX_NONE;
	FVector2D CapturedSliderSize = FVector2D::ZeroVector;
	FVector2D CapturedSliderPosition = FVector2D::ZeroVector;
	// Fix-round-4 regression consumers: a real drag source and a real high-precision requester.
	TSharedPtr<SWidget> DragConsumer;
	bool bDragConsumerHighPrecision = false;
	TSharedPtr<FCortexPhysicalInputTestDragDrop> DragDropOperation;
	int32 DragDetectedCount = 0;
	TSharedPtr<SWidget> HighPrecisionConsumer;
	/** CR-08: a second runtime panel sharing the probe's root/control tags. */
	TSharedPtr<SWidget> DuplicateTaggedOverlay;

	// CR-03 replay-ownership regressions: an off-route foreign keyboard consumer in its own window.
	TSharedPtr<SWidget> ForeignKeyConsumer;
	TSharedPtr<SWindow> ForeignKeyHostWindow;
	int32 ForeignKeyCount = 0;
	FKey ForeignLastKey;

	FCortexEditorPhysicalInputTestFixture()
	{
#if WITH_DEV_AUTOMATION_TESTS
		// Deterministic automation: override the physical-key snapshot to neutral so admission
		// never depends on ambient host keyboard input. The real GetAsyncKeyState snapshot remains
		// the production default; the pre-installation regression drives it explicitly.
		FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
			[](const FKey&) { return false; });
#endif
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			bSavedInactiveInputHandling = Slate.GetHandleDeviceInputWhenApplicationNotActive();
			Slate.SetHandleDeviceInputWhenApplicationNotActive(true);
		}
		EditorWorldBefore = GEditor->GetEditorWorldContext().World();
		UWorld* World = EditorWorldBefore.Get();
		if (World)
		{
			bOriginalDirty = World->GetPackage()->IsDirty();
			World->GetPackage()->SetDirtyFlag(true);
			const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
			EditorMapBefore = Current;
			RequestedMap = Current == TEXT("/Game/Maps/TestMap")
				? TEXT("/Game/Maps/TestRoomMap") : TEXT("/Game/Maps/TestMap");
		}
	}
	~FCortexEditorPhysicalInputTestFixture()
	{
		Session->Shutdown();
		if (BorrowedSession.IsValid())
		{
			BorrowedSession->Shutdown();
		}
		if (SuccessorSession.IsValid())
		{
			SuccessorSession->Shutdown();
		}
		if (UWorld* World = EditorWorldBefore.Get())
		{
			World->GetPackage()->SetDirtyFlag(bOriginalDirty);
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
		}
#if WITH_DEV_AUTOMATION_TESTS
		FCortexEditorPhysicalInputSession::ClearPhysicalKeySnapshotResolver();
#endif
	}
};

// ---------------------------------------------------------------------------
// Preparation gates
//
// Two owned-PIE preparations must remain genuinely unready for the product's whole
// 30-second deadline. Both helpers are test-only and act on the owned PIE world that the
// session itself created (never on an existing/external PIE session). They expose the
// same observable unavailability the product validates: a selected controller without a
// possessed pawn, and a world whose owned viewport is not registered with its client.
// ---------------------------------------------------------------------------
enum class ECortexPhysicalInputGateMode : uint8
{
	/** Keep the owned PIE controller from possessing a pawn until released. */
	Possession,

	/** Withhold the owned PIE viewport's registration with its viewport client until released. */
	Viewport
};

class FCortexPhysicalInputPreparationGate
{
public:
	explicit FCortexPhysicalInputPreparationGate(ECortexPhysicalInputGateMode InMode)
		: Mode(InMode)
	{
	}

	/** Record the PIE worlds that already exist so only the owned successor is gated. */
	void CaptureBaselineWorlds()
	{
		BaselineWorlds.Reset();
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World())
				{
					BaselineWorlds.Add(Context.World());
				}
			}
		}
	}

	/** Enforce the withheld prerequisite for the owned PIE world created after the baseline. */
	void Enforce()
	{
		if (bClosed)
		{
			UWorld* World = FindOwnedPIEWorld();
			if (World == nullptr)
			{
				return;
			}
			GatedWorld = World;
			if (Mode == ECortexPhysicalInputGateMode::Possession)
			{
				bPrerequisiteWithheld |= EnforceMissingPawn(World);
			}
			else
			{
				bPrerequisiteWithheld |= EnforceUnregisteredViewport(World);
			}
		}
	}

	/** True once the gate really withheld the owned prerequisite (not just found a world). */
	bool WasPrerequisiteWithheld() const { return bPrerequisiteWithheld; }

	/** Stop withholding and restore any viewport registration that was removed. */
	void Release()
	{
		if (!bClosed)
		{
			return;
		}
		bClosed = false;
		if (bSavedDefaultPawnClass)
		{
			if (UWorld* World = GatedWorld.Get())
			{
				if (AGameModeBase* GameMode = World->GetAuthGameMode())
				{
					GameMode->DefaultPawnClass = SavedDefaultPawnClass;
				}
			}
			bSavedDefaultPawnClass = false;
		}
		if (Mode == ECortexPhysicalInputGateMode::Viewport && WithheldViewportClient.IsValid() && WithheldSceneViewport != nullptr)
		{
			WithheldViewportClient->AddAssociation(*WithheldSceneViewport);
		}
	}

private:
	UWorld* FindOwnedPIEWorld()
	{
		if (GatedWorld.IsValid())
		{
			return GatedWorld.Get();
		}
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Context.WorldType == EWorldType::PIE && Candidate != nullptr && !BaselineWorlds.Contains(Candidate))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	bool EnforceMissingPawn(UWorld* World)
	{
		AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
		if (GameMode && !bSavedDefaultPawnClass)
		{
			SavedDefaultPawnClass = GameMode->DefaultPawnClass;
			bSavedDefaultPawnClass = true;
			GameMode->DefaultPawnClass = nullptr;
		}
		APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		if (Controller == nullptr)
		{
			return GameMode != nullptr;
		}
		if (APawn* Pawn = Controller->GetPawn())
		{
			Controller->UnPossess();
			Pawn->Destroy();
		}
		return true;
	}

	bool EnforceUnregisteredViewport(UWorld* World)
	{
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		if (Client == nullptr)
		{
			return false;
		}
		if (WithheldViewportClient.Get() != Client)
		{
			WithheldViewportClient = Client;
			WithheldSceneViewport = nullptr;
		}
		if (WithheldSceneViewport == nullptr)
		{
			// The owned viewport may not be registered yet on the first enforcing tick.
			WithheldSceneViewport = Client->GetGameViewport();
		}
		if (WithheldSceneViewport != nullptr && Client->GetGameViewport() == WithheldSceneViewport)
		{
			Client->RemoveAssociation(*WithheldSceneViewport);
		}
		return WithheldSceneViewport != nullptr;
	}

	ECortexPhysicalInputGateMode Mode;
	bool bClosed = true;
	bool bPrerequisiteWithheld = false;
	TSubclassOf<APawn> SavedDefaultPawnClass;
	bool bSavedDefaultPawnClass = false;
	TArray<TWeakObjectPtr<UWorld>> BaselineWorlds;
	TWeakObjectPtr<UWorld> GatedWorld;
	TWeakObjectPtr<UGameViewportClient> WithheldViewportClient;
	FSceneViewport* WithheldSceneViewport = nullptr;
};

// ---------------------------------------------------------------------------
// Latent commands
// ---------------------------------------------------------------------------

class FCortexWaitOwnedInputReady : public IAutomationLatentCommand
{
public:
	FCortexWaitOwnedInputReady(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}
	bool Update() override
	{
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds; }
		if (!Fixture->bReadySeen)
		{
			if (FPlatformTime::Seconds() <= Deadline) { return false; }
			Fixture->PIEWorld = Fixture->Session->GetTargetBinding().World;
			Test->AddError(TEXT("Owned input readiness timed out"));
			return true;
		}
		Test->TestTrue(TEXT("Owned target ready"), Fixture->Ready.bSuccess);
		if (Fixture->Ready.bSuccess)
		{
			Fixture->PIEWorld = Fixture->Session->GetTargetBinding().World;
			UWorld* PIE = Fixture->PIEWorld.Get();
			Test->TestNotNull(TEXT("Exact bound PIE world exists"), PIE);
			if (PIE)
			{
				Test->TestEqual(TEXT("Actual loaded map"), UWorld::RemovePIEPrefix(PIE->GetPackage()->GetName()), Fixture->RequestedMap);
			}
			UWorld* Before = Fixture->EditorWorldBefore.Get();
			Test->TestEqual(TEXT("Editor world preserved"), GEditor->GetEditorWorldContext().World(), Before);
			Test->TestTrue(TEXT("Unsaved dirty flag preserved"), Before && Before->GetPackage()->IsDirty());
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	double Deadline = 0.0;
};

class FCortexEndOwnedInputFixture : public IAutomationLatentCommand
{
public:
	FCortexEndOwnedInputFixture(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}
	bool Update() override
	{
		if (Deadline == 0.0)
		{
			if (!Fixture->PIEWorld.IsValid())
			{
				Fixture->PIEWorld = Fixture->Session->GetTargetBinding().World;
			}
			Fixture->Session->Shutdown();
			if (UWorld* World = Fixture->PIEWorld.Get())
			{
				if (Fixture->Overlay.IsValid() && World->GetGameViewport())
				{
					World->GetGameViewport()->RemoveViewportWidgetContent(Fixture->Overlay.ToSharedRef());
				}
			}
			Fixture->Overlay.Reset();
			Deadline = FPlatformTime::Seconds() + CortexPhysicalInputTeardownWatchdogSeconds;
		}
		if (!Fixture->PIEWorld.IsValid()) { return true; }
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World() == Fixture->PIEWorld.Get())
			{
				if (FPlatformTime::Seconds() <= Deadline) { return false; }
				Test->AddError(TEXT("Owned PIE teardown timed out"));
				return true;
			}
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	double Deadline = 0.0;
};

/** Runs one synchronous semantic check once the fixture-owned session is ready. */
class FCortexRunWhenOwnedInputReady : public IAutomationLatentCommand
{
public:
	FCortexRunWhenOwnedInputReady(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture,
		FString InLabel,
		TFunction<void(FAutomationTestBase&)> InAction)
		: Test(InTest)
		, Fixture(InFixture)
		, Label(MoveTemp(InLabel))
		, Action(MoveTemp(InAction)) {}
	bool Update() override
	{
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds; }
		if (!Fixture->bReadySeen)
		{
			if (FPlatformTime::Seconds() <= Deadline) { return false; }
			Test->AddError(FString::Printf(TEXT("%s: owned readiness never completed"), *Label));
			return true;
		}
		if (!Fixture->Ready.bSuccess)
		{
			Test->AddError(FString::Printf(TEXT("%s: owned readiness failed with %s"), *Label, *Fixture->Ready.ErrorCode));
			return true;
		}
		if (!bRan)
		{
			bRan = true;
			Action(*Test);
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	FString Label;
	TFunction<void(FAutomationTestBase&)> Action;
	bool bRan = false;
	double Deadline = 0.0;
};

/**
 * Keeps a real prerequisite withheld for the whole product preparation deadline, then
 * asserts the product failed through its own callback and tore down its owned PIE
 * context without any harness Shutdown().
 */
class FCortexAwaitOwnedInputPreparationTimeout : public IAutomationLatentCommand
{
public:
	FCortexAwaitOwnedInputPreparationTimeout(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture,
		TSharedRef<FCortexPhysicalInputPreparationGate> InGate)
		: Test(InTest), Fixture(InFixture), Gate(InGate) {}

	bool Update() override
	{
		if (WatchdogStart == 0.0) { WatchdogStart = FPlatformTime::Seconds(); }

		if (bClosed)
		{
			Gate->Enforce();
		}

		if (!Fixture->bReadySeen)
		{
			FCortexCommandResult TargetError;
			Test->TestFalse(TEXT("Withheld prerequisite never validates as ready"),
				Fixture->Session->ValidateTarget(TargetError));
			if (FPlatformTime::Seconds() - WatchdogStart <= CortexPhysicalInputProductDeadlineWatchdogSeconds)
			{
				return false;
			}
			if (bClosed) { Gate->Release(); bClosed = false; }
			Test->AddError(TEXT("Product preparation deadline did not fail within the 45s harness watchdog"));
			return true;
		}

		if (!bAssertedFailure)
		{
			bAssertedFailure = true;
			Test->TestFalse(TEXT("Withheld preparation failed"), Fixture->Ready.bSuccess);
			Test->TestEqual(TEXT("Preparation failure is INPUT_PREPARATION_TIMEOUT"),
				Fixture->Ready.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::PreparationTimeout));
			Test->TestEqual(TEXT("Readiness published exactly once"), Fixture->ReadyCallbackCount, 1);
			Test->TestTrue(TEXT("Withheld prerequisite was really observed"), Gate->WasPrerequisiteWithheld());
			if (bClosed) { Gate->Release(); bClosed = false; }
			ReleasedAt = FPlatformTime::Seconds();
			return false;
		}

		// The product must end its own owned PIE; a harness Shutdown() may not manufacture this.
		if (!Fixture->Session->IsOwnedPIEEnded())
		{
			if (FPlatformTime::Seconds() - ReleasedAt <= CortexPhysicalInputTeardownWatchdogSeconds) { return false; }
			Test->AddError(TEXT("Owned PIE context did not disappear after the preparation timeout"));
			return true;
		}
		Test->TestEqual(TEXT("Readiness still published exactly once after teardown"), Fixture->ReadyCallbackCount, 1);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	TSharedRef<FCortexPhysicalInputPreparationGate> Gate;
	double WatchdogStart = 0.0;
	double ReleasedAt = 0.0;
	bool bClosed = true;
	bool bAssertedFailure = false;
};

/**
 * After an expired preparation, starts a successor owned session with the gate opened.
 * The expired generation must not end or rebind the successor, and must not publish a
 * second readiness result. The old session is then shut down while the successor lives.
 */
class FCortexStartOwnedInputSuccessor : public IAutomationLatentCommand
{
public:
	FCortexStartOwnedInputSuccessor(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}

	bool Update() override
	{
		if (WatchdogStart == 0.0) { WatchdogStart = FPlatformTime::Seconds(); }

		if (Fixture->SuccessorSession == nullptr)
		{
			Fixture->SuccessorSession = MakeShared<FCortexEditorPhysicalInputSession>();
		}

		if (!bAdmitted)
		{
			const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
			const FCortexCommandResult Accepted = Fixture->SuccessorSession->BeginOwnedPIE(
				Fixture->RequestedMap, 0, [WeakFixture](const FCortexCommandResult& Ready)
				{
					if (const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = WeakFixture.Pin())
					{
						Pinned->SuccessorReady = Ready;
						Pinned->bSuccessorReadySeen = true;
						Pinned->SuccessorReadyCallbackCount++;
						Pinned->SuccessorWorld = Pinned->SuccessorSession->GetTargetBinding().World;
					}
				});
			if (Accepted.bSuccess)
			{
				bAdmitted = true;
				return false;
			}
			if (FPlatformTime::Seconds() - WatchdogStart <= CortexPhysicalInputReadyWatchdogSeconds)
			{
				return false; // A previous owned teardown may still be finishing; retry next frame.
			}
			Test->AddError(TEXT("Successor owned PIE preparation was never admitted"));
			return true;
		}

		if (!Fixture->bSuccessorReadySeen)
		{
			if (FPlatformTime::Seconds() - WatchdogStart <= CortexPhysicalInputReadyWatchdogSeconds + 15.0) { return false; }
			Test->AddError(TEXT("Successor owned input readiness timed out"));
			return true;
		}

		if (!bAssertedSuccessor)
		{
			bAssertedSuccessor = true;
			Test->TestTrue(TEXT("Successor owned target ready"), Fixture->SuccessorReady.bSuccess);
			Test->TestEqual(TEXT("Expired generation published no second result"), Fixture->ReadyCallbackCount, 1);
			Test->TestEqual(TEXT("Successor published exactly once"), Fixture->SuccessorReadyCallbackCount, 1);

			// The failed session must have reached its stable terminal state and must never report
			// the live successor context as its own (that adoption is what ended the successor and
			// crashed the editor).
			Test->TestTrue(TEXT("Failed session reached its stable terminal state"),
				Fixture->Session->IsOwnedPIEEnded());
			Test->TestTrue(TEXT("Failed session does not report the successor context as its own"),
				Fixture->Session->IsOwnedPIEEnded());

			UWorld* Successor = Fixture->SuccessorWorld.Get();
			Test->TestNotNull(TEXT("Successor world exists"), Successor);
			if (Successor && Fixture->PIEWorld.IsValid())
			{
				Test->TestTrue(TEXT("Successor world differs from the expired owned world"), Successor != Fixture->PIEWorld.Get());
			}

			// Shutting the expired session down must not end or unbind the successor.
			Fixture->Session->Shutdown();
			return false;
		}

		if (!bAssertedOldShutdownIsolation)
		{
			bAssertedOldShutdownIsolation = true;
			FCortexCommandResult SuccessorError;
			Test->TestTrue(TEXT("Successor target remains valid after the expired session Shutdown"),
				Fixture->SuccessorSession->ValidateTarget(SuccessorError));
			UWorld* Successor = Fixture->SuccessorSession->GetTargetBinding().World.Get();
			Test->TestNotNull(TEXT("Successor binding still holds its world"), Successor);
			bool bSuccessorStillInPIE = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == Successor)
				{
					bSuccessorStillInPIE = true;
					break;
				}
			}
			Test->TestTrue(TEXT("Expired session did not end the successor PIE"), bSuccessorStillInPIE);

			Fixture->SuccessorSession->EndOwnedPIE();
			TeardownAt = FPlatformTime::Seconds();
			return false;
		}

		if (!Fixture->SuccessorSession->IsOwnedPIEEnded())
		{
			if (FPlatformTime::Seconds() - TeardownAt <= CortexPhysicalInputTeardownWatchdogSeconds) { return false; }
			Test->AddError(TEXT("Successor owned PIE teardown timed out"));
			return true;
		}
		Test->TestEqual(TEXT("Successor still published exactly once"), Fixture->SuccessorReadyCallbackCount, 1);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	double WatchdogStart = 0.0;
	double TeardownAt = 0.0;
	bool bAdmitted = false;
	bool bAssertedSuccessor = false;
	bool bAssertedOldShutdownIsolation = false;
};

/** Asserts the editor package identity/dirty state survived an owned PIE run. */
class FCortexAssertEditorUnsavedStatePreserved : public IAutomationLatentCommand
{
public:
	FCortexAssertEditorUnsavedStatePreserved(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}
	bool Update() override
	{
		if (bRan) { return true; }
		bRan = true;
		UWorld* Before = Fixture->EditorWorldBefore.Get();
		Test->TestEqual(TEXT("Editor world identity preserved after owned PIE"),
			GEditor->GetEditorWorldContext().World(), Before);
		if (Before)
		{
			Test->TestTrue(TEXT("Unsaved dirty flag preserved after owned PIE"), Before->GetPackage()->IsDirty());
			Test->TestEqual(TEXT("Editor map path unchanged after owned PIE"),
				UWorld::RemovePIEPrefix(Before->GetPackage()->GetName()), Fixture->EditorMapBefore);
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	bool bRan = false;
};

/** True when a PIE world context exists for the given map asset path. */
bool IsPIEWorldContextPresentForMap(const FString& MapAssetPath)
{
	if (!GEngine)
	{
		return false;
	}
	const FString RequestedPackage = FPackageName::ObjectPathToPackageName(MapAssetPath);
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE || Context.World() == nullptr)
		{
			continue;
		}
		if (UWorld::RemovePIEPrefix(Context.World()->GetPackage()->GetName()) == RequestedPackage)
		{
			return true;
		}
	}
	return false;
}

bool HasAnyPIEWorldContext()
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

/** Runs one synchronous check in the next latent frame. */
class FCortexRunOnceCommand : public IAutomationLatentCommand
{
public:
	FCortexRunOnceCommand(FAutomationTestBase* InTest, TFunction<void(FAutomationTestBase&)> InAction)
		: Test(InTest), Action(MoveTemp(InAction)) {}
	bool Update() override
	{
		if (!bRan)
		{
			bRan = true;
			Action(*Test);
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TFunction<void(FAutomationTestBase&)> Action;
	bool bRan = false;
};

/** Waits for a PIE session started outside the bridge to begin playing. */
class FCortexWaitForExternalPIEPlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexWaitForExternalPIEPlaying(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (GEditor && GEditor->PlayWorld != nullptr
			&& GEditor->PlayWorld->HasBegunPlay()
			&& GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > CortexPhysicalInputReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("Timed out waiting for the external PIE session"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	double StartTime = 0.0;
};

/** Waits until no PIE world context remains. */
class FCortexWaitForNoPIEWorlds : public IAutomationLatentCommand
{
public:
	explicit FCortexWaitForNoPIEWorlds(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (!HasAnyPIEWorldContext())
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > CortexPhysicalInputReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("PIE contexts did not disappear in time"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	double StartTime = 0.0;
};

/** Waits for owned end resolution and asserts nothing owned is left running. */
class FCortexWaitForOwnedInputEnded : public IAutomationLatentCommand
{
public:
	FCortexWaitForOwnedInputEnded(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (!Fixture->Session->IsOwnedPIEEnded())
		{
			if (FPlatformTime::Seconds() - StartTime <= CortexPhysicalInputReadyWatchdogSeconds)
			{
				return false;
			}
			Test->AddError(TEXT("Owned PIE end never resolved"));
			return true;
		}
		Test->TestFalse(TEXT("No owned PIE world remains after end"),
			IsPIEWorldContextPresentForMap(Fixture->RequestedMap));
		Test->TestFalse(TEXT("No PIE session remains after end"), HasAnyPIEWorldContext());
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	double StartTime = 0.0;
};

/** Cancels any queued request and ends any PIE session, then waits for quiescence. */
class FCortexCleanupPIERequests : public IAutomationLatentCommand
{
public:
	explicit FCortexCleanupPIERequests(FAutomationTestBase* InTest) : Test(InTest) {}
	bool Update() override
	{
		if (Deadline == 0.0)
		{
			if (GEditor)
			{
				// Only a request that is still queued may be cancelled. CancelRequestPlaySession()
				// resets the engine's PlayInEditorSessionInfo (PlayLevel.cpp:1014-1019), and the
				// teardown of a session that has already started destroys its PIE window
				// (PlayLevel.cpp:786) whose close callback dereferences that session info
				// (PlayLevel.cpp:3672) and asserts when it is unset. Any started session is
				// therefore ended only through RequestEndPlayMap(), which resets the session info
				// itself after the window is gone (PlayLevel.cpp:667).
				const bool bSessionStarted = GEditor->IsPlayingSessionInEditor()
					|| GEditor->PlayWorld != nullptr
					|| HasAnyPIEWorldContext();
				if (bSessionStarted)
				{
					GEditor->RequestEndPlayMap();
				}
				else if (GEditor->IsPlaySessionRequestQueued())
				{
					// No session and therefore no PIE window can exist yet: cancelling is safe.
					GEditor->CancelRequestPlaySession();
				}
			}
			Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds;
		}
		if (!HasAnyPIEWorldContext())
		{
			return true;
		}
		if (FPlatformTime::Seconds() > Deadline)
		{
			Test->AddError(TEXT("PIE request cleanup timed out"));
			return true;
		}
		if (GEditor && !GEditor->ShouldEndPlayMap())
		{
			GEditor->RequestEndPlayMap();
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	double Deadline = 0.0;
};

/**
 * Holds the real possession gate until the owned PIE is running, externally ends that
 * context, and asserts the preparation fails on its own captured context.
 */
class FCortexLoseOwnedPreparationContext : public IAutomationLatentCommand
{
public:
	FCortexLoseOwnedPreparationContext(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture,
		TSharedRef<FCortexPhysicalInputPreparationGate> InGate)
		: Test(InTest), Fixture(InFixture), Gate(InGate) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }

		if (Phase == 0)
		{
			Gate->Enforce();
			if (GEditor && GEditor->PlayWorld != nullptr)
			{
				Phase = 1;
				PhaseDeadline = FPlatformTime::Seconds() + 2.0;
			}
			else if (FPlatformTime::Seconds() - StartTime > CortexPhysicalInputReadyWatchdogSeconds)
			{
				Test->AddError(TEXT("Owned PIE never started for the context-replacement case"));
				return true;
			}
			return false;
		}

		if (Phase == 1)
		{
			if (FPlatformTime::Seconds() >= PhaseDeadline)
			{
				// Externally end the unready owned PIE; the bridge must fail on its own context.
				if (GEditor) { GEditor->RequestEndPlayMap(); }
				Gate->Release();
				Phase = 2;
				PhaseDeadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds;
			}
			return false;
		}

		if (!Fixture->bReadySeen)
		{
			if (FPlatformTime::Seconds() <= PhaseDeadline) { return false; }
			Test->AddError(TEXT("Preparation did not fail after its owned context disappeared"));
			return true;
		}
		Test->TestFalse(TEXT("Preparation failed after its owned context disappeared"), Fixture->Ready.bSuccess);
		Test->TestEqual(TEXT("Failure published exactly once"), Fixture->ReadyCallbackCount, 1);
		Test->TestFalse(TEXT("Failed preparation holds no bound world"), Fixture->Session->GetTargetBinding().World.IsValid());
		Test->TestTrue(TEXT("Owned session reports its own context ended"), Fixture->Session->IsOwnedPIEEnded());
		return true;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	TSharedRef<FCortexPhysicalInputPreparationGate> Gate;
	double StartTime = 0.0;
	double PhaseDeadline = 0.0;
	int32 Phase = 0;
};

/** Builds an owned-readiness callback that records the first result exactly once. */
TFunction<void(const FCortexCommandResult&)> MakeFixtureReadyCallback(
	const TSharedRef<FCortexEditorPhysicalInputTestFixture>& Fixture)
{
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	return [WeakFixture](const FCortexCommandResult& Ready)
	{
		if (const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = WeakFixture.Pin())
		{
			Pinned->Ready = Ready;
			Pinned->bReadySeen = true;
			Pinned->ReadyCallbackCount++;
			if (Ready.bSuccess)
			{
				Pinned->PIEWorld = Pinned->Session->GetTargetBinding().World;
			}
		}
	};
}

/**
 * Simulates the engine's deferred PIE startup window, where the PIE context exists before
 * its world (PlayLevel.cpp:1827-1875 creates the context; :1603-1622 creates the world
 * later). The test consumes the owned request and detaches the owned world, so the session
 * must keep observing the world-less owned context instead of claiming completion, and must
 * end that context once its world is restored.
 */
class FCortexRestoreDeferredOwnedWorld : public IAutomationLatentCommand
{
public:
	FCortexRestoreDeferredOwnedWorld(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture,
		FName InContextHandle,
		UWorld* InDetachedWorld)
		: Test(InTest)
		, Fixture(InFixture)
		, ContextHandle(InContextHandle)
		, DetachedWorld(InDetachedWorld) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }

		if (!bAssertedDeferred)
		{
			bAssertedDeferred = true;
			// The request was consumed before the session's first poll and the owned context has
			// no world yet: the session must keep observing instead of claiming completion.
			Test->TestFalse(TEXT("Deferred owned startup is not declared complete without a world"),
				Fixture->Session->IsOwnedPIEEnded());

			// The deferred world now appears; the session must end the owned context.
			if (DetachedWorld != nullptr && GEngine)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE && Context.ContextHandle == ContextHandle)
					{
						// GetWorldContexts() exposes the engine-owned array const; the context
						// itself is mutable engine state, so re-attach through a const-cast.
						const_cast<FWorldContext&>(Context).SetCurrentWorld(DetachedWorld);
						DetachedWorld->RemoveFromRoot();
						bWorldRestored = true;
						break;
					}
				}
			}
			return false;
		}

		if (!Fixture->Session->IsOwnedPIEEnded())
		{
			if (FPlatformTime::Seconds() - StartTime <= CortexPhysicalInputTeardownWatchdogSeconds) { return false; }
			Test->AddError(TEXT("Owned PIE did not end after its deferred world appeared"));
			return true;
		}
		if (DetachedWorld != nullptr)
		{
			Test->TestTrue(TEXT("Detached owned world was restored for teardown"), bWorldRestored);
		}
		Test->TestFalse(TEXT("No owned PIE world remains after deferred teardown"),
			IsPIEWorldContextPresentForMap(Fixture->RequestedMap));
		Test->TestFalse(TEXT("No PIE session remains after deferred teardown"), HasAnyPIEWorldContext());
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	FName ContextHandle;
	UWorld* DetachedWorld = nullptr;
	double StartTime = 0.0;
	bool bAssertedDeferred = false;
	bool bWorldRestored = false;
};

// ---------------------------------------------------------------------------
// Task 3: consuming Slate probe, capture/dispatch drivers and foreign consumers
//
// The probe is the real rendered consumer the capture/dispatch tests observe: its menu is
// opened by a consuming M handler, its value only changes through genuine SSlider routing and
// its root/control carry distinct authored tags. No test invokes OnKeyDown, OnValueChanged or
// a capture handler directly.
// ---------------------------------------------------------------------------

class SCortexPhysicalInputProbe : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalInputProbe) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexEditorPhysicalInputTestFixture>, Fixture)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Fixture = Args._Fixture;
		const auto Pinned = Fixture.Pin();
		check(Pinned.IsValid());
		const TWeakPtr<FCortexEditorPhysicalInputTestFixture> Weak = Fixture;
		ChildSlot
		[
			SNew(SBox).WidthOverride(320.0f).HeightOverride(100.0f)
			[
				SAssignNew(Pinned->Slider, SSlider)
				.Value_Lambda([Weak]()
				{
					const auto F = Weak.Pin();
					return F ? F->SliderValue : 0.0f;
				})
				.Visibility_Lambda([Weak]()
				{
					const auto F = Weak.Pin();
					return F && F->bMenuOpen ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnValueChanged_Lambda([Weak](float Value)
				{
					if (const auto F = Weak.Pin()) { F->SliderValue = Value; }
				})
			]
		];
		SetTag(FName(TEXT("CortexPhysicalProbeRoot")));
		Pinned->Slider->SetTag(FName(TEXT("CortexPhysicalProbeSlider")));
	}
	bool SupportsKeyboardFocus() const override { return true; }
	FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
	{
		if (Event.GetKey() == EKeys::M)
		{
			if (const auto F = Fixture.Pin())
			{
				F->bMenuOpen = !F->bMenuOpen;
				return FReply::Handled();
			}
		}
		return FReply::Unhandled();
	}
private:
	TWeakPtr<FCortexEditorPhysicalInputTestFixture> Fixture;
};

/**
 * A foreign (non-owned) editor consumer for the cleanup regressions. It captures the pointer
 * on Down and records only its real matching Up, so cleanup must neither synthesize a click
 * nor steal its capture or cached button state.
 */
class SCortexPhysicalInputForeignButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalInputForeignButton) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexEditorPhysicalInputTestFixture>, Fixture)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Fixture = Args._Fixture;
		ChildSlot
		[
			SNew(SBox).WidthOverride(140.0f).HeightOverride(44.0f)
		];
		SetTag(FName(TEXT("CortexPhysicalForeignButton")));
	}
	// Real focusable Slate widget: SetUserFocus walks the path to the first widget that supports
	// keyboard focus, so this must be true for the foreign focus regression to establish focus on
	// this consumer rather than one of its ancestors.
	bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		return FReply::Unhandled();
	}
	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			if (const auto F = Fixture.Pin()) { F->ForeignButtonClicks++; }
			return FReply::Handled().ReleaseMouseCapture();
		}
		return FReply::Unhandled();
	}
private:
	TWeakPtr<FCortexEditorPhysicalInputTestFixture> Fixture;
};

/**
 * Foreign (non-owned) off-route keyboard consumer for the replay-ownership regressions. It
 * records every key-down it receives, so a synthetic replay key must never reach it once the
 * selected route no longer owns focus.
 */
class SCortexPhysicalForeignKeyConsumer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalForeignKeyConsumer) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexEditorPhysicalInputTestFixture>, Fixture)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Fixture = Args._Fixture;
		ChildSlot
		[
			SNew(SBox).WidthOverride(140.0f).HeightOverride(44.0f)
		];
		SetTag(FName(TEXT("CortexPhysicalForeignKeyConsumer")));
	}
	bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
	{
		if (const auto F = Fixture.Pin())
		{
			F->ForeignKeyCount++;
			F->ForeignLastKey = Event.GetKey();
		}
		return FReply::Handled();
	}
private:
	TWeakPtr<FCortexEditorPhysicalInputTestFixture> Fixture;
};

/**
 * Real replay consumer that reports the cursor delta it received through normal Slate routing, so a
 * replayed pointer move can be judged by what a widget actually saw rather than by the capture
 * callback.
 */
class SCortexPhysicalInputDeltaConsumer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalInputDeltaConsumer) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexEditorPhysicalInputTestFixture>, Fixture)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Fixture = Args._Fixture;
		ChildSlot
		[
			SNew(SBox).WidthOverride(320.0f).HeightOverride(140.0f)
		];
		SetTag(FName(TEXT("CortexPhysicalDeltaRoot")));
	}
	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& Event) override
	{
		if (const auto F = Fixture.Pin())
		{
			F->ConsumerDelta = Event.GetCursorDelta();
			F->ConsumerScreenPosition = Event.GetScreenSpacePosition();
			F->ConsumerDeltas.Add(Event.GetCursorDelta());
			F->ConsumerMoveCount++;
		}
		return FReply::Handled();
	}
private:
	TWeakPtr<FCortexEditorPhysicalInputTestFixture> Fixture;
};

/**
 * Real drag source for the owned drag/drop cleanup regression. A pointer down starts Slate drag
 * detection and the first move past the trigger distance establishes a real drag-drop operation
 * through this widget's normal OnDragDetected path.
 */
class SCortexPhysicalInputDragConsumer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalInputDragConsumer) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexEditorPhysicalInputTestFixture>, Fixture)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Fixture = Args._Fixture;
		ChildSlot
		[
			SNew(SBox).WidthOverride(320.0f).HeightOverride(100.0f)
		];
		SetTag(FName(TEXT("CortexPhysicalDragRoot")));
	}
	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			FReply Reply = FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
			// Opt-in: also request high-precision raw mouse movement so this owned dispatch acquires
			// Slate capture, OS capture and the raw-input mode together (as a real consumer can).
			if (const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = Fixture.Pin())
			{
				if (Pinned->bDragConsumerHighPrecision)
				{
					Reply.UseHighPrecisionMouseMovement(SharedThis(this));
				}
			}
			return Reply;
		}
		return FReply::Unhandled();
	}
	virtual FReply OnDragDetected(const FGeometry&, const FPointerEvent& Event) override
	{
		const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = Fixture.Pin();
		if (!Pinned.IsValid())
		{
			return FReply::Unhandled();
		}
		const TSharedRef<FCortexPhysicalInputTestDragDrop> Operation =
			MakeShared<FCortexPhysicalInputTestDragDrop>();
		Pinned->DragDropOperation = Operation;
		Pinned->DragDetectedCount++;
		return FReply::Handled().BeginDragDrop(Operation);
	}
private:
	TWeakPtr<FCortexEditorPhysicalInputTestFixture> Fixture;
};

/**
 * Real consumer that requests high-precision (raw input) mouse movement on its pointer down, so the
 * owned high-precision mode cleanup must release is genuinely established by the engine.
 */
class SCortexPhysicalInputHighPrecisionConsumer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexPhysicalInputHighPrecisionConsumer) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(SBox).WidthOverride(320.0f).HeightOverride(100.0f)
		];
		SetTag(FName(TEXT("CortexPhysicalHighPrecisionRoot")));
	}
	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			return FReply::Handled().UseHighPrecisionMouseMovement(SharedThis(this));
		}
		return FReply::Unhandled();
	}
};

/** Records every captured event, its monotonic capture time and its pre-press context. */
TFunction<void(const FCortexEditorPhysicalInputEvent&, double,
	const FCortexEditorPhysicalInputCaptureContext&)> MakeFixtureCaptureCallback(
		const TSharedRef<FCortexEditorPhysicalInputTestFixture>& Fixture)
{
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> Weak = Fixture;
	return [Weak](const FCortexEditorPhysicalInputEvent& Event, double TimeSeconds,
		const FCortexEditorPhysicalInputCaptureContext& Context)
	{
		if (const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = Weak.Pin())
		{
			Pinned->Captured.Add(Event);
			Pinned->CapturedTimes.Add(TimeSeconds);
			Pinned->CapturedContexts.Add(Context);
		}
	};
}

/** Adds the probe to the owned viewport, enables GameAndUI input and focuses the selected user. */
TSharedPtr<SWidget> InstallFixtureProbe(const TSharedRef<FCortexEditorPhysicalInputTestFixture>& Fixture,
	FCortexEditorPhysicalInputSession& Session)
{
	UWorld* World = Session.GetTargetBinding().World.Get();
	APlayerController* Controller = Session.GetTargetBinding().Controller.Get();
	if (!World || !Controller || !World->GetGameViewport())
	{
		return nullptr;
	}
	const TSharedRef<SCortexPhysicalInputProbe> Probe = SNew(SCortexPhysicalInputProbe).Fixture(Fixture);
	Fixture->Overlay = Probe;
	World->GetGameViewport()->AddViewportWidgetContent(Probe);
	Controller->SetShowMouseCursor(true);
	Controller->SetInputMode(FInputModeGameAndUI().SetWidgetToFocus(Probe).SetHideCursorDuringCapture(false));
	FSlateApplication::Get().SetUserFocus(Session.GetTargetBinding().SlateUserIndex, Probe, EFocusCause::SetDirectly);
	return Probe;
}

/** The window hosting the session's selected route widget (its viewport, else its input root). */
TSharedPtr<SWindow> ResolveSelectedRouteWindow(const FCortexEditorPhysicalInputSession& Session)
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	const FCortexEditorPhysicalInputTargetBinding& Binding = Session.GetTargetBinding();
	TSharedPtr<SWidget> RouteWidget = Binding.ViewportWidget.Pin();
	if (!RouteWidget.IsValid())
	{
		RouteWidget = Binding.InputRoot.Pin();
	}
	return RouteWidget.IsValid()
		? FSlateApplication::Get().FindWidgetWindow(RouteWidget.ToSharedRef()) : nullptr;
}

/**
 * Brings the session's selected route window to the front so it is the actually-active top-level
 * window. Replay ownership requires the actually-active route (CR-03); automation may leave another
 * window active, which is a test environment condition, never a product relaxation. Never mutates
 * shared Slate application activation state.
 */
void EnsureSelectedRouteWindowActive(const FCortexEditorPhysicalInputSession& Session)
{
	if (const TSharedPtr<SWindow> Window = ResolveSelectedRouteWindow(Session))
	{
		Window->BringToFront();
	}
}

/**
 * Installs the probe (optional), arms capture on a neutral target (optional), waits for real
 * probe geometry and then runs one semantic action on the Game Thread.
 */
class FCortexDrivePhysicalInput : public IAutomationLatentCommand
{
public:
	using FAction = TFunction<void(FAutomationTestBase&, FCortexEditorPhysicalInputTestFixture&)>;

	FCortexDrivePhysicalInput(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture,
		FString InLabel, FAction InAction,
		bool bInInstallProbe = true, bool bInArmCapture = true, bool bInOpenMenu = true)
		: Test(InTest)
		, Fixture(InFixture)
		, Label(MoveTemp(InLabel))
		, Action(MoveTemp(InAction))
		, bInstallProbe(bInInstallProbe)
		, bArmCapture(bInArmCapture)
		, bOpenMenu(bInOpenMenu) {}

	bool Update() override
	{
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds; }
		UWorld* World = Fixture->Session->GetTargetBinding().World.Get();
		APlayerController* Controller = Fixture->Session->GetTargetBinding().Controller.Get();
		if (!World || !Controller || !World->GetGameViewport())
		{
			if (FPlatformTime::Seconds() <= Deadline) { return false; }
			Test->AddError(FString::Printf(TEXT("%s: physical capture target disappeared"), *Label));
			return true;
		}
		if (Stage == 0)
		{
			if (bInstallProbe && !Fixture->Overlay.IsValid())
			{
				if (InstallFixtureProbe(Fixture, *Fixture->Session) == nullptr)
				{
					Test->AddError(FString::Printf(TEXT("%s: probe installation failed"), *Label));
					return true;
				}
			}
			if (bInstallProbe && bOpenMenu && !Fixture->bMenuOpen)
			{
				// Open the menu through the real consuming handler so the slider is hit-testable.
				// This setup input is issued before capture is armed, so it is never recorded.
				const auto& Binding = Fixture->Session->GetTargetBinding();
				const FModifierKeysState Modifiers;
				FSlateApplication::Get().ProcessKeyDownEvent(
					FKeyEvent(EKeys::M, Modifiers, Binding.InputDevice, false, 0, 0, Binding.SlateUserIndex));
				FSlateApplication::Get().ProcessKeyUpEvent(
					FKeyEvent(EKeys::M, Modifiers, Binding.InputDevice, false, 0, 0, Binding.SlateUserIndex));
			}
			if (bArmCapture && !Fixture->bCaptureArmed)
			{
				const FCortexCommandResult Armed =
					Fixture->Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
				Test->TestTrue(FString::Printf(TEXT("%s: neutral capture armed"), *Label), Armed.bSuccess);
				if (!Armed.bSuccess) { return true; }
				Fixture->bCaptureArmed = true;
			}
			Stage = 1;
			Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds;
			return false;
		}
		if (bInstallProbe && (!Fixture->Slider.IsValid()
			|| Fixture->Slider->GetCachedGeometry().GetLocalSize().X <= 0.0))
		{
			if (FPlatformTime::Seconds() <= Deadline) { return false; }
			Test->AddError(FString::Printf(TEXT("%s: probe geometry never became usable"), *Label));
			return true;
		}
		if (!bRan)
		{
			bRan = true;
			Action(*Test, *Fixture);
		}
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	FString Label;
	FAction Action;
	bool bInstallProbe = true;
	bool bArmCapture = true;
	bool bOpenMenu = true;
	bool bRan = false;
	int32 Stage = 0;
	double Deadline = 0.0;
};

/**
 * Retained native capture-and-drag driver: the consuming M press opens the probe menu, a real
 * pointer drag moves the rendered slider, and the recorded pointer stream is replayed through
 * normal Slate routing without being re-recorded.
 */
class FCortexDrivePhysicalCapture : public IAutomationLatentCommand
{
public:
	FCortexDrivePhysicalCapture(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(InFixture) {}
	bool Update() override
	{
		if (!Fixture->Ready.bSuccess) { return true; }
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + 15.0; }
		if (FPlatformTime::Seconds() > Deadline)
		{
			Test->AddError(TEXT("Rendered physical capture fixture timed out"));
			return true;
		}
		const auto& Binding = Fixture->Session->GetTargetBinding();
		UWorld* World = Binding.World.Get();
		APlayerController* Controller = Binding.Controller.Get();
		if (!World || !Controller || !World->GetGameViewport())
		{
			Test->AddError(TEXT("Exact physical capture target disappeared"));
			return true;
		}
		auto& Slate = FSlateApplication::Get();
		const FModifierKeysState Modifiers;
		if (Stage == 0)
		{
			const auto Probe = SNew(SCortexPhysicalInputProbe).Fixture(Fixture);
			Fixture->Overlay = Probe;
			World->GetGameViewport()->AddViewportWidgetContent(Probe);
			Controller->SetShowMouseCursor(true);
			Controller->SetInputMode(FInputModeGameAndUI().SetWidgetToFocus(Probe).SetHideCursorDuringCapture(false));
			Slate.SetUserFocus(Binding.SlateUserIndex, Probe, EFocusCause::SetDirectly);
			const TWeakPtr<FCortexEditorPhysicalInputTestFixture> Weak = Fixture;
			const auto Armed = Fixture->Session->SetCaptureCallback(
				[Weak](const FCortexEditorPhysicalInputEvent& Event, double,
					const FCortexEditorPhysicalInputCaptureContext& Context)
				{
					if (const auto F = Weak.Pin())
					{
						F->Captured.Add(Event);
						F->CapturedContexts.Add(Context);
					}
				});
			Test->TestTrue(TEXT("Neutral target capture armed"), Armed.bSuccess);
			if (!Armed.bSuccess) { return true; }
			Fixture->bCaptureArmed = true;
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::M, Modifiers, Binding.InputDevice, false, 0, 0, Binding.SlateUserIndex));
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::M, Modifiers, Binding.InputDevice, false, 0, 0, Binding.SlateUserIndex));
			Test->TestTrue(TEXT("Normal consuming handler opened menu"), Fixture->bMenuOpen);
			Stage = 1;
			return false;
		}
		const FGeometry Geometry = Fixture->Slider->GetCachedGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		if (Size.X <= 0.0 || Size.Y <= 0.0) { return false; }
		if (Stage == 2)
		{
			bool bReplayedMove = false;
			for (const auto& Event : PointerReplay)
			{
				Test->TestTrue(TEXT("Captured pointer event dispatched"), Fixture->Session->Dispatch(Event).bSuccess);
				if (Event.Kind == ECortexEditorPhysicalInputKind::PointerMove)
				{
					bReplayedMove = true;
					Test->TestTrue(TEXT("Bridge move changed slider before Up"), Fixture->SliderValue > 0.60f);
				}
			}
			Test->TestTrue(TEXT("Captured pointer move was replayed"), bReplayedMove);
			Test->TestTrue(TEXT("Bridge replay drag changed actual slider"), Fixture->SliderValue > 0.60f);
			Test->TestEqual(TEXT("Pointer replay was not re-recorded"), Fixture->Captured.Num(), BeforePointerReplay);
			return true;
		}
		const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.25, Size.Y * 0.5));
		const FVector2D Finish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.75, Size.Y * 0.5));
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		const int32 PointerBegin = Fixture->Captured.Num();
		const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
		Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer, Start, Start, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
		Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer, Finish, Start, Pressed, EKeys::Invalid, 0.0f, Modifiers, Binding.SlateUserIndex), false);
		Pressed.Reset();
		Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer, Finish, Finish, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
		Test->TestTrue(TEXT("Normal drag changed the rendered slider"), Fixture->SliderValue > 0.60f);

		// The guarded press boundary must carry a real pre-press pose and the exact tagged
		// runtime selector resolved from the actual hit route, not a callback echo.
		if (!Fixture->CapturedContexts.IsValidIndex(PointerBegin))
		{
			Test->AddError(TEXT("Pointer press context missing from capture"));
			return true;
		}
		const auto& PressContext = Fixture->CapturedContexts[PointerBegin];
		Test->TestTrue(TEXT("Press pose captured before the consumer"), PressContext.PressPose.IsSet());
		Test->TestEqual(TEXT("Tagged slider protection supported"),
			PressContext.UICoverage, ECortexEditorUICoverage::Supported);
		Test->TestTrue(TEXT("Portable target selector exists"), PressContext.UITarget.IsValid());
		if (PressContext.UITarget.IsValid())
		{
			Test->TestEqual(TEXT("Authored root tag"), PressContext.UITarget->RootTag,
				FString(TEXT("CortexPhysicalProbeRoot")));
			Test->TestEqual(TEXT("Authored control tag"), PressContext.UITarget->TargetTag,
				FString(TEXT("CortexPhysicalProbeSlider")));
		}

		if (Fixture->Captured.Num() < 2)
		{
			Test->AddError(TEXT("Consuming opening press/release missing from capture"));
			return true;
		}
		Test->TestEqual(TEXT("Opening press captured before consumer"), Fixture->Captured[0].Key, EKeys::M);
		Test->TestEqual(TEXT("Opening edge kind"), Fixture->Captured[0].Kind, ECortexEditorPhysicalInputKind::KeyDown);
		const int32 BeforeReplay = Fixture->Captured.Num();
		Test->TestTrue(TEXT("Opening press dispatched"), Fixture->Session->Dispatch(Fixture->Captured[0]).bSuccess);
		Test->TestTrue(TEXT("Opening release dispatched"), Fixture->Session->Dispatch(Fixture->Captured[1]).bSuccess);
		Test->TestFalse(TEXT("Normal replay consumer closed menu"), Fixture->bMenuOpen);
		Test->TestEqual(TEXT("Synthetic dispatch was not re-recorded"), Fixture->Captured.Num(), BeforeReplay);
		for (int32 Index = PointerBegin; Index < BeforeReplay; ++Index)
		{
			PointerReplay.Add(Fixture->Captured[Index]);
		}
		Fixture->SliderValue = 0.0f;
		Test->TestTrue(TEXT("Menu reopen press dispatched"), Fixture->Session->Dispatch(Fixture->Captured[0]).bSuccess);
		Test->TestTrue(TEXT("Menu reopen release dispatched"), Fixture->Session->Dispatch(Fixture->Captured[1]).bSuccess);
		Test->TestTrue(TEXT("Visible replay-drag phase reopened menu"), Fixture->bMenuOpen);
		BeforePointerReplay = Fixture->Captured.Num();
		Stage = 2;
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	int32 Stage = 0;
	TArray<FCortexEditorPhysicalInputEvent> PointerReplay;
	int32 BeforePointerReplay = 0;
	double Deadline = 0.0;
};

/** Waits a fixed number of frames, then runs one action once (layout settle after re-creation). */
class FCortexRunAfterFrames : public IAutomationLatentCommand
{
public:
	FCortexRunAfterFrames(FAutomationTestBase* InTest, int32 InFrames,
		TFunction<void(FAutomationTestBase&)> InAction)
		: Test(InTest), FramesRemaining(InFrames), Action(MoveTemp(InAction)) {}
	bool Update() override
	{
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds; }
		if (FramesRemaining > 0)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				Test->AddError(TEXT("Frame wait timed out"));
				return true;
			}
			--FramesRemaining;
			return false;
		}
		if (!bRan)
		{
			bRan = true;
			Action(*Test);
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	int32 FramesRemaining = 0;
	TFunction<void(FAutomationTestBase&)> Action;
	bool bRan = false;
	double Deadline = 0.0;
};

/** Captures the selected binding's controller for convenient assertions inside actions. */
APlayerController* FixtureController(FCortexEditorPhysicalInputTestFixture& Fixture)
{
	return Fixture.Session->GetTargetBinding().Controller.Get();
}

/** Converts a screen-space point into the selected viewport's local space (portable event space). */
FVector2D ToViewportLocal(const FCortexEditorPhysicalInputTestFixture& Fixture, const FVector2D& ScreenSpace)
{
	const TSharedPtr<SWidget> Viewport = Fixture.Session->GetTargetBinding().ViewportWidget.Pin();
	if (!Viewport.IsValid())
	{
		return ScreenSpace;
	}
	return Viewport->GetCachedGeometry().AbsoluteToLocal(ScreenSpace);
}

/** Converts a viewport-local point back into screen space (the inverse of ToViewportLocal). */
FVector2D ToViewportScreen(const FCortexEditorPhysicalInputTestFixture& Fixture, const FVector2D& ViewportLocal)
{
	const TSharedPtr<SWidget> Viewport = Fixture.Session->GetTargetBinding().ViewportWidget.Pin();
	if (!Viewport.IsValid())
	{
		return ViewportLocal;
	}
	return Viewport->GetCachedGeometry().LocalToAbsolute(ViewportLocal);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedMapTest,
	"Cortex.Editor.PhysicalInputOwnedMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedMapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	const FCortexCommandResult Accepted = Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, [WeakFixture](const FCortexCommandResult& Ready)
		{
			if (const auto Pinned = WeakFixture.Pin())
			{
				Pinned->Ready = Ready;
				Pinned->bReadySeen = true;
				Pinned->PIEWorld = Pinned->Session->GetTargetBinding().World;
			}
		});
	TestTrue(TEXT("PIE preparation admitted"), Accepted.bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Explicit selected world/local-player binding and rejection of other targets
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputBindingTest,
	"Cortex.Editor.PhysicalInputBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputBindingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture, TEXT("Binding"),
		[Fixture](FAutomationTestBase& Test)
		{
			UWorld* PIE = Fixture->PIEWorld.Get();
			UWorld* EditorWorld = Fixture->EditorWorldBefore.Get();
			Test.TestNotNull(TEXT("Owned PIE world available for binding"), PIE);
			Test.TestNotNull(TEXT("Editor world available for rejection"), EditorWorld);
			if (!PIE) { return; }

			const TSharedRef<FCortexEditorPhysicalInputSession> Bound =
				MakeShared<FCortexEditorPhysicalInputSession>();

			// Unrelated: an editor world is not the owned, selected PIE target.
			FCortexCommandResult Unrelated;
			if (EditorWorld)
			{
				Unrelated = Bound->BindTarget(*EditorWorld, 0);
				Test.TestFalse(TEXT("Unrelated editor world rejected"), Unrelated.bSuccess);
			}

			// Missing: local player indices outside the world's selected players.
			Test.TestFalse(TEXT("Missing local player rejected"),
				Bound->BindTarget(*PIE, 999).bSuccess);
			Test.TestFalse(TEXT("Negative local player rejected"),
				Bound->BindTarget(*PIE, -1).bSuccess);

			// Nothing was attached by any rejected attempt.
			FCortexCommandResult RejectedError;
			Test.TestFalse(TEXT("Rejected binding never validates"), Bound->ValidateTarget(RejectedError));
			Test.TestFalse(TEXT("Rejected binding holds no world"), Bound->GetTargetBinding().World.IsValid());

			// Explicit selected world/local-player binding succeeds and is usable.
			const FCortexCommandResult Selected = Bound->BindTarget(*PIE, 0);
			Test.TestTrue(TEXT("Explicit selected world/local player binds"), Selected.bSuccess);
			const FCortexEditorPhysicalInputTargetBinding& Binding = Bound->GetTargetBinding();
			Test.TestTrue(TEXT("Binding holds the exact PIE world"), Binding.World.Get() == PIE);
			Test.TestTrue(TEXT("Binding holds a controller"), Binding.Controller.IsValid());
			Test.TestTrue(TEXT("Binding holds the possessed pawn"), Binding.Pawn.IsValid());
			Test.TestEqual(TEXT("Binding records the selected local player"),
				Bound->GetTargetInfo().LocalPlayerIndex, 0);
			FCortexEditorPhysicalInputPlayerPose BoundPose;
			Test.TestTrue(TEXT("Bound target pose readable"), Bound->ReadPlayerPose(BoundPose).bSuccess);

			// A bound session does not silently switch to another world.
			FCortexCommandResult Ambiguous;
			if (EditorWorld)
			{
				Ambiguous = Bound->BindTarget(*EditorWorld, 0);
				Test.TestFalse(TEXT("Rebinding to another world rejected"), Ambiguous.bSuccess);
			}
			FCortexCommandResult StillValid;
			Test.TestTrue(TEXT("Original binding survives a rejected rebind"), Bound->ValidateTarget(StillValid));
			Test.TestTrue(TEXT("Original binding still holds the PIE world"), Bound->GetTargetBinding().World.Get() == PIE);

			Bound->Shutdown();
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Unsaved editor state is never saved or discarded while owned PIE runs another map
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputUnsavedStateTest,
	"Cortex.Editor.PhysicalInputUnsavedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputUnsavedStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("Editor world was dirty before owned PIE"), Fixture->EditorWorldBefore->GetPackage()->IsDirty());
	TestTrue(TEXT("Owned map differs from the editor map"),
		Fixture->RequestedMap != UWorld::RemovePIEPrefix(Fixture->EditorWorldBefore->GetPackage()->GetName()));
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexAssertEditorUnsavedStatePreserved(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Borrowed world: a second session reads the same pawn and its Shutdown is inert
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputBorrowedWorldTest,
	"Cortex.Editor.PhysicalInputBorrowedWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputBorrowedWorldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture, TEXT("BorrowedWorld"),
		[Fixture](FAutomationTestBase& Test)
		{
			UWorld* PIE = Fixture->PIEWorld.Get();
			Test.TestNotNull(TEXT("Owned PIE world for borrowing"), PIE);
			if (!PIE) { return; }

			FCortexEditorPhysicalInputPlayerPose OwnerPose;
			Test.TestTrue(TEXT("Owner pose readable"), Fixture->Session->ReadPlayerPose(OwnerPose).bSuccess);

			Fixture->BorrowedSession = MakeShared<FCortexEditorPhysicalInputSession>();
			const FCortexCommandResult Borrowed = Fixture->BorrowedSession->BindTarget(*PIE, 0);
			Test.TestTrue(TEXT("Borrowed session binds the same running world"), Borrowed.bSuccess);
			Test.TestTrue(TEXT("Borrowed binding matches the owner's pawn"),
				Fixture->BorrowedSession->GetTargetBinding().Pawn.Get() == Fixture->Session->GetTargetBinding().Pawn.Get());

			FCortexEditorPhysicalInputPlayerPose BorrowedPose;
			Test.TestTrue(TEXT("Borrowed pose readable"), Fixture->BorrowedSession->ReadPlayerPose(BorrowedPose).bSuccess);
			Test.TestTrue(TEXT("Borrowed pose matches the owner's pose"),
				FCortexEditorPhysicalInputSession::ComparePlayerPose(OwnerPose, BorrowedPose).bSuccess);

			// Shutting the borrowed session must not end the owned PIE or invalidate the owner.
			Fixture->BorrowedSession->Shutdown();

			FCortexCommandResult OwnerError;
			Test.TestTrue(TEXT("Owner still validates after borrowed Shutdown"), Fixture->Session->ValidateTarget(OwnerError));
			Test.TestTrue(TEXT("Owner still holds its PIE world"), Fixture->Session->GetTargetBinding().World.Get() == PIE);
			Test.TestTrue(TEXT("Owner still holds its pawn"), Fixture->Session->GetTargetBinding().Pawn.IsValid());
			bool bPIEPresent = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == PIE)
				{
					bPIEPresent = true;
					break;
				}
			}
			Test.TestTrue(TEXT("Owned PIE still running after borrowed Shutdown"), bPIEPresent);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Destroyed/replaced pawn: the session fails instead of following a successor
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPawnReplacedTest,
	"Cortex.Editor.PhysicalInputPawnReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPawnReplacedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture, TEXT("PawnReplaced"),
		[Fixture](FAutomationTestBase& Test)
		{
			UWorld* PIE = Fixture->PIEWorld.Get();
			APlayerController* Controller = Fixture->Session->GetTargetBinding().Controller.Get();
			APawn* OriginalPawn = Fixture->Session->GetTargetBinding().Pawn.Get();
			Test.TestNotNull(TEXT("Bound controller present"), Controller);
			Test.TestNotNull(TEXT("Bound pawn present"), OriginalPawn);
			if (!PIE || !Controller || !OriginalPawn) { return; }

			OriginalPawn->Destroy();
			Controller->UnPossess();

			FCortexCommandResult DestroyedError;
			Test.TestFalse(TEXT("Destroyed pawn invalidates the target"), Fixture->Session->ValidateTarget(DestroyedError));
			FCortexEditorPhysicalInputPlayerPose Pose;
			Test.TestFalse(TEXT("Destroyed pawn is not readable"), Fixture->Session->ReadPlayerPose(Pose).bSuccess);

			// A successor pawn possessed by the same controller must not be adopted.
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			APawn* Replacement = PIE->SpawnActor<APawn>(APawn::StaticClass(), FTransform::Identity, SpawnParameters);
			Test.TestNotNull(TEXT("Replacement pawn spawned"), Replacement);
			if (Replacement)
			{
				Controller->Possess(Replacement);
				Test.TestTrue(TEXT("Controller possesses the replacement"), Controller->GetPawn() == Replacement);
				FCortexCommandResult SuccessorError;
				Test.TestFalse(TEXT("Replaced pawn is not adopted as the target"),
					Fixture->Session->ValidateTarget(SuccessorError));
				Test.TestFalse(TEXT("Replaced pawn is not readable"), Fixture->Session->ReadPlayerPose(Pose).bSuccess);

				// The real Dispatch must fail on the destroyed original pawn instead of following
				// the successor, and must leave the successor controller key/consumer state unchanged.
				FCortexEditorPhysicalInputEvent PhysicalKeyDown;
				PhysicalKeyDown.Kind = ECortexEditorPhysicalInputKind::KeyDown;
				PhysicalKeyDown.Key = EKeys::W;
				const FCortexCommandResult Dispatched = Fixture->Session->Dispatch(PhysicalKeyDown);
				Test.TestFalse(TEXT("Dispatch on the replaced pawn fails"), Dispatched.bSuccess);
				Test.TestFalse(TEXT("Successor controller key state unchanged"),
					Controller->IsInputKeyDown(EKeys::W));
				Test.TestTrue(TEXT("Successor pawn still possesses the controller"),
					Controller->GetPawn() == Replacement);
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Initial pose restoration on the exact fixture pawn
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputRestorationTest,
	"Cortex.Editor.PhysicalInputRestoration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputRestorationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture, TEXT("Restoration"),
		[Fixture](FAutomationTestBase& Test)
		{
			APawn* Pawn = Fixture->Session->GetTargetBinding().Pawn.Get();
			APlayerController* Controller = Fixture->Session->GetTargetBinding().Controller.Get();
			Test.TestNotNull(TEXT("Fixture pawn present"), Pawn);
			Test.TestNotNull(TEXT("Fixture controller present"), Controller);
			if (!Pawn || !Controller) { return; }

			// Move to a chosen non-PlayerStart pose with an independent rotation and nonunit scale.
			const FTransform RecordedTransform(
				FRotator(11.0, 22.0, 33.0),
				FVector(1234.0, -567.0, 89.0),
				FVector(1.5, 2.5, 0.25));
			const FRotator RecordedControl(7.0, -14.0, 3.0);
			Pawn->SetActorTransform(RecordedTransform, false, nullptr, ETeleportType::TeleportPhysics);
			Controller->SetControlRotation(RecordedControl);

			FCortexEditorPhysicalInputPlayerPose SavedPose;
			Test.TestTrue(TEXT("Recorded pose readable before any input"),
				Fixture->Session->ReadPlayerPose(SavedPose).bSuccess);
			Test.TestTrue(TEXT("Recorded transform captured"),
				FCortexEditorPhysicalInputSession::ComparePlayerPose(
					FCortexEditorPhysicalInputPlayerPose{ RecordedTransform, RecordedControl }, SavedPose).bSuccess);

			// Perturb the same fields, then restore the recorded original pose.
			Pawn->SetActorTransform(FTransform(FRotator(90.0, 0.0, 0.0), FVector(10.0, 20.0, 30.0), FVector::OneVector),
				false, nullptr, ETeleportType::TeleportPhysics);
			Controller->SetControlRotation(FRotator(45.0, 45.0, 45.0));

			const FCortexCommandResult Restored = Fixture->Session->RestorePlayerPose(
				SavedPose, Fixture->Session->GetTargetInfo().PawnClassPath);
			Test.TestTrue(TEXT("Recorded pose restored"), Restored.bSuccess);

			FCortexEditorPhysicalInputPlayerPose Readback;
			Test.TestTrue(TEXT("Restored original pawn readable"),
				Fixture->Session->ReadPlayerPose(Readback).bSuccess);
			Test.TestTrue(TEXT("All restored pose fields within fixed bounds"),
				FCortexEditorPhysicalInputSession::ComparePlayerPose(SavedPose, Readback).bSuccess);

			// A recorded class that is not the pawn's class is a preparation error, not a fallback.
			const FCortexCommandResult Mismatched = Fixture->Session->RestorePlayerPose(
				SavedPose, TEXT("/Script/Engine.StaticMeshActor"));
			Test.TestFalse(TEXT("Mismatched recorded pawn class rejected"), Mismatched.bSuccess);
			Test.TestEqual(TEXT("Mismatched class is INITIAL_POSE_RESTORE_FAILED"),
				Mismatched.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed));

			// A non-finite recorded pose is rejected before any mutation.
			FCortexEditorPhysicalInputPlayerPose InvalidPose;
			InvalidPose.PawnTransform = FTransform::Identity;
			InvalidPose.PawnTransform.SetLocation(
				FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
			InvalidPose.ControlRotation = FRotator::ZeroRotator;
			const FCortexCommandResult InvalidRestore = Fixture->Session->RestorePlayerPose(
				InvalidPose, Fixture->Session->GetTargetInfo().PawnClassPath);
			Test.TestFalse(TEXT("Invalid recorded pose rejected"), InvalidRestore.bSuccess);
			Test.TestEqual(TEXT("Invalid recorded pose is INITIAL_POSE_RESTORE_FAILED"),
				InvalidRestore.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed));
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Production preparation deadline: withheld pawn possession
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPreparationTimeoutPossessionTest,
	"Cortex.Editor.PhysicalInputPreparationTimeoutPossession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPreparationTimeoutPossessionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	const TSharedRef<FCortexPhysicalInputPreparationGate> Gate =
		MakeShared<FCortexPhysicalInputPreparationGate>(ECortexPhysicalInputGateMode::Possession);
	Gate->CaptureBaselineWorlds();

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexAwaitOwnedInputPreparationTimeout(this, Fixture, Gate));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexStartOwnedInputSuccessor(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Production preparation deadline: withheld owned viewport registration
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPreparationTimeoutViewportTest,
	"Cortex.Editor.PhysicalInputPreparationTimeoutViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPreparationTimeoutViewportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	const TSharedRef<FCortexPhysicalInputPreparationGate> Gate =
		MakeShared<FCortexPhysicalInputPreparationGate>(ECortexPhysicalInputGateMode::Viewport);
	Gate->CaptureBaselineWorlds();

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexAwaitOwnedInputPreparationTimeout(this, Fixture, Gate));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexStartOwnedInputSuccessor(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Pose comparator boundaries with plain data (no PIE)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputComparatorBoundariesTest,
	"Cortex.Editor.PhysicalInputComparatorBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputComparatorBoundariesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FTransform BaseTransform(FRotator(10.0, 20.0, 30.0), FVector(100.0, 200.0, 300.0), FVector(1.0, 1.0, 1.0));
	const FRotator BaseControl(5.0, -10.0, 15.0);
	FCortexEditorPhysicalInputPlayerPose Expected;
	Expected.PawnTransform = BaseTransform;
	Expected.ControlRotation = BaseControl;

	TestTrue(TEXT("Identical poses compare equal"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Expected).bSuccess);

	// Position tolerance: 0.5cm inclusive, 1cm rejected.
	FCortexEditorPhysicalInputPlayerPose Moved = Expected;
	Moved.PawnTransform.SetLocation(BaseTransform.GetLocation() + FVector(0.5, 0.0, 0.0));
	TestTrue(TEXT("Position delta at 0.5cm accepted"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Moved).bSuccess);

	Moved.PawnTransform.SetLocation(BaseTransform.GetLocation() + FVector(1.0, 0.0, 0.0));
	const FCortexCommandResult OneCm = FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Moved);
	TestFalse(TEXT("Position delta at 1cm rejected"), OneCm.bSuccess);
	TestEqual(TEXT("Position mismatch is INPUT_POSE_MISMATCH"),
		OneCm.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::PoseMismatch));

	// Quaternion wrap: a full revolution is equivalent to no rotation.
	FCortexEditorPhysicalInputPlayerPose Wrapped = Expected;
	Wrapped.PawnTransform.SetRotation(FQuat(BaseTransform.GetRotation() * FQuat(FRotator(0.0, 0.0, 360.0))));
	Wrapped.ControlRotation = BaseControl + FRotator(360.0, -360.0, 360.0);
	TestTrue(TEXT("Full-revolution wrap compares equal"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Wrapped).bSuccess);

	// 1 degree rotation deltas are rejected for both pawn and control rotation.
	FCortexEditorPhysicalInputPlayerPose RotatedPawn = Expected;
	RotatedPawn.PawnTransform.SetRotation(FQuat(BaseTransform.GetRotation() * FQuat(FRotator(0.0, 0.0, 1.0))));
	const FCortexCommandResult PawnDegree = FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, RotatedPawn);
	TestFalse(TEXT("1 degree pawn rotation rejected"), PawnDegree.bSuccess);
	TestEqual(TEXT("Pawn rotation mismatch is INPUT_POSE_MISMATCH"),
		PawnDegree.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::PoseMismatch));

	FCortexEditorPhysicalInputPlayerPose RotatedControl = Expected;
	RotatedControl.ControlRotation = BaseControl + FRotator(0.0, 0.0, 1.0);
	const FCortexCommandResult ControlDegree = FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, RotatedControl);
	TestFalse(TEXT("1 degree control rotation rejected"), ControlDegree.bSuccess);

	// Scale tolerance: 0.001 inclusive, 0.002 rejected on the maximum component difference.
	FCortexEditorPhysicalInputPlayerPose Scaled = Expected;
	Scaled.PawnTransform.SetScale3D(FVector(1.001, 1.0, 1.0));
	TestTrue(TEXT("Scale delta at 0.001 accepted"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Scaled).bSuccess);

	Scaled.PawnTransform.SetScale3D(FVector(1.002, 1.0, 1.0));
	TestFalse(TEXT("Scale delta at 0.002 rejected"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Scaled).bSuccess);

	// Non-finite and degenerate poses are rejected outright.
	FCortexEditorPhysicalInputPlayerPose NonFinite = Expected;
	NonFinite.PawnTransform.SetLocation(FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
	TestFalse(TEXT("Non-finite pose rejected"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, NonFinite).bSuccess);

	FCortexEditorPhysicalInputPlayerPose Degenerate = Expected;
	Degenerate.PawnTransform.SetRotation(FQuat(0.0, 0.0, 0.0, 0.0));
	TestFalse(TEXT("Degenerate quaternion rejected"),
		FCortexEditorPhysicalInputSession::ComparePlayerPose(Expected, Degenerate).bSuccess);

	return true;
}

// ---------------------------------------------------------------------------
// An unrelated PIE session or queued request is never consumed or ended by the bridge
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputExternalPIETest,
	"Cortex.Editor.PhysicalInputExternalPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputExternalPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForExternalPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this, [Fixture](FAutomationTestBase& Test)
	{
		UWorld* ExternalPlayWorld = GEditor ? GEditor->PlayWorld : nullptr;

		const FCortexCommandResult Rejected = Fixture->Session->BeginOwnedPIE(
			Fixture->RequestedMap, 0, [](const FCortexCommandResult&) {});
		Test.TestFalse(TEXT("Owned PIE rejected while an unrelated PIE runs"), Rejected.bSuccess);
		Test.TestEqual(TEXT("Rejection reports a busy editor"),
			Rejected.ErrorCode, FString(CortexErrorCodes::EditorBusy));

		Test.TestTrue(TEXT("External PIE is still running"), GEditor && GEditor->PlayWorld == ExternalPlayWorld);
		if (ExternalPlayWorld)
		{
			Test.TestTrue(TEXT("External PIE still has its world context"),
				IsPIEWorldContextPresentForMap(UWorld::RemovePIEPrefix(ExternalPlayWorld->GetPackage()->GetName())));
		}
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForNoPIEWorlds(this));
	return true;
}

// ---------------------------------------------------------------------------
// A same-map replacement of the queued request is never cancelled by the bridge
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputQueuedRequestReplacedTest,
	"Cortex.Editor.PhysicalInputQueuedRequestReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputQueuedRequestReplacedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	// The accepted request is queued but not started. The engine only stores it in the single
	// PlaySessionRequest optional (PlayLevel.cpp:985-988) and consumes it in the next
	// UEditorEngine::Tick (EditorEngine.cpp:2061-2064, "queued up during the last frame"), so at
	// this point no PIE world or PIE window exists and nothing has to be cancelled. A second
	// caller queuing its own request in this same invocation therefore overwrites ours verbatim.
	const TOptional<FRequestPlaySessionParams> OwnRequest = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Owned request is still queued before the replacement"), OwnRequest.IsSet());
	TestTrue(TEXT("Owned request selects an in-process destination viewport"),
		OwnRequest.IsSet() && OwnRequest->DestinationSlateViewport.IsSet());
	// The engine has not created a PIE world (and therefore no PIE window) yet, so overwriting the
	// queued request here cannot disturb any window bookkeeping.
	TestTrue(TEXT("No PIE play world exists while the request is still queued"), GEditor->PlayWorld == nullptr);
	TestFalse(TEXT("No PIE world context exists while the request is still queued"), HasAnyPIEWorldContext());

	FRequestPlaySessionParams Replacement;
	Replacement.SessionDestination = EPlaySessionDestinationType::InProcess;
	Replacement.WorldType = EPlaySessionWorldType::PlayInEditor;
	Replacement.GlobalMapOverride = Fixture->RequestedMap;
	// Keep the owned request's in-process destination viewport so the replacement never creates a
	// floating PIE window; only its placement differs from the request the bridge accepted.
	if (OwnRequest.IsSet())
	{
		Replacement.DestinationSlateViewport = OwnRequest->DestinationSlateViewport;
	}
	Replacement.StartLocation = FVector(1.0, 2.0, 3.0);
	GEditor->RequestPlaySession(Replacement);

	// Ending the bridge must relinquish its claim without cancelling the replacement.
	Fixture->Session->EndOwnedPIE();

	// Ownership is not resolved yet, so completion must not be claimed from an empty handle.
	TestFalse(TEXT("Bridge does not claim completion from an unresolved request"),
		Fixture->Session->IsOwnedPIEEnded());

	const TOptional<FRequestPlaySessionParams> Pending = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Bridge did not cancel the replacement request"), Pending.IsSet());
	if (Pending.IsSet())
	{
		TestEqual(TEXT("Replacement request identity is preserved"),
			Pending->GlobalMapOverride, Fixture->RequestedMap);
		TestTrue(TEXT("Replacement request keeps its own placement"), Pending->StartLocation.IsSet());
	}

	// The relinquishing bridge must not adopt the survivor as its own target either.
	FCortexCommandResult RelinquishError;
	TestFalse(TEXT("Relinquished bridge validates no target"),
		Fixture->Session->ValidateTarget(RelinquishError));
	TestFalse(TEXT("Relinquished bridge binds no world"),
		Fixture->Session->GetTargetBinding().World.IsValid());

	// The replacement is a real session the test caused; the cleanup ends it safely.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	return true;
}

// ---------------------------------------------------------------------------
// Preparation is pinned to its own context and never adopts or ends a successor
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPreparationContextReplacedTest,
	"Cortex.Editor.PhysicalInputPreparationContextReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPreparationContextReplacedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	const TSharedRef<FCortexPhysicalInputPreparationGate> Gate =
		MakeShared<FCortexPhysicalInputPreparationGate>(ECortexPhysicalInputGateMode::Possession);
	Gate->CaptureBaselineWorlds();

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexLoseOwnedPreparationContext(this, Fixture, Gate));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexStartOwnedInputSuccessor(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Ending during startup must not claim completion before startup is resolved
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputEndDuringStartupTest,
	"Cortex.Editor.PhysicalInputEndDuringStartup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputEndDuringStartupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	// End immediately: the engine may already own the request even though nothing is captured.
	Fixture->Session->EndOwnedPIE();
	TestFalse(TEXT("Completion is not claimed while owned startup is unresolved"),
		Fixture->Session->IsOwnedPIEEnded());

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForOwnedInputEnded(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Ending owned PIE invalidates the usable binding immediately
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputEndInvalidatesBindingTest,
	"Cortex.Editor.PhysicalInputEndInvalidatesBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputEndInvalidatesBindingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this, [Fixture](FAutomationTestBase& Test)
	{
		FCortexEditorPhysicalInputPlayerPose Pose;
		Test.TestTrue(TEXT("Pose readable before ending"), Fixture->Session->ReadPlayerPose(Pose).bSuccess);

		Fixture->Session->EndOwnedPIE();

		FCortexCommandResult EndError;
		Test.TestFalse(TEXT("Ending invalidates ValidateTarget"), Fixture->Session->ValidateTarget(EndError));
		Test.TestFalse(TEXT("Ending invalidates pose reads"), Fixture->Session->ReadPlayerPose(Pose).bSuccess);
		Test.TestFalse(TEXT("Ending releases the bound world"), Fixture->Session->GetTargetBinding().World.IsValid());
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForOwnedInputEnded(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A pawn that rejects its transform produces INITIAL_POSE_RESTORE_FAILED
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputImmutableRootTest,
	"Cortex.Editor.PhysicalInputImmutableRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputImmutableRootTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture, TEXT("ImmutableRoot"),
		[Fixture](FAutomationTestBase& Test)
		{
			APawn* Pawn = Fixture->Session->GetTargetBinding().Pawn.Get();
			Test.TestNotNull(TEXT("Fixture pawn present"), Pawn);
			if (!Pawn) { return; }

			FCortexEditorPhysicalInputPlayerPose Recorded;
			Test.TestTrue(TEXT("Pose readable before root removal"),
				Fixture->Session->ReadPlayerPose(Recorded).bSuccess);

			USceneComponent* OriginalRoot = Pawn->GetRootComponent();
			Test.TestNotNull(TEXT("Pawn has a root component"), OriginalRoot);
			Test.TestTrue(TEXT("Root removed for the immutable-root case"), Pawn->SetRootComponent(nullptr));

			// Differ in control rotation so the restore branch is entered and must mutate.
			Recorded.ControlRotation = Recorded.ControlRotation + FRotator(0.0, 5.0, 0.0);

			const FCortexCommandResult Failed = Fixture->Session->RestorePlayerPose(
				Recorded, Fixture->Session->GetTargetInfo().PawnClassPath);
			Test.TestFalse(TEXT("Restore fails when the pawn rejects the transform"), Failed.bSuccess);
			Test.TestEqual(TEXT("Immutable root reports INITIAL_POSE_RESTORE_FAILED"),
				Failed.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::InitialPoseRestoreFailed));

			if (OriginalRoot)
			{
				Pawn->SetRootComponent(OriginalRoot);
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A successor that differs only in DestinationSlateViewport is never cancelled
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputSuccessorViewportDiffersTest,
	"Cortex.Editor.PhysicalInputSuccessorViewportDiffers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputSuccessorViewportDiffersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	const TOptional<FRequestPlaySessionParams> OwnRequest = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Owned request is still queued before the replacement"), OwnRequest.IsSet());
	TestTrue(TEXT("Owned request selects an in-process destination viewport"),
		OwnRequest.IsSet() && OwnRequest->DestinationSlateViewport.IsSet());
	TestTrue(TEXT("No PIE play world exists while the request is still queued"), GEditor->PlayWorld == nullptr);

	// The successor differs from the accepted request ONLY in DestinationSlateViewport: it keeps
	// the same map and leaves StartLocation/StartRotation unset exactly like the owned request.
	// Its destination viewport is left unset, so the queued identity cannot be mistaken for ours.
	FRequestPlaySessionParams Replacement;
	Replacement.SessionDestination = EPlaySessionDestinationType::InProcess;
	Replacement.WorldType = EPlaySessionWorldType::PlayInEditor;
	Replacement.GlobalMapOverride = Fixture->RequestedMap;
	TestFalse(TEXT("Successor viewport really differs from the owned request"),
		Replacement.DestinationSlateViewport.IsSet());
	GEditor->RequestPlaySession(Replacement);

	// Ending the bridge must relinquish its claim without cancelling the successor.
	Fixture->Session->EndOwnedPIE();

	TestFalse(TEXT("Bridge does not claim completion from an unresolved request"),
		Fixture->Session->IsOwnedPIEEnded());

	const TOptional<FRequestPlaySessionParams> Pending = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Bridge did not cancel the viewport-only successor"), Pending.IsSet());
	if (Pending.IsSet())
	{
		TestEqual(TEXT("Successor request identity is preserved"),
			Pending->GlobalMapOverride, Fixture->RequestedMap);
		TestFalse(TEXT("Successor keeps its own destination viewport identity"),
			Pending->DestinationSlateViewport.IsSet());
	}

	// The relinquishing bridge must not adopt the survivor as its own target either.
	FCortexCommandResult RelinquishError;
	TestFalse(TEXT("Relinquished bridge validates no target"),
		Fixture->Session->ValidateTarget(RelinquishError));
	TestFalse(TEXT("Relinquished bridge binds no world"),
		Fixture->Session->GetTargetBinding().World.IsValid());

	// The successor is a real session the test caused; the cleanup ends it safely.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	return true;
}

// ---------------------------------------------------------------------------
// A request consumed before the first poll, with a deferred-startup (world-less) context
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputDeferredStartupTest,
	"Cortex.Editor.PhysicalInputDeferredStartup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputDeferredStartupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	// Consume the queued request in this invocation so the session cannot observe it while it is
	// still merely queued; the engine only consumes it in UEditorEngine::Tick otherwise.
	GEditor->StartQueuedPlaySessionRequest();

	// Locate the owned PIE context the engine created for the consumed request.
	UWorld* DetachedWorld = nullptr;
	FName ContextHandle = NAME_None;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		UWorld* ContextWorld = Context.World();
		if (ContextWorld != nullptr
			&& UWorld::RemovePIEPrefix(ContextWorld->GetPackage()->GetName()) != Fixture->RequestedMap)
		{
			continue;
		}
		ContextHandle = Context.ContextHandle;
		DetachedWorld = ContextWorld;
		break;
	}
	TestTrue(TEXT("Owned PIE context exists after the request is consumed"), ContextHandle != NAME_None);
	if (ContextHandle == NAME_None) { return false; }

	// Reproduce the deferred-startup window: the owned PIE context exists but has no world yet.
	// The detached world is kept alive by the test until it is restored, which is exactly the
	// point at which the session must end its owned context.
	if (DetachedWorld != nullptr)
	{
		DetachedWorld->AddToRoot();
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.ContextHandle == ContextHandle)
			{
				// GetWorldContexts() exposes the engine-owned array const; the context itself is
				// mutable engine state, so detach through a const-cast.
				const_cast<FWorldContext&>(Context).SetCurrentWorld(nullptr);
				break;
			}
		}
	}

	Fixture->Session->EndOwnedPIE();

	ADD_LATENT_AUTOMATION_COMMAND(
		FCortexRestoreDeferredOwnedWorld(this, Fixture, ContextHandle, DetachedWorld));
	return true;
}

// ---------------------------------------------------------------------------
// Admission rejects unsupported play settings
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputUnsupportedSettingsTest,
	"Cortex.Editor.PhysicalInputUnsupportedSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputUnsupportedSettingsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	TestNotNull(TEXT("Play in editor settings available"), PlaySettings);
	if (!PlaySettings) { return false; }

	bool bOriginalRunUnderOneProcess = false;
	PlaySettings->GetRunUnderOneProcess(bOriginalRunUnderOneProcess);
	TestTrue(TEXT("Supported settings keep RunUnderOneProcess enabled"), bOriginalRunUnderOneProcess);

	// An owned in-process session must not be admitted without RunUnderOneProcess.
	PlaySettings->SetRunUnderOneProcess(false);
	const FCortexCommandResult Rejected = Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, [](const FCortexCommandResult&) {});
	PlaySettings->SetRunUnderOneProcess(bOriginalRunUnderOneProcess);

	TestFalse(TEXT("Owned PIE rejected without RunUnderOneProcess"), Rejected.bSuccess);
	TestEqual(TEXT("Unsupported settings report an invalid operation"),
		Rejected.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestFalse(TEXT("Rejected admission never binds a world"),
		Fixture->Session->GetTargetBinding().World.IsValid());
	return true;
}

// ---------------------------------------------------------------------------
// Rendered capture/dispatch: consuming key captured before the consumer and replayed
// without re-recording, plus a held-button drag that moves the real rendered slider
// (Review Focus #1).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCaptureAndDragTest,
	"Cortex.Editor.PhysicalInputCaptureAndDrag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCaptureAndDragTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> Weak = Fixture;
	const auto Accepted = Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0,
		[Weak](const FCortexCommandResult& Ready)
		{
			if (const auto F = Weak.Pin())
			{
				F->Ready = Ready;
				F->bReadySeen = true;
				F->PIEWorld = F->Session->GetTargetBinding().World;
			}
		});
	TestTrue(TEXT("Capture PIE preparation admitted"), Accepted.bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalCapture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Repeat/modifier and release ordering survive the input-mode change of the opening press
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputRepeatModifierOrderingTest,
	"Cortex.Editor.PhysicalInputRepeatModifierOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputRepeatModifierOrderingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("RepeatModifierOrdering"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FInputDeviceId Device = Binding.InputDevice;
			const FModifierKeysState NoModifiers;
			const FModifierKeysState ShiftDown(true, false, false, false, false, false, false, false, false);

			// A modifier transition, a modifier-held press, its repeat, then the mode change and
			// the matching releases. Non-consume paths only.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::LeftShift, ShiftDown, Device, false, 0, 0, User));
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, ShiftDown, Device, false, 0, 0, User));
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, ShiftDown, Device, true, 0, 0, User));
			APlayerController* Controller = FixtureController(F);
			if (Controller)
			{
				Controller->SetInputMode(FInputModeGameOnly());
			}
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::W, ShiftDown, Device, false, 0, 0, User));
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::LeftShift, NoModifiers, Device, false, 0, 0, User));

			TArray<int32> WIndices;
			for (int32 Index = 0; Index < F.Captured.Num(); ++Index)
			{
				if (F.Captured[Index].Key == EKeys::W)
				{
					WIndices.Add(Index);
				}
			}
			Test.TestEqual(TEXT("Exactly the three W edges were captured"), WIndices.Num(), 3);
			if (WIndices.Num() != 3) { return; }
			Test.TestEqual(TEXT("First W edge is a down"), F.Captured[WIndices[0]].Kind,
				ECortexEditorPhysicalInputKind::KeyDown);
			Test.TestFalse(TEXT("First W edge is not a repeat"), F.Captured[WIndices[0]].bRepeat);
			Test.TestTrue(TEXT("Modifier held across the press is captured"),
				F.Captured[WIndices[0]].Modifiers.IsShiftDown());
			Test.TestEqual(TEXT("Second W edge is the repeat"), F.Captured[WIndices[1]].Kind,
				ECortexEditorPhysicalInputKind::KeyDown);
			Test.TestTrue(TEXT("Repeat flag is preserved"), F.Captured[WIndices[1]].bRepeat);
			Test.TestEqual(TEXT("Third W edge is the release after the mode change"),
				F.Captured[WIndices[2]].Kind, ECortexEditorPhysicalInputKind::KeyUp);
			Test.TestTrue(TEXT("Release keeps the held modifier"),
				F.Captured[WIndices[2]].Modifiers.IsShiftDown());
			Test.TestTrue(TEXT("Recorded edges stay ordered"),
				WIndices[0] < WIndices[1] && WIndices[1] < WIndices[2]);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Absolute UI coordinates are preserved; relative gameplay motion is not an absolute UI hit
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputRelativeVersusAbsoluteTest,
	"Cortex.Editor.PhysicalInputRelativeVersusAbsolute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputRelativeVersusAbsoluteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("RelativeVersusAbsolute"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.2, Size.Y * 0.5));
			const FVector2D Finish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.8, Size.Y * 0.5));
			const int32 Begin = F.Captured.Num();
			TSet<FKey> Pressed;
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice,
				FSlateApplicationBase::CursorPointerIndex, Finish, Start, Pressed, EKeys::Invalid,
				0.0f, Modifiers, Binding.SlateUserIndex), false);

			Test.TestTrue(TEXT("Absolute pointer motion was captured"), F.Captured.Num() > Begin);
			if (F.Captured.Num() <= Begin) { return; }
			const auto& Move = F.Captured[Begin];
			Test.TestEqual(TEXT("Captured absolute motion kind"), Move.Kind,
				ECortexEditorPhysicalInputKind::PointerMove);
			Test.TestTrue(TEXT("Absolute motion preserves its explicit cursor delta"),
				Move.Delta.Equals(Finish - Start, 0.01));
			Test.TestTrue(TEXT("Absolute motion records a viewport position"),
				Move.ViewportPosition.SizeSquared() > 0.0);

			// Relative gameplay/camera motion is dispatched as relative movement: it must not be
			// applied as an absolute UI pointer position.
			FCortexEditorPhysicalInputEvent Relative;
			Relative.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
			Relative.Key = EKeys::MouseX;
			Relative.Delta = FVector2D(12.0, 0.0);
			const float Before = F.SliderValue;
			Test.TestTrue(TEXT("Relative motion dispatched"), F.Session->Dispatch(Relative).bSuccess);
			Test.TestEqual(TEXT("Relative camera motion is not an absolute UI coordinate"),
				F.SliderValue, Before);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Double-click and wheel are captured/replayed once with a balanced button state
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputDoubleClickWheelTest,
	"Cortex.Editor.PhysicalInputDoubleClickWheel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputDoubleClickWheelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("DoubleClickWheel"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Center = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));
			TSet<FKey> Pressed;

			// Wheel over the viewport: the real engine wheel edge must be captured exactly once.
			const int32 WheelBegin = F.Captured.Num();
			Slate.ProcessMouseWheelOrGestureEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, Pressed, EKeys::Invalid, 1.0f, Modifiers, Binding.SlateUserIndex), nullptr);
			int32 WheelIndex = INDEX_NONE;
			int32 WheelCount = 0;
			for (int32 Index = WheelBegin; Index < F.Captured.Num(); ++Index)
			{
				if (F.Captured[Index].Kind == ECortexEditorPhysicalInputKind::Wheel)
				{
					if (WheelIndex == INDEX_NONE) { WheelIndex = Index; }
					++WheelCount;
				}
			}
			Test.TestEqual(TEXT("Exactly one wheel edge recorded"), WheelCount, 1);
			if (WheelIndex != INDEX_NONE)
			{
				Test.TestTrue(TEXT("Wheel edge records its delta"), F.Captured[WheelIndex].WheelDelta != 0.0f);
				const int32 BeforeWheelReplay = F.Captured.Num();
				Test.TestTrue(TEXT("Captured wheel dispatched once"),
					F.Session->Dispatch(F.Captured[WheelIndex]).bSuccess);
				Test.TestEqual(TEXT("Wheel replay was not re-recorded"), F.Captured.Num(), BeforeWheelReplay);
			}

			// A double click is one classified press plus its own release. The human sequence must be
			// balanced, and the recorded pair is replayed together so no cached bit is left held.
			const int32 DoubleBegin = F.Captured.Num();
			TSet<FKey> DownPressed;
			DownPressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, DownPressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Slate.ProcessMouseButtonDoubleClickEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, DownPressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Test.TestEqual(TEXT("Balanced button state after the human double-click sequence"),
				Slate.GetPressedMouseButtons().Num(), 0);

			int32 DoubleIndex = INDEX_NONE;
			int32 DoubleCount = 0;
			for (int32 Index = DoubleBegin; Index < F.Captured.Num(); ++Index)
			{
				if (F.Captured[Index].Kind == ECortexEditorPhysicalInputKind::DoubleClick)
				{
					if (DoubleIndex == INDEX_NONE) { DoubleIndex = Index; }
					++DoubleCount;
				}
			}
			Test.TestEqual(TEXT("Double click captured exactly once"), DoubleCount, 1);
			if (DoubleIndex != INDEX_NONE)
			{
				// The classified DoubleClick press is always followed by its own release; replaying
				// the press alone would leave Slate's cached button bit set.
				Test.TestTrue(TEXT("Classified double click has a recorded release"),
					F.Captured.IsValidIndex(DoubleIndex + 1)
						&& F.Captured[DoubleIndex + 1].Kind == ECortexEditorPhysicalInputKind::PointerUp);
				const int32 BeforeDoubleReplay = F.Captured.Num();
				Test.TestTrue(TEXT("Double click press dispatched once"),
					F.Session->Dispatch(F.Captured[DoubleIndex]).bSuccess);
				if (F.Captured.IsValidIndex(DoubleIndex + 1))
				{
					Test.TestTrue(TEXT("Double click release dispatched once"),
						F.Session->Dispatch(F.Captured[DoubleIndex + 1]).bSuccess);
				}
				Test.TestEqual(TEXT("Double-click replay was not re-recorded"),
					F.Captured.Num(), BeforeDoubleReplay);
			}
			Test.TestEqual(TEXT("Balanced button state after the double-click replay"),
				Slate.GetPressedMouseButtons().Num(), 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// CR-01: real captured motion/relative-motion/wheel events carry the canonical portable
// mouse-axis keys, so a recording containing them is accepted, loadable and replayable.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPortableMotionKeysTest,
	"Cortex.Editor.PhysicalInputPortableMotionKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPortableMotionKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("PortableMotionKeys"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { Test.AddError(TEXT("Probe has no geometry")); return; }
			const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.2, Size.Y * 0.5));
			const FVector2D Finish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.8, Size.Y * 0.5));
			const TSet<FKey> Pressed;

			// Absolute pointer motion (high-precision off) records the canonical Mouse2D axis.
			const int32 MoveBegin = F.Captured.Num();
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer, Finish, Start,
				Pressed, EKeys::Invalid, 0.0f, Modifiers, User), false);
			Test.TestTrue(TEXT("Absolute motion captured"), F.Captured.Num() > MoveBegin);
			if (F.Captured.Num() <= MoveBegin) { return; }
			const FCortexEditorPhysicalInputEvent MoveEvent = F.Captured[MoveBegin];
			Test.TestEqual(TEXT("Absolute motion kind"), MoveEvent.Kind,
				ECortexEditorPhysicalInputKind::PointerMove);
			Test.TestEqual(TEXT("Absolute motion records the canonical Mouse2D key"),
				MoveEvent.Key, EKeys::Mouse2D);

			// Wheel records the canonical MouseWheelAxis key.
			const int32 WheelBegin = F.Captured.Num();
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			Slate.ProcessMouseWheelOrGestureEvent(FPointerEvent(Binding.InputDevice, Pointer, Center,
				Center, Pressed, EKeys::Invalid, 1.0f, Modifiers, User), nullptr);
			int32 WheelIndex = INDEX_NONE;
			for (int32 Index = WheelBegin; Index < F.Captured.Num(); ++Index)
			{
				if (F.Captured[Index].Kind == ECortexEditorPhysicalInputKind::Wheel)
				{
					WheelIndex = Index;
					break;
				}
			}
			Test.TestTrue(TEXT("Wheel captured"), WheelIndex != INDEX_NONE);
			if (WheelIndex == INDEX_NONE) { return; }
			Test.TestEqual(TEXT("Wheel records the canonical MouseWheelAxis key"),
				F.Captured[WheelIndex].Key, EKeys::MouseWheelAxis);

			Test.TestTrue(TEXT("Captured absolute motion replayed"),
				F.Session->Dispatch(MoveEvent).bSuccess);
			Test.TestTrue(TEXT("Captured wheel replayed"),
				F.Session->Dispatch(F.Captured[WheelIndex]).bSuccess);

			// Install the high-precision consumer for the relative-motion step; its geometry becomes
			// usable on a later frame.
			UWorld* World = Binding.World.Get();
			if (!World || !World->GetGameViewport()) { Test.AddError(TEXT("Viewport missing")); return; }
			const TSharedRef<SCortexPhysicalInputHighPrecisionConsumer> Consumer =
				SNew(SCortexPhysicalInputHighPrecisionConsumer);
			F.HighPrecisionConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const TSet<FKey> Pressed;
			const TSharedPtr<SWidget> Consumer = F.HighPrecisionConsumer;
			if (!Consumer.IsValid()) { Test.AddError(TEXT("High-precision consumer missing")); return; }
			const FGeometry ConsumerGeometry = Consumer->GetCachedGeometry();
			const FVector2D ConsumerSize = ConsumerGeometry.GetLocalSize();
			if (ConsumerSize.X <= 0.0)
			{
				Test.AddError(TEXT("High-precision consumer has no geometry"));
				return;
			}
			const FVector2D ConsumerCenter = ConsumerGeometry.LocalToAbsolute(ConsumerSize * 0.5);

			// Real relative (high-precision) motion records the canonical Mouse2D key.
			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = ToViewportLocal(F, ConsumerCenter);
			Test.TestTrue(TEXT("High-precision down dispatched"),
				F.Session->Dispatch(PointerDown).bSuccess);
			Test.TestTrue(TEXT("High-precision mouse movement active"),
				Slate.IsUsingHighPrecisionMouseMovment());

			const int32 RelativeBegin = F.Captured.Num();
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer,
				ConsumerCenter + FVector2D(8.0, 0.0), ConsumerCenter, Pressed, EKeys::Invalid, 0.0f,
				Modifiers, User), false);
			Test.TestTrue(TEXT("Relative motion captured"), F.Captured.Num() > RelativeBegin);
			if (F.Captured.Num() > RelativeBegin)
			{
				const FCortexEditorPhysicalInputEvent RelativeEvent = F.Captured[RelativeBegin];
				Test.TestEqual(TEXT("Relative motion kind"), RelativeEvent.Kind,
					ECortexEditorPhysicalInputKind::RelativeMove);
				Test.TestEqual(TEXT("Relative motion records the canonical Mouse2D key"),
					RelativeEvent.Key, EKeys::Mouse2D);
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Unrelated editor input is excluded; matching captured releases survive focus loss
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputUnrelatedAndPostFocusReleaseTest,
	"Cortex.Editor.PhysicalInputUnrelatedAndPostFocusRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputUnrelatedAndPostFocusReleaseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("UnrelatedAndPostFocusRelease"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FInputDeviceId Device = Binding.InputDevice;
			const FModifierKeysState Modifiers;
			const int32 Begin = F.Captured.Num();

			// Input attributed to another Slate user is unrelated editor input and excluded.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::K, Modifiers, Device, false, 0, 0, User + 1));
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::K, Modifiers, Device, false, 0, 0, User + 1));
			Test.TestEqual(TEXT("Unrelated-user input was excluded"), F.Captured.Num(), Begin);

			// A real selected-user down is captured.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, Modifiers, Device, false, 0, 0, User));
			Test.TestEqual(TEXT("Selected-user down captured"), F.Captured.Num(), Begin + 1);
			if (F.Captured.Num() <= Begin) { return; }
			Test.TestEqual(TEXT("Captured down is W"), F.Captured[Begin].Key, EKeys::W);

			// Focus leaves the viewport to an unrelated editor control; the matching release of an
			// already-captured press must still be recorded in order.
			const TSharedPtr<SButton> ForeignButton = SNew(SButton);
			F.ForeignButton = ForeignButton;
			if (UWorld* World = F.Session->GetTargetBinding().World.Get())
			{
				if (UGameViewportClient* ViewportClient = World->GetGameViewport())
				{
					ViewportClient->AddViewportWidgetContent(ForeignButton.ToSharedRef());
				}
			}
			Slate.SetUserFocus(User, ForeignButton, EFocusCause::SetDirectly);
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::W, Modifiers, Device, false, 0, 0, User));
			Test.TestEqual(TEXT("Matching release after focus loss captured"), F.Captured.Num(), Begin + 2);
			if (F.Captured.Num() > Begin + 1)
			{
				Test.TestEqual(TEXT("Focus-loss release kind"),
					F.Captured[Begin + 1].Kind, ECortexEditorPhysicalInputKind::KeyUp);
				Test.TestEqual(TEXT("Focus-loss release key"), F.Captured[Begin + 1].Key, EKeys::W);
			}
			Test.TestTrue(TEXT("Foreign editor focus was not stolen"),
				Slate.GetUserFocusedWidget(User).Get() == ForeignButton.Get());
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Replay never re-captures itself or counts as human interference; foreign input interrupts
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplaySelfCaptureAndInterruptionTest,
	"Cortex.Editor.PhysicalInputReplaySelfCaptureAndInterruption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplaySelfCaptureAndInterruptionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("ReplaySelfCaptureAndInterruption"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});
			const FCortexCommandResult Armed = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestTrue(TEXT("Capture armed for the self-capture case"), Armed.bSuccess);
			if (!Armed.bSuccess) { return; }
			F.bCaptureArmed = true;

			// Replay ownership is armed at epoch establishment, not at the first Dispatch.
			Test.TestTrue(TEXT("Replay epoch armed for unattended ownership"),
				F.Session->BeginReplayEpoch().bSuccess);

			// Synthetic replay must not be re-captured and must not be treated as human interference.
			const int32 Before = F.Captured.Num();
			FCortexEditorPhysicalInputEvent Synthetic;
			Synthetic.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			Synthetic.Key = EKeys::W;
			Test.TestTrue(TEXT("Synthetic replay input dispatched"), F.Session->Dispatch(Synthetic).bSuccess);
			Test.TestEqual(TEXT("Synthetic replay was not re-captured"), F.Captured.Num(), Before);
			Test.TestEqual(TEXT("Synthetic replay did not interrupt"), F.InterruptionCount, 0);

			// A foreign physical press on the selected user/device interrupts the unattended replay.
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::Q, Modifiers, Binding.InputDevice, false, 0, 0,
				Binding.SlateUserIndex));
			Test.TestEqual(TEXT("Foreign physical input interrupted the unattended replay"),
				F.InterruptionCount, 1);
			Test.TestFalse(TEXT("Interruption reports a non-success result"), F.Interruption.bSuccess);
		}, /*bInstallProbe=*/true, /*bArmCapture=*/false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A human-origin replay refuses to dispatch once route ownership is lost.
// Real-input interruption and the focus/window/route guarantee apply to every replay origin.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplayAttendedOwnershipTest,
	"Cortex.Editor.PhysicalInputReplayAttendedOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplayAttendedOwnershipTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("ReplayAttendedOwnership"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const int32 User = F.Session->GetTargetBinding().SlateUserIndex;
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});

			// A human-origin replay is attended, but route ownership is still enforced.
			Test.TestTrue(TEXT("Attended replay epoch armed before the first event"),
				F.Session->BeginReplayEpoch().bSuccess);

			// A human establishes foreign, off-route focus before the first recorded event.
			const TSharedRef<SCortexPhysicalForeignKeyConsumer> Foreign =
				SNew(SCortexPhysicalForeignKeyConsumer).Fixture(Fixture);
			F.ForeignKeyConsumer = Foreign;
			const TSharedRef<SWindow> HostWindow =
				SNew(SWindow).ClientSize(FVector2D(200.0f, 120.0f))[Foreign];
			F.ForeignKeyHostWindow = HostWindow;
			Slate.AddWindow(HostWindow);
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			Test.TestTrue(TEXT("Foreign off-route focus established before the first event"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);

			// The first recorded event must never reach the foreign consumer, attended or not.
			FCortexEditorPhysicalInputEvent Recorded;
			Recorded.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			Recorded.Key = EKeys::M;
			const FCortexCommandResult Dispatched = F.Session->Dispatch(Recorded);
			Test.TestFalse(TEXT("Attended dispatch refuses once the selected route lost ownership"),
				Dispatched.bSuccess);
			Test.TestEqual(TEXT("Route loss raised exactly one interruption"), F.InterruptionCount, 1);
			Test.TestEqual(TEXT("The foreign consumer received no synthetic key"), F.ForeignKeyCount, 0);
			Test.TestTrue(TEXT("Foreign focus was not stolen"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);

			Slate.RequestDestroyWindow(HostWindow);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// CR-03: with the replay epoch established (not at the first Dispatch), a foreign off-route
// focus established before the first recorded event is interference: the event is never
// delivered to the foreign consumer and the foreign focus is never stolen.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplayOwnershipBeforeFirstEventTest,
	"Cortex.Editor.PhysicalInputReplayOwnershipBeforeFirstEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplayOwnershipBeforeFirstEventTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("ReplayOwnershipBeforeFirstEvent"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const int32 User = F.Session->GetTargetBinding().SlateUserIndex;
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});

			// Replay ownership is armed at epoch establishment, before the first event.
			Test.TestTrue(TEXT("Replay epoch armed before the first event"),
				F.Session->BeginReplayEpoch().bSuccess);

			// A human establishes foreign, off-route focus before the first recorded event.
			const TSharedRef<SCortexPhysicalForeignKeyConsumer> Foreign =
				SNew(SCortexPhysicalForeignKeyConsumer).Fixture(Fixture);
			F.ForeignKeyConsumer = Foreign;
			const TSharedRef<SWindow> HostWindow =
				SNew(SWindow).ClientSize(FVector2D(200.0f, 120.0f))[Foreign];
			F.ForeignKeyHostWindow = HostWindow;
			Slate.AddWindow(HostWindow);
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			Test.TestTrue(TEXT("Foreign off-route focus established before the first event"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);

			// The first recorded event must never reach the foreign consumer.
			FCortexEditorPhysicalInputEvent Recorded;
			Recorded.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			Recorded.Key = EKeys::M;
			const FCortexCommandResult Dispatched = F.Session->Dispatch(Recorded);
			Test.TestFalse(TEXT("Dispatch refuses once the selected route lost ownership"),
				Dispatched.bSuccess);
			Test.TestEqual(TEXT("Route loss raised exactly one interruption"), F.InterruptionCount, 1);
			Test.TestEqual(TEXT("The foreign consumer received no synthetic key"), F.ForeignKeyCount, 0);
			Test.TestTrue(TEXT("Foreign focus was not stolen"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);

			Slate.RequestDestroyWindow(HostWindow);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// CR-03: programmatic focus loss between events is detected through the wait/idle transition,
// not only at the next dispatch: the run is interrupted with zero foreign-consumer effects and
// the foreign focus is unchanged.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplayOwnershipFocusLossBetweenEventsTest,
	"Cortex.Editor.PhysicalInputReplayOwnershipFocusLossBetweenEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplayOwnershipFocusLossBetweenEventsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ReplayOwnershipSetup"),
		[](FAutomationTestBase&, FCortexEditorPhysicalInputTestFixture&) {}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const int32 User = F.Session->GetTargetBinding().SlateUserIndex;
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});
			Test.TestTrue(TEXT("Replay epoch armed before the first event"),
				F.Session->BeginReplayEpoch().bSuccess);

			// The first event is dispatched while the selected route still owns focus.
			FCortexEditorPhysicalInputEvent First;
			First.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			First.Key = EKeys::W;
			Test.TestTrue(TEXT("First event dispatched while ownership is intact"),
				F.Session->Dispatch(First).bSuccess);
			Test.TestEqual(TEXT("No interruption while ownership is intact"), F.InterruptionCount, 0);

			// Programmatic focus loss to a foreign off-route consumer between events.
			const TSharedRef<SCortexPhysicalForeignKeyConsumer> Foreign =
				SNew(SCortexPhysicalForeignKeyConsumer).Fixture(Fixture);
			F.ForeignKeyConsumer = Foreign;
			const TSharedRef<SWindow> HostWindow =
				SNew(SWindow).ClientSize(FVector2D(200.0f, 120.0f))[Foreign];
			F.ForeignKeyHostWindow = HostWindow;
			Slate.AddWindow(HostWindow);
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			Test.TestTrue(TEXT("Foreign off-route focus established between events"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Fixture](FAutomationTestBase& Test)
		{
			const FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const int32 User = F.Session->GetTargetBinding().SlateUserIndex;
			Test.TestEqual(TEXT("Route ownership loss interrupted replay between events"),
				F.InterruptionCount, 1);
			Test.TestEqual(TEXT("The foreign consumer received no synthetic key"), F.ForeignKeyCount, 0);
			Test.TestTrue(TEXT("Foreign focus was not stolen"),
				Slate.GetUserFocusedWidget(User) == F.ForeignKeyConsumer);
			if (F.ForeignKeyHostWindow.IsValid())
			{
				Slate.RequestDestroyWindow(F.ForeignKeyHostWindow.ToSharedRef());
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Capture admission and edge policy: a pre-held key/button/modifier denies arming without
// touching the human's original state
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCaptureAdmissionTest,
	"Cortex.Editor.PhysicalInputCaptureAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCaptureAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("CaptureAdmission"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FInputDeviceId Device = Binding.InputDevice;
			const FModifierKeysState NoModifiers;

			// Pre-held W: denied with INVALID_OPERATION, and an immediate re-arm stays denied,
			// which proves the session did not forcibly release the human's key to admit itself.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, NoModifiers, Device, false, 0, 0, User));
			FCortexCommandResult Denied = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestFalse(TEXT("Pre-held W denies capture admission"), Denied.bSuccess);
			Test.TestEqual(TEXT("Pre-held W denial is INVALID_OPERATION"),
				Denied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			Test.TestFalse(TEXT("Pre-held W human state untouched (re-arm still denied)"),
				F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture)).bSuccess);
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::W, NoModifiers, Device, false, 0, 0, User));

			// Pre-held Shift modifier: denied and untouched.
			const FModifierKeysState ShiftDown(true, false, false, false, false, false, false, false, false);
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::LeftShift, ShiftDown, Device, false, 0, 0, User));
			Denied = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestFalse(TEXT("Pre-held Shift denies capture admission"), Denied.bSuccess);
			Test.TestEqual(TEXT("Pre-held Shift denial is INVALID_OPERATION"),
				Denied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			Test.TestFalse(TEXT("Pre-held Shift human state untouched (re-arm still denied)"),
				F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture)).bSuccess);
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::LeftShift, NoModifiers, Device, false, 0, 0, User));

			// A UI-consumed mouse press is still a held button: denied, Slate keeps it pressed, and
			// the human's button is not released by the denial.
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			const FVector2D Center = Size.X > 0.0
				? Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5))
				: FVector2D::ZeroVector;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Device,
				FSlateApplicationBase::CursorPointerIndex, Center, Center, Pressed,
				EKeys::LeftMouseButton, 0.0f, NoModifiers, User));
			Test.TestTrue(TEXT("UI-consumed press is really held by Slate"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));
			Denied = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestFalse(TEXT("UI-consumed press denies capture admission"), Denied.bSuccess);
			Test.TestEqual(TEXT("UI-consumed press denial is INVALID_OPERATION"),
				Denied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			Test.TestTrue(TEXT("UI-consumed press still held after denial"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Device,
				FSlateApplicationBase::CursorPointerIndex, Center, Center, Released,
				EKeys::LeftMouseButton, 0.0f, NoModifiers, User));

			// A neutral target is admitted.
			const FCortexCommandResult Armed = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestTrue(TEXT("Neutral target capture admitted"), Armed.bSuccess);
			F.bCaptureArmed = Armed.bSuccess;
		}, /*bInstallProbe=*/true, /*bArmCapture=*/false, /*bOpenMenu=*/true));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Cleanup regressions with real consumers: owned key/drag neutralization and target loss
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCleanupOwnedStateTest,
	"Cortex.Editor.PhysicalInputCleanupOwnedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCleanupOwnedStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("CleanupOwnedState"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			APlayerController* Controller = FixtureController(F);
			Test.TestNotNull(TEXT("Bound controller for cleanup"), Controller);
			if (!Controller) { return; }

			// An owned synthetic W down and an owned pointer drag are both live at cleanup time.
			FCortexEditorPhysicalInputEvent KeyDown;
			KeyDown.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			KeyDown.Key = EKeys::W;
			Test.TestTrue(TEXT("Owned W down dispatched"), F.Session->Dispatch(KeyDown).bSuccess);

			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			FVector2D DragStart = FVector2D::ZeroVector;
			FVector2D DragFinish = FVector2D::ZeroVector;
			if (Size.X > 0.0)
			{
				DragStart = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.2, Size.Y * 0.5));
				DragFinish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.7, Size.Y * 0.5));
				FCortexEditorPhysicalInputEvent PointerDown;
				PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
				PointerDown.Key = EKeys::LeftMouseButton;
				PointerDown.ViewportPosition = ToViewportLocal(F, DragStart);
				FCortexEditorPhysicalInputEvent PointerMove;
				PointerMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
				PointerMove.ViewportPosition = ToViewportLocal(F, DragFinish);
				PointerMove.Delta = DragFinish - DragStart;
				Test.TestTrue(TEXT("Owned pointer down dispatched"), F.Session->Dispatch(PointerDown).bSuccess);
				Test.TestTrue(TEXT("Owned pointer move dispatched"), F.Session->Dispatch(PointerMove).bSuccess);
				Test.TestTrue(TEXT("Owned drag moved the rendered slider"), F.SliderValue > 0.5f);
			}

			Test.TestFalse(TEXT("UI wait is unavailable while owned key/button state is held"),
				F.Session->CanWaitForUI());
			const float SliderValueAfterReplay = F.SliderValue;

			const FCortexCommandResult Released = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Owned key and drag cleanup succeeded"), Released.bSuccess);
			Test.TestFalse(TEXT("Original controller is not down after cleanup"),
				Controller->IsInputKeyDown(EKeys::W));
			Test.TestTrue(TEXT("UI wait is available again"), F.Session->CanWaitForUI());
			Test.TestEqual(TEXT("Owned cached button bit was cleared"),
				Slate.GetPressedMouseButtons().Num(), 0);

			// A cancelled operation must not keep dragging: later physical movement over the same
			// slider must no longer change its value.
			if (Size.X > 0.0)
			{
				const FVector2D LaterMove = DragFinish + FVector2D(9.0, 0.0);
				Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice,
					FSlateApplicationBase::CursorPointerIndex, LaterMove, DragFinish,
					TSet<FKey>(), EKeys::Invalid, 0.0f, FModifierKeysState(),
					TOptional<int32>(User)), false);
				Test.TestEqual(TEXT("Cancelled slider ignores later physical movement"),
					F.SliderValue, SliderValueAfterReplay);
			}

			// Repeated cleanup must not mutate the prior success result.
			const FCortexCommandResult Repeated = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Repeated cleanup stays successful"), Repeated.bSuccess);

			// Losing the original target reports INPUT_CLEANUP_FAILED instead of claiming success.
			APawn* Pawn = F.Session->GetTargetBinding().Pawn.Get();
			Test.TestNotNull(TEXT("Bound pawn for the lost-target cleanup"), Pawn);
			if (Pawn)
			{
				Pawn->Destroy();
				Controller->UnPossess();
				const FCortexCommandResult Lost = F.Session->ReleaseHeldInputs();
				Test.TestFalse(TEXT("Cleanup after target loss fails"), Lost.bSuccess);
				Test.TestEqual(TEXT("Lost-target cleanup is INPUT_CLEANUP_FAILED"), Lost.ErrorCode,
					FString(CortexEditorPhysicalInputErrorCodes::CleanupFailed));
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Cleanup must preserve foreign focus, a foreign held button, its capture and its real Up
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCleanupForeignStateTest,
	"Cortex.Editor.PhysicalInputCleanupForeignState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCleanupForeignStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("CleanupForeignState"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputForeignButton> Foreign =
				SNew(SCortexPhysicalInputForeignButton).Fixture(Fixture);
			F.CapturingWidget = Foreign;
			World->GetGameViewport()->AddViewportWidgetContent(Foreign);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;

			// An independent target binding acts as the successor observer for this cleanup.
			F.BorrowedSession = MakeShared<FCortexEditorPhysicalInputSession>();
			UWorld* World = Binding.World.Get();
			Test.TestTrue(TEXT("Independent successor binding succeeds"),
				World && F.BorrowedSession->BindTarget(*World, 0).bSuccess);
			APlayerController* SuccessorController =
				F.BorrowedSession->GetTargetBinding().Controller.Get();
			APawn* SuccessorPawn = F.BorrowedSession->GetTargetBinding().Pawn.Get();

			const TSharedPtr<SWidget> Foreign = F.CapturingWidget;
			Test.TestTrue(TEXT("Foreign consumer present"), Foreign.IsValid());
			if (!Foreign.IsValid()) { return; }

			// Establish the foreign focus only now that the consumer is laid out, and prove it is
			// foreign before cleanup so the later assertion really covers cleanup behaviour.
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			Test.TestTrue(TEXT("Foreign focus established before cleanup"),
				Slate.GetUserFocusedWidget(User).Get() == Foreign.Get());

			const FGeometry ForeignGeometry = Foreign->GetCachedGeometry();
			const FVector2D ForeignSize = ForeignGeometry.GetLocalSize();
			const FVector2D ForeignCenter = ForeignSize.X > 0.0
				? ForeignGeometry.LocalToAbsolute(FVector2D(ForeignSize.X * 0.5, ForeignSize.Y * 0.5))
				: FVector2D::ZeroVector;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				ForeignCenter, ForeignCenter, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestTrue(TEXT("Foreign press really captured the pointer"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));

			// Cleanup must not synthesize a click on the already-pressed foreign button, must not
			// steal its capture, and must leave its cached button bit intact.
			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Owned cleanup reports success"), Cleanup.bSuccess);
			Test.TestEqual(TEXT("Foreign button received no cleanup click"), F.ForeignButtonClicks, 0);
			Test.TestTrue(TEXT("Foreign cached button bit survived cleanup"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));
			Test.TestTrue(TEXT("Foreign pointer capture survived cleanup"),
				Slate.HasUserMouseCapture(User));
			Test.TestTrue(TEXT("Foreign focus survived cleanup"),
				Slate.GetUserFocusedWidget(User).Get() == Foreign.Get());

			// The foreign consumer's real Up is still usable and completes its capture normally.
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				ForeignCenter, ForeignCenter, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Foreign button received its real Up"), F.ForeignButtonClicks, 1);
			Test.TestEqual(TEXT("Foreign button state is balanced after its real Up"),
				Slate.GetPressedMouseButtons().Num(), 0);

			// The successor binding is unchanged by the original session's cleanup.
			FCortexCommandResult SuccessorError;
			Test.TestTrue(TEXT("Successor target still validates after cleanup"),
				F.BorrowedSession->ValidateTarget(SuccessorError));
			Test.TestTrue(TEXT("Successor controller unchanged after cleanup"),
				F.BorrowedSession->GetTargetBinding().Controller.Get() == SuccessorController);
			Test.TestTrue(TEXT("Successor pawn unchanged after cleanup"),
				F.BorrowedSession->GetTargetBinding().Pawn.Get() == SuccessorPawn);
			F.BorrowedSession->Shutdown();
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Guard geometry: a recreated tagged root resolves on the selected route and the observation
// reports the actual normalized position for the new geometry, not a recorded echo
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputGuardGeometryTest,
	"Cortex.Editor.PhysicalInputGuardGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputGuardGeometryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("GuardGeometryCapture"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Local(Size.X * 0.25, Size.Y * 0.5);
			const FVector2D Absolute = Geometry.LocalToAbsolute(Local);
			const int32 PressIndex = F.Captured.Num();
			F.CapturedPressIndex = PressIndex;
			F.CapturedAbsolutePosition = Absolute;
			F.CapturedSliderSize = Size;
			F.CapturedSliderPosition = Geometry.GetAbsolutePosition();
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));

			Test.TestTrue(TEXT("Guarded press captured"), F.Captured.Num() > PressIndex);
			if (F.Captured.Num() <= PressIndex || !F.CapturedContexts.IsValidIndex(PressIndex)) { return; }
			const auto& Context = F.CapturedContexts[PressIndex];
			Test.TestTrue(TEXT("Guard context carries a tagged identity"), Context.UITarget.IsValid());
			if (!Context.UITarget.IsValid()) { return; }
			F.CapturedIdentity = Context.UITarget;
			F.CapturedLocalPosition = Context.UILocalPosition;
			Test.TestEqual(TEXT("Guard root tag"), Context.UITarget->RootTag,
				FString(TEXT("CortexPhysicalProbeRoot")));
			Test.TestEqual(TEXT("Guard control tag"), Context.UITarget->TargetTag,
				FString(TEXT("CortexPhysicalProbeSlider")));

			FCortexEditorPhysicalInputUIObservation Observation;
			const FCortexCommandResult Observed = F.Session->ObserveUI(
				F.Captured[PressIndex], *Context.UITarget, Observation);
			Test.TestTrue(TEXT("ObserveUI succeeded for the captured press"), Observed.bSuccess);
			Test.TestEqual(TEXT("Captured press observation is ready"),
				Observation.State, ECortexEditorUIObservationState::Ready);
			Test.TestTrue(TEXT("Observed normalized X matches the real press"),
				FMath::Abs(Observation.LocalPosition.X
					- static_cast<float>(F.CapturedLocalPosition.X)) <= 0.005f);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("GuardGeometryRecreate"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			// A recreated tagged root that genuinely changes the control geometry replaces the
			// captured root. WidthOverride/HeightOverride only affect an auto-sized SBox's desired
			// size and are ignored under the viewport overlay's fill slot, so Padding is used: the
			// SBox still fills, but the slider is arranged inside a smaller, offset child area.
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			if (F.Overlay.IsValid())
			{
				World->GetGameViewport()->RemoveViewportWidgetContent(F.Overlay.ToSharedRef());
			}
			const TWeakPtr<FCortexEditorPhysicalInputTestFixture> Weak = Fixture;
			const TSharedRef<SWidget> Replacement = SNew(SBox)
				.Padding(FMargin(0.0f, 20.0f, 200.0f, 20.0f))
				[
					SAssignNew(F.Slider, SSlider)
					.Value_Lambda([Weak]()
					{
						const auto P = Weak.Pin();
						return P ? P->SliderValue : 0.0f;
					})
					.OnValueChanged_Lambda([Weak](float Value)
					{
						if (const auto P = Weak.Pin()) { P->SliderValue = Value; }
					})
				];
			Replacement->SetTag(FName(TEXT("CortexPhysicalProbeRoot")));
			F.Slider->SetTag(FName(TEXT("CortexPhysicalProbeSlider")));
			F.Overlay = Replacement;
			World->GetGameViewport()->AddViewportWidgetContent(Replacement);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			if (!F.CapturedIdentity.IsValid()
				|| !F.Captured.IsValidIndex(F.CapturedPressIndex))
			{
				Test.AddError(TEXT("Captured press identity missing for the geometry change"));
				return;
			}
			const FGeometry NewGeometry = F.Slider->GetCachedGeometry();
			const FVector2D NewSize = NewGeometry.GetLocalSize();
			if (NewSize.X <= 0.0)
			{
				Test.AddError(TEXT("Recreated tagged root has no geometry"));
				return;
			}
			// Prove the recreated control really has different absolute geometry before relying on
			// the normalized-position check.
			const FVector2D NewPosition = NewGeometry.GetAbsolutePosition();
			Test.TestTrue(TEXT("Recreated control size really changed"),
				!NewSize.Equals(F.CapturedSliderSize, 0.5));
			Test.TestTrue(TEXT("Recreated control absolute geometry really changed"),
				!NewPosition.Equals(F.CapturedSliderPosition, 0.5)
					|| !NewSize.Equals(F.CapturedSliderSize, 0.5));

			const FVector2D NewLocal = NewGeometry.AbsoluteToLocal(F.CapturedAbsolutePosition);
			const float NewNormalizedX = static_cast<float>(NewLocal.X / NewSize.X);
			Test.TestTrue(TEXT("Recreated geometry really differs from the capture geometry"),
				FMath::Abs(NewNormalizedX - static_cast<float>(F.CapturedLocalPosition.X)) > 0.005f);

			FCortexEditorPhysicalInputUIObservation Observation;
			const FCortexCommandResult Observed = F.Session->ObserveUI(
				F.Captured[F.CapturedPressIndex], *F.CapturedIdentity, Observation);
			Test.TestTrue(TEXT("Recreated tagged root resolved on the selected route"), Observed.bSuccess);
			Test.TestEqual(TEXT("Recreated tagged root observation is ready"),
				Observation.State, ECortexEditorUIObservationState::Ready);
			Test.TestTrue(TEXT("Observation reflects the new geometry, not the recorded echo"),
				FMath::Abs(Observation.LocalPosition.X - NewNormalizedX) <= 0.005f);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A physical same-button down colliding with owned replay input is foreign: it interrupts, its
// cached bit survives cleanup, and the matching Up ends the foreign ownership.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputForeignSameButtonDownTest,
	"Cortex.Editor.PhysicalInputForeignSameButtonDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputForeignSameButtonDownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ForeignSameButtonDown"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			const FVector2D CenterViewport = ToViewportLocal(F, Center);

			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});

			// Replay ownership is armed at epoch establishment, not at the first Dispatch.
			Test.TestTrue(TEXT("Replay epoch armed for unattended ownership"),
				F.Session->BeginReplayEpoch().bSuccess);

			// Own the left mouse button through a real replayed press.
			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = CenterViewport;
			Test.TestTrue(TEXT("Owned pointer down dispatched"), F.Session->Dispatch(PointerDown).bSuccess);
			Test.TestTrue(TEXT("Owned cached button bit present"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));

			// A physical same-button Down arrives while replay owns the button.
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, TSet<FKey>({ EKeys::LeftMouseButton }), EKeys::LeftMouseButton,
				0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Foreign same-button down interrupted replay"), F.InterruptionCount, 1);
			Test.TestFalse(TEXT("Interruption reports a non-success result"), F.Interruption.bSuccess);

			// Cleanup must not manufacture a cache-removal Up for the foreign-held button.
			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Foreign-held button bit preserved through cleanup"),
				Slate.GetPressedMouseButtons().Contains(EKeys::LeftMouseButton));
			Test.TestTrue(TEXT("Foreign-held collision cannot claim clean neutralization"),
				!Cleanup.bSuccess);

			// The real Up ends the foreign ownership and balances the cached state.
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, TSet<FKey>(), EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Balanced after the foreign real Up"),
				Slate.GetPressedMouseButtons().Num(), 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A replaced PlayerInput is never mutated by cleanup
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPlayerInputReplacedTest,
	"Cortex.Editor.PhysicalInputPlayerInputReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPlayerInputReplacedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("PlayerInputReplaced"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			APlayerController* Controller = FixtureController(F);
			Test.TestNotNull(TEXT("Bound controller for the replaced PlayerInput"), Controller);
			if (!Controller) { return; }
			UPlayerInput* const OriginalPlayerInput = Controller->PlayerInput;
			Test.TestNotNull(TEXT("Original PlayerInput present"), OriginalPlayerInput);
			if (!OriginalPlayerInput) { return; }

			FCortexEditorPhysicalInputEvent KeyDown;
			KeyDown.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			KeyDown.Key = EKeys::W;
			Test.TestTrue(TEXT("Owned W down dispatched"), F.Session->Dispatch(KeyDown).bSuccess);

			UPlayerInput* Replacement = NewObject<UPlayerInput>(Controller);
			Test.TestNotNull(TEXT("Replacement PlayerInput created"), Replacement);
			Controller->PlayerInput = Replacement;

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestFalse(TEXT("Cleanup fails when the PlayerInput was replaced"), Cleanup.bSuccess);
			Test.TestEqual(TEXT("Replaced PlayerInput cleanup reports INPUT_CLEANUP_FAILED"),
				Cleanup.ErrorCode, FString(CortexEditorPhysicalInputErrorCodes::CleanupFailed));
			Test.TestTrue(TEXT("Replacement PlayerInput key state untouched"),
				Replacement->GetKeyState(EKeys::W) == nullptr
					|| !Replacement->GetKeyState(EKeys::W)->bDown);

			// Restore the original so fixture teardown can still neutralize the owned key.
			Controller->PlayerInput = OriginalPlayerInput;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Same-user, same-device editor input outside the selected route is not captured
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputUnrelatedSameUserPointerTest,
	"Cortex.Editor.PhysicalInputUnrelatedSameUserPointer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputUnrelatedSameUserPointerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("UnrelatedSameUserPointer"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const TSharedPtr<SWidget> InputRoot = Binding.InputRoot.Pin();
			Test.TestTrue(TEXT("Selected input root available"), InputRoot.IsValid());
			if (!InputRoot.IsValid()) { return; }

			// A point outside the selected viewport's own route (same Slate user and device).
			const FVector2D OffRoute =
				InputRoot->GetCachedGeometry().GetAbsolutePosition() - FVector2D(40.0, 40.0);
			const int32 Begin = F.Captured.Num();
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer, OffRoute, OffRoute,
				TSet<FKey>(), EKeys::Invalid, 0.0f, Modifiers, User), false);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				OffRoute, OffRoute, TSet<FKey>({ EKeys::LeftMouseButton }), EKeys::LeftMouseButton,
				0.0f, Modifiers, User));
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer, OffRoute,
				OffRoute, TSet<FKey>(), EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Same-user off-route input was not captured"), F.Captured.Num(), Begin);

			// Input genuinely on the selected route is still captured.
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer, Center, Center,
				TSet<FKey>(), EKeys::Invalid, 0.0f, Modifiers, User), false);
			Test.TestTrue(TEXT("On-route pointer motion is still captured"), F.Captured.Num() > Begin);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// UI waits follow live owned state, not whether replay has ever dispatched
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputWaitEligibilityTest,
	"Cortex.Editor.PhysicalInputWaitEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputWaitEligibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("WaitEligibility"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			FCortexEditorPhysicalInputEvent KeyDown;
			KeyDown.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			KeyDown.Key = EKeys::M;
			FCortexEditorPhysicalInputEvent KeyUp;
			KeyUp.Kind = ECortexEditorPhysicalInputKind::KeyUp;
			KeyUp.Key = EKeys::M;

			Test.TestTrue(TEXT("Menu opening press dispatched"), F.Session->Dispatch(KeyDown).bSuccess);
			Test.TestFalse(TEXT("UI wait is not eligible while replay owns a key"),
				F.Session->CanWaitForUI());
			Test.TestTrue(TEXT("Menu release dispatched"), F.Session->Dispatch(KeyUp).bSuccess);
			Test.TestTrue(TEXT("UI wait is eligible after balanced replay"), F.Session->CanWaitForUI());
			Test.TestTrue(TEXT("Further dispatch is still allowed after balanced replay"),
				F.Session->Dispatch(KeyDown).bSuccess);
			Test.TestFalse(TEXT("UI wait is ineligible again while the new key is held"),
				F.Session->CanWaitForUI());
			Test.TestTrue(TEXT("Trailing key released for teardown"), F.Session->Dispatch(KeyUp).bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Inconsistent modifier bits fault the capture epoch instead of recording it as valid
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputModifierFaultTest,
	"Cortex.Editor.PhysicalInputModifierFault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputModifierFaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ModifierFault"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FInputDeviceId Device = Binding.InputDevice;
			const FModifierKeysState NoModifiers;
			const FModifierKeysState ShiftDown(true, false, false, false, false, false, false, false, false);

			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});
			const FCortexCommandResult Armed = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestTrue(TEXT("Capture armed for the modifier fault case"), Armed.bSuccess);
			if (!Armed.bSuccess) { return; }
			F.bCaptureArmed = true;

			const int32 CapturedBefore = F.Captured.Num();
			// Modifier bits with no recorded modifier transition must fault the epoch.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, ShiftDown, Device, false, 0, 0, User));
			Test.TestEqual(TEXT("Inconsistent modifier bits faulted the epoch"), F.InterruptionCount, 1);
			Test.TestFalse(TEXT("Capture fault reports a non-success result"), F.Interruption.bSuccess);
			Test.TestEqual(TEXT("Capture fault is INVALID_OPERATION"), F.Interruption.ErrorCode,
				FString(CortexErrorCodes::InvalidOperation));
			Test.TestTrue(TEXT("Faulted human event was still recorded (processor did not consume)"),
				F.Captured.Num() > CapturedBefore);

			// A properly recorded modifier transition then agrees with the held bits.
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::LeftShift, ShiftDown, Device, false, 0, 0, User));
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, ShiftDown, Device, false, 0, 0, User));
			Test.TestEqual(TEXT("Recorded modifier transition does not fault again"),
				F.InterruptionCount, 1);
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::W, ShiftDown, Device, false, 0, 0, User));
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::LeftShift, NoModifiers, Device, false, 0, 0, User));
		}, /*bInstallProbe=*/true, /*bArmCapture=*/false, /*bOpenMenu=*/true));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Replayed pointer movement delivers the recorded explicit delta to a real consumer
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputDispatchDeltaTest,
	"Cortex.Editor.PhysicalInputDispatchDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputDispatchDeltaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("DispatchDeltaInstall"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputDeltaConsumer> Consumer =
				SNew(SCortexPhysicalInputDeltaConsumer).Fixture(Fixture);
			F.DeltaConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			const TSharedPtr<SWidget> Consumer = F.DeltaConsumer;
			Test.TestTrue(TEXT("Delta consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("Delta consumer has no geometry"));
				return;
			}
			const FVector2D CenterScreen = Geometry.LocalToAbsolute(Size * 0.5);
			FCortexEditorPhysicalInputEvent Move;
			Move.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			Move.ViewportPosition = ToViewportLocal(F, CenterScreen);
			Move.Delta = FVector2D(37.0, -11.0);
			Test.TestTrue(TEXT("Captured pointer move dispatched"), F.Session->Dispatch(Move).bSuccess);
			Test.TestTrue(TEXT("Real replay consumer saw the move"), F.ConsumerMoveCount > 0);
			Test.TestTrue(TEXT("Real replay consumer received the recorded explicit delta"),
				F.ConsumerDelta.Equals(Move.Delta, 0.01));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Replayed relative look is exact: the synthetic pointer accumulates in the consumed
// screen space and presents integral positions, so the recorded deltas cannot drift.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputRelativeMotionExactnessTest,
	"Cortex.Editor.PhysicalInputRelativeMotionExactness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputRelativeMotionExactnessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("RelativeMotionExactnessInstall"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputDeltaConsumer> Consumer =
				SNew(SCortexPhysicalInputDeltaConsumer).Fixture(Fixture);
			F.DeltaConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			const TSharedPtr<SWidget> Consumer = F.DeltaConsumer;
			Test.TestTrue(TEXT("Delta consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("Delta consumer has no geometry"));
				return;
			}
			// A deliberately fractional recorded pointer location: the previous viewport-derived
			// accumulator presented a fractional screen position here, which a rounding consumer
			// could drift a fraction of a pixel per event.
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			const FVector2D SeedScreen = Center + FVector2D(0.37, -0.61);
			FCortexEditorPhysicalInputEvent SeedMove;
			SeedMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			SeedMove.ViewportPosition = ToViewportLocal(F, SeedScreen);

			const FVector2D Delta(3.5, -2.25);
			const int32 Count = 6;

			// Sequence 1: N relative moves of Delta.
			Test.TestTrue(TEXT("Seed pointer move dispatched"), F.Session->Dispatch(SeedMove).bSuccess);
			F.ConsumerDeltas.Reset();
			F.ConsumerScreenPosition = FVector2D::ZeroVector;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FCortexEditorPhysicalInputEvent Relative;
				Relative.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
				Relative.Key = EKeys::Mouse2D;
				Relative.Delta = Delta;
				Test.TestTrue(TEXT("Relative motion dispatched"), F.Session->Dispatch(Relative).bSuccess);
			}
			const FVector2D PresentedAfterMany = F.ConsumerScreenPosition;
			FVector2D AccumulatorAfterMany, ViewportAfterMany;
			F.Session->GetReplayPointerPositionForTests(AccumulatorAfterMany, ViewportAfterMany);

			Test.TestEqual(TEXT("Every relative event presented through the consumer"),
				F.ConsumerDeltas.Num(), Count);
			for (int32 Index = 0; Index < F.ConsumerDeltas.Num(); ++Index)
			{
				Test.TestTrue(TEXT("Presented delta is exactly the recorded delta"),
					F.ConsumerDeltas[Index].Equals(Delta, 1e-6));
			}
			// Integral presentation: a consumer that rounds or truncates the cursor position on every
			// event can never accumulate a fraction of a pixel.
			Test.TestTrue(TEXT("Presented relative position is integral (X)"),
				FMath::RoundToDouble(PresentedAfterMany.X) == PresentedAfterMany.X);
			Test.TestTrue(TEXT("Presented relative position is integral (Y)"),
				FMath::RoundToDouble(PresentedAfterMany.Y) == PresentedAfterMany.Y);

			// Sequence 2: one relative move carrying the whole recorded delta, from the same seed.
			Test.TestTrue(TEXT("Seed pointer move re-dispatched"), F.Session->Dispatch(SeedMove).bSuccess);
			F.ConsumerDeltas.Reset();
			F.ConsumerScreenPosition = FVector2D::ZeroVector;
			FCortexEditorPhysicalInputEvent WholeRelative;
			WholeRelative.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
			WholeRelative.Key = EKeys::Mouse2D;
			WholeRelative.Delta = Delta * static_cast<double>(Count);
			Test.TestTrue(TEXT("Whole relative motion dispatched"), F.Session->Dispatch(WholeRelative).bSuccess);
			const FVector2D PresentedAfterOne = F.ConsumerScreenPosition;
			FVector2D AccumulatorAfterOne, ViewportAfterOne;
			F.Session->GetReplayPointerPositionForTests(AccumulatorAfterOne, ViewportAfterOne);

			Test.TestTrue(TEXT("Synthetic screen accumulator is additive"),
				AccumulatorAfterMany.Equals(AccumulatorAfterOne, 1e-6));
			Test.TestTrue(TEXT("Presented positions are additive"),
				PresentedAfterMany.Equals(PresentedAfterOne, 1e-6));
			Test.TestTrue(TEXT("Derived viewport pointer is additive"),
				ViewportAfterMany.Equals(ViewportAfterOne, 1e-6));
			Test.TestTrue(TEXT("Single-event delta is exactly the recorded total delta"),
				F.ConsumerDelta.Equals(Delta * static_cast<double>(Count), 1e-6));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// The replayed pointer derives its viewport-local position from the exact screen sum, so
// no viewport<->screen scale rounding accumulates; absolute moves still pin their location.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputRelativeMotionNoDriftTest,
	"Cortex.Editor.PhysicalInputRelativeMotionNoDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputRelativeMotionNoDriftTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("RelativeMotionNoDriftInstall"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputDeltaConsumer> Consumer =
				SNew(SCortexPhysicalInputDeltaConsumer).Fixture(Fixture);
			F.DeltaConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			const TSharedPtr<SWidget> Consumer = F.DeltaConsumer;
			Test.TestTrue(TEXT("Delta consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("Delta consumer has no geometry"));
				return;
			}
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			const FVector2D StartScreen = Center + FVector2D(0.23, 0.41);
			FCortexEditorPhysicalInputEvent StartMove;
			StartMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			StartMove.ViewportPosition = ToViewportLocal(F, StartScreen);
			Test.TestTrue(TEXT("Recorded starting pointer move dispatched"),
				F.Session->Dispatch(StartMove).bSuccess);

			FVector2D SeedScreen, SeedViewport;
			F.Session->GetReplayPointerPositionForTests(SeedScreen, SeedViewport);

			const FVector2D Delta(2.25, -1.75);
			const int32 Count = 8;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FCortexEditorPhysicalInputEvent Relative;
				Relative.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
				Relative.Key = EKeys::Mouse2D;
				Relative.Delta = Delta;
				Test.TestTrue(TEXT("Relative motion dispatched"), F.Session->Dispatch(Relative).bSuccess);
			}

			FVector2D Screen, Viewport;
			F.Session->GetReplayPointerPositionForTests(Screen, Viewport);
			const FVector2D Presented = F.ConsumerScreenPosition;
			const FVector2D ExpectedScreen = SeedScreen + Delta * static_cast<double>(Count);
			Test.TestTrue(TEXT("Screen accumulator equals the recorded deltas applied to the seed"),
				Screen.Equals(ExpectedScreen, 1e-6));
			Test.TestTrue(TEXT("Derived viewport position equals the exact sum (no scale-rounding drift)"),
				Viewport.Equals(ToViewportLocal(F, ExpectedScreen), 1e-6));
			// The presented cursor position is the quantized exact accumulator, so a rounding
			// consumer sums the recorded motion without accumulating a scale-rounding fraction.
			Test.TestTrue(TEXT("Presented position is the quantized exact sum"),
				Presented.Equals(FVector2D(FMath::RoundToDouble(ExpectedScreen.X),
					FMath::RoundToDouble(ExpectedScreen.Y)), 1e-6));

			// An absolute pointer move still pins its recorded viewport position and derives the
			// matching screen position, preserving the absolute semantics.
			FCortexEditorPhysicalInputEvent EndMove;
			EndMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			EndMove.ViewportPosition = ToViewportLocal(F, Center + FVector2D(-5.0, 4.0));
			Test.TestTrue(TEXT("Absolute pointer move dispatched"), F.Session->Dispatch(EndMove).bSuccess);
			FVector2D EndScreen, EndViewport;
			F.Session->GetReplayPointerPositionForTests(EndScreen, EndViewport);
			Test.TestTrue(TEXT("Absolute move pins its recorded viewport position"),
				EndViewport.Equals(EndMove.ViewportPosition, 1e-6));
			Test.TestTrue(TEXT("Absolute move derives the matching screen position"),
				EndScreen.Equals(ToViewportScreen(F, EndMove.ViewportPosition), 1e-6));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// An owned gameplay mouse button is neutralized on the original controller
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedMouseButtonNeutralizedTest,
	"Cortex.Editor.PhysicalInputOwnedMouseButtonNeutralized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedMouseButtonNeutralizedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("OwnedMouseButtonNeutralized"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			const auto& Binding = F.Session->GetTargetBinding();
			APlayerController* Controller = FixtureController(F);
			Test.TestNotNull(TEXT("Bound controller for the mouse-button cleanup"), Controller);
			if (!Controller) { return; }

			// A real gameplay input mode is required for the initial mouse down to reach the game
			// viewport and its controller: FSceneViewport forwards the first down to the viewport
			// client only for CaptureDuringMouseDown (or when it already has capture).
			Controller->SetInputMode(FInputModeGameAndUI());

			const TSharedPtr<SWidget> Viewport = Binding.ViewportWidget.Pin();
			const FVector2D ViewportCenter = Viewport.IsValid()
				? Viewport->GetCachedGeometry().GetLocalSize() * 0.5
				: FVector2D(100.0, 100.0);
			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = ViewportCenter;
			Test.TestTrue(TEXT("Gameplay mouse button down dispatched"),
				F.Session->Dispatch(PointerDown).bSuccess);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			APlayerController* Controller = FixtureController(F);
			Test.TestNotNull(TEXT("Bound controller for the mouse-button cleanup"), Controller);
			if (!Controller) { return; }

			// The original controller's gameplay PlayerInput really reports the owned button down:
			// PlayerInput updates bDown when it processes the queued press, so this is asserted a
			// few frames after the dispatch through the real path.
			UPlayerInput* const OriginalPlayerInput = Controller->PlayerInput;
			Test.TestNotNull(TEXT("Original PlayerInput present"), OriginalPlayerInput);
			Test.TestTrue(TEXT("Gameplay mouse button is down in the original PlayerInput before cleanup"),
				Controller->IsInputKeyDown(EKeys::LeftMouseButton));
			if (OriginalPlayerInput)
			{
				const FKeyState* const State = OriginalPlayerInput->GetKeyState(EKeys::LeftMouseButton);
				Test.TestTrue(TEXT("Original PlayerInput key state records the owned button down"),
					State != nullptr && State->bDown);
			}

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Gameplay mouse-button cleanup succeeded"), Cleanup.bSuccess);
			Test.TestFalse(TEXT("Gameplay mouse button is neutral after cleanup"),
				Controller->IsInputKeyDown(EKeys::LeftMouseButton));
			Test.TestEqual(TEXT("Cached mouse button bit cleared by the mouse-button cleanup"),
				Slate.GetPressedMouseButtons().Num(), 0);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A balanced replayed press/release on a real captured consumer reconciles owned capture
// ownership with the live engine state, so a UI wait is permitted again
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplayedCaptureWaitTest,
	"Cortex.Editor.PhysicalInputReplayedCaptureWait",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplayedCaptureWaitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ReplayedCaptureWait"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			const FVector2D CenterViewport = ToViewportLocal(F, Center);

			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = CenterViewport;
			Test.TestTrue(TEXT("Replayed press dispatched"), F.Session->Dispatch(PointerDown).bSuccess);
			Test.TestTrue(TEXT("Replayed press really captured the pointer"),
				Slate.HasUserMouseCapture(User));
			Test.TestFalse(TEXT("UI wait is not eligible while the replayed press holds capture"),
				F.Session->CanWaitForUI());

			FCortexEditorPhysicalInputEvent PointerUp;
			PointerUp.Kind = ECortexEditorPhysicalInputKind::PointerUp;
			PointerUp.Key = EKeys::LeftMouseButton;
			PointerUp.ViewportPosition = CenterViewport;
			Test.TestTrue(TEXT("Replayed release dispatched"), F.Session->Dispatch(PointerUp).bSuccess);
			Test.TestFalse(TEXT("Replayed release really released the live pointer capture"),
				Slate.HasUserMouseCapture(User));
			Test.TestTrue(TEXT("UI wait is eligible after the balanced capture release"),
				F.Session->CanWaitForUI());
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Movement routed to a live selected-route captor outside its hit area is admitted and recorded
// with the off-route position, while unrelated editor motion remains excluded. Consumer-level
// delivery of a captured off-route move (a rendered widget reacting to the routed move) is deferred
// to the Task 10 rendered matrix (physical human input) per the fix-cap ruling: this harness can
// only establish the session's admission/recording contract (the round-4
// IsPointerCaptureOnSelectedRoute admission is what admits this move).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCapturedMoveOutsideRouteTest,
	"Cortex.Editor.PhysicalInputCapturedMoveOutsideRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCapturedMoveOutsideRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("CapturedMoveOutsideRoute"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			const TSharedPtr<SWidget> InputRoot = Binding.InputRoot.Pin();
			Test.TestTrue(TEXT("Selected input root available"), InputRoot.IsValid());
			if (!InputRoot.IsValid()) { return; }
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { return; }
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			// A point outside the selected viewport's own route (same Slate user and device).
			const FVector2D OffRoute =
				InputRoot->GetCachedGeometry().GetAbsolutePosition() - FVector2D(40.0, 40.0);
			const FVector2D OffRouteLocal =
				InputRoot->GetCachedGeometry().AbsoluteToLocal(OffRoute);

			const int32 Begin = F.Captured.Num();
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Center, Center, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestTrue(TEXT("Captured press really captured the pointer"),
				Slate.HasUserMouseCapture(User));

			// The pointer leaves the viewport while the capture is live: the round-4
			// IsPointerCaptureOnSelectedRoute admission admits the move, so the replay stream records
			// it (with the off-route position) even though it is outside the captured hit area.
			Slate.ProcessMouseMoveEvent(FPointerEvent(Binding.InputDevice, Pointer, OffRoute, Center,
				Pressed, EKeys::Invalid, 0.0f, Modifiers, User), false);
			Test.TestEqual(TEXT("Captured outside-route press and move were recorded"),
				F.Captured.Num(), Begin + 2);
			if (F.Captured.IsValidIndex(Begin + 1))
			{
				Test.TestTrue(TEXT("Recorded outside-route move carries the off-route position"),
					F.Captured[Begin + 1].ViewportPosition.Equals(OffRouteLocal, 1.0f));
			}

			Pressed.Reset();
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer, OffRoute,
				OffRoute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Captured outside-route release was recorded"),
				F.Captured.Num(), Begin + 3);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A human down on a replay-owned keyboard key is foreign: cleanup never synthesizes its release or
// per-key neutralization, and the human's own Up balances the engine state
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputForeignSameKeyDownTest,
	"Cortex.Editor.PhysicalInputForeignSameKeyDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputForeignSameKeyDownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ForeignSameKeySetup"),
		[](FAutomationTestBase&, FCortexEditorPhysicalInputTestFixture&) {}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});

			// Replay ownership is armed at epoch establishment, not at the first Dispatch.
			Test.TestTrue(TEXT("Replay epoch armed for unattended ownership"),
				F.Session->BeginReplayEpoch().bSuccess);

			// Replay owns W, then a human press of the same key arrives while replay is active.
			FCortexEditorPhysicalInputEvent KeyDown;
			KeyDown.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			KeyDown.Key = EKeys::W;
			Test.TestTrue(TEXT("Owned W down dispatched"), F.Session->Dispatch(KeyDown).bSuccess);
			const FModifierKeysState Modifiers;
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::W, Modifiers, Binding.InputDevice,
				false, 0, 0, Binding.SlateUserIndex));
			Test.TestEqual(TEXT("Foreign same-key down interrupted replay"), F.InterruptionCount, 1);
			Test.TestFalse(TEXT("Interruption reports a non-success result"), F.Interruption.bSuccess);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			APlayerController* Controller = FixtureController(F);
			Test.TestNotNull(TEXT("Bound controller for the foreign-key cleanup"), Controller);
			if (!Controller) { return; }
			Test.TestTrue(TEXT("Original controller reports the key down before cleanup"),
				Controller->IsInputKeyDown(EKeys::W));

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Foreign-held key cleanup reports success"), Cleanup.bSuccess);
			Test.TestTrue(TEXT("Human-held key is not synthetically released by cleanup"),
				Controller->IsInputKeyDown(EKeys::W));

			// The human's real Up ends the foreign ownership and balances the engine state.
			const FModifierKeysState Modifiers;
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::W, Modifiers, Binding.InputDevice,
				false, 0, 0, Binding.SlateUserIndex));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			APlayerController* Controller = FixtureController(F);
			if (!Controller) { return; }
			Test.TestFalse(TEXT("Human key release balances the original controller"),
				Controller->IsInputKeyDown(EKeys::W));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// An owned drag-drop established by dispatch survives a capture-only check, so cleanup cancels the
// exact owned operation and only then reports success
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedDragDropCleanupTest,
	"Cortex.Editor.PhysicalInputOwnedDragDropCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedDragDropCleanupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("OwnedDragDropInstall"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputDragConsumer> Consumer =
				SNew(SCortexPhysicalInputDragConsumer).Fixture(Fixture);
			F.DragConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const TSharedPtr<SWidget> Consumer = F.DragConsumer;
			Test.TestTrue(TEXT("Drag consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("Drag consumer has no geometry"));
				return;
			}
			const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.3, Size.Y * 0.5));
			const FVector2D Finish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.9, Size.Y * 0.5));

			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = ToViewportLocal(F, Start);
			Test.TestTrue(TEXT("Drag pointer down dispatched"),
				F.Session->Dispatch(PointerDown).bSuccess);

			FCortexEditorPhysicalInputEvent PointerMove;
			PointerMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			PointerMove.ViewportPosition = ToViewportLocal(F, Finish);
			PointerMove.Delta = Finish - Start;
			Test.TestTrue(TEXT("Drag pointer move dispatched"),
				F.Session->Dispatch(PointerMove).bSuccess);

			const TSharedPtr<FSlateUser> User = Slate.GetUser(Binding.SlateUserIndex);
			Test.TestTrue(TEXT("Owned dispatch really established a drag-drop operation"),
				User.IsValid() && User->GetDragDropContent().IsValid());
			Test.TestEqual(TEXT("Drag was detected exactly once"), F.DragDetectedCount, 1);
			Test.TestFalse(TEXT("UI wait is not eligible while the owned drag is live"),
				F.Session->CanWaitForUI());

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Owned drag-drop cleanup succeeded"), Cleanup.bSuccess);
			const TSharedPtr<FSlateUser> PostUser = Slate.GetUser(Binding.SlateUserIndex);
			Test.TestFalse(TEXT("Owned drag-drop operation is gone after cleanup"),
				PostUser.IsValid() && PostUser->GetDragDropContent().IsValid());
			Test.TestTrue(TEXT("Owned drag-drop operation received the engine's cancel callback"),
				F.DragDropOperation.IsValid() && F.DragDropOperation->DropCount == 1
					&& F.DragDropOperation->bLastDropCancelled);
			Test.TestTrue(TEXT("UI wait is eligible again after the owned drag was cancelled"),
				F.Session->CanWaitForUI());
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// High-precision raw mouse movement established by an owned dispatch is released under the same
// identity guard during cleanup
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedHighPrecisionCleanupTest,
	"Cortex.Editor.PhysicalInputOwnedHighPrecisionCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedHighPrecisionCleanupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("HighPrecisionInstall"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			const TSharedRef<SCortexPhysicalInputHighPrecisionConsumer> Consumer =
				SNew(SCortexPhysicalInputHighPrecisionConsumer);
			F.HighPrecisionConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const TSharedPtr<SWidget> Consumer = F.HighPrecisionConsumer;
			Test.TestTrue(TEXT("High-precision consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("High-precision consumer has no geometry"));
				return;
			}
			const FVector2D Center = Geometry.LocalToAbsolute(Size * 0.5);
			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = ToViewportLocal(F, Center);
			Test.TestTrue(TEXT("High-precision pointer down dispatched"),
				F.Session->Dispatch(PointerDown).bSuccess);
			Test.TestTrue(TEXT("Owned dispatch really enabled high-precision mouse movement"),
				Slate.IsUsingHighPrecisionMouseMovment());
			Test.TestFalse(TEXT("UI wait is not eligible while the owned capture is live"),
				F.Session->CanWaitForUI());

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Owned high-precision cleanup succeeded"), Cleanup.bSuccess);
			Test.TestFalse(TEXT("Owned high-precision mouse movement is disabled after cleanup"),
				Slate.IsUsingHighPrecisionMouseMovment());
			Test.TestFalse(TEXT("Owned pointer capture is released after cleanup"),
				Slate.HasUserMouseCapture(Binding.SlateUserIndex));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// An owned dispatch can establish native (OS) capture together with high precision and then start a
// drag; Slate releases only the Slate captor on the drag start, so the native capture and
// high-precision mode survive with no Slate captor left. Cleanup must retain native-capture ownership
// independently of the Slate captor, release it under the owned identity, and verify it is gone
// before reporting success.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedNativeCaptureCleanupTest,
	"Cortex.Editor.PhysicalInputOwnedNativeCaptureCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedNativeCaptureCleanupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("OwnedNativeCaptureInstall"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			F.bDragConsumerHighPrecision = true;
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport())
			{
				Test.AddError(TEXT("Native-capture drag consumer target disappeared"));
				return;
			}
			const TSharedRef<SCortexPhysicalInputDragConsumer> Consumer =
				SNew(SCortexPhysicalInputDragConsumer).Fixture(Fixture);
			F.DragConsumer = Consumer;
			World->GetGameViewport()->AddViewportWidgetContent(Consumer);
		}, /*bInstallProbe=*/false, /*bArmCapture=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const TSharedPtr<SWidget> Consumer = F.DragConsumer;
			Test.TestTrue(TEXT("Native-capture drag consumer present"), Consumer.IsValid());
			if (!Consumer.IsValid()) { return; }
			const FGeometry Geometry = Consumer->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0)
			{
				Test.AddError(TEXT("Native-capture drag consumer has no geometry"));
				return;
			}
			const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication();
			const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.3, Size.Y * 0.5));
			const FVector2D Finish = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.9, Size.Y * 0.5));

			FCortexEditorPhysicalInputEvent PointerDown;
			PointerDown.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			PointerDown.Key = EKeys::LeftMouseButton;
			PointerDown.ViewportPosition = ToViewportLocal(F, Start);
			Test.TestTrue(TEXT("Native-capture drag down dispatched"),
				F.Session->Dispatch(PointerDown).bSuccess);
			const void* const OwnedNativeCapture = PlatformApplication.IsValid()
				? PlatformApplication->GetCapture() : nullptr;
			Test.TestTrue(TEXT("Owned dispatch established native capture"),
				OwnedNativeCapture != nullptr);

			FCortexEditorPhysicalInputEvent PointerMove;
			PointerMove.Kind = ECortexEditorPhysicalInputKind::PointerMove;
			PointerMove.ViewportPosition = ToViewportLocal(F, Finish);
			PointerMove.Delta = Finish - Start;
			Test.TestTrue(TEXT("Native-capture drag move dispatched"),
				F.Session->Dispatch(PointerMove).bSuccess);
			const TSharedPtr<FSlateUser> User = Slate.GetUser(Binding.SlateUserIndex);
			Test.TestTrue(TEXT("Owned dispatch really started a drag-drop operation"),
				User.IsValid() && User->GetDragDropContent().IsValid());
			Test.TestTrue(TEXT("Owned high precision survives the drag-start captor release"),
				Slate.IsUsingHighPrecisionMouseMovment());
			Test.TestTrue(TEXT("Owned native capture survives the drag-start captor release"),
				PlatformApplication.IsValid() && PlatformApplication->GetCapture() == OwnedNativeCapture);

			const FCortexCommandResult Cleanup = F.Session->ReleaseHeldInputs();
			Test.TestTrue(TEXT("Owned native-capture cleanup succeeded"), Cleanup.bSuccess);
			Test.TestFalse(TEXT("Owned native capture is released after cleanup"),
				PlatformApplication.IsValid() && PlatformApplication->GetCapture() == OwnedNativeCapture);
			Test.TestFalse(TEXT("Owned high precision is disabled after cleanup"),
				Slate.IsUsingHighPrecisionMouseMovment());
			Test.TestFalse(TEXT("Owned drag-drop operation is gone after cleanup"),
				User.IsValid() && User->GetDragDropContent().IsValid());
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// CR-04: admission must resolve the whole supported keyboard/mouse domain, not a small
// gameplay subset. A non-table key (Tab/Enter/I/F5) held on the selected controller before the
// capture processor could observe its down edge must deny admission; where the physical state
// cannot be resolved the policy fails rather than assumes neutrality.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Waits until capture admission is admitted, recording (once) the measured held-input report
// while it is denied and failing on timeout so a stuck non-neutral state is never a silent pass.
// This keeps the admission-on-neutral assertion independent of momentary ambient host input.
// ---------------------------------------------------------------------------
class FCortexWaitCaptureAdmitted : public IAutomationLatentCommand
{
public:
	FCortexWaitCaptureAdmitted(FAutomationTestBase* InTest,
		TSharedRef<FCortexEditorPhysicalInputTestFixture> InFixture)
		: Test(InTest), Fixture(MoveTemp(InFixture)) {}
	bool Update() override
	{
		if (Deadline == 0.0) { Deadline = FPlatformTime::Seconds() + CortexPhysicalInputReadyWatchdogSeconds; }
		FCortexEditorPhysicalInputTestFixture& F = *Fixture;
		const FCortexCommandResult Armed =
			F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
		if (Armed.bSuccess)
		{
			Test->TestTrue(TEXT("Neutral target is admitted after every held key is released"), true);
			F.bCaptureArmed = true;
			return true;
		}
		if (!bInfoRecorded)
		{
			bInfoRecorded = true;
			Test->AddInfo(FString::Printf(
				TEXT("Capture admission still held while waiting for neutrality: %s"), *Armed.ErrorMessage));
		}
		if (FPlatformTime::Seconds() > Deadline)
		{
			Test->AddError(FString::Printf(
				TEXT("Neutral target was never admitted; capture admission still held: %s"),
				*Armed.ErrorMessage));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorPhysicalInputTestFixture> Fixture;
	double Deadline = 0.0;
	bool bInfoRecorded = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCaptureAdmissionCoverageTest,
	"Cortex.Editor.PhysicalInputCaptureAdmissionCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCaptureAdmissionCoverageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("CoverageHoldKeys"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			APlayerController* Controller = FixtureController(F);
			if (!Controller) { Test.AddError(TEXT("Bound controller missing")); return; }
			const FInputDeviceId Device = F.Session->GetTargetBinding().InputDevice;
			// Held directly on the selected controller, so no Slate edge reaches the (installed)
			// observation processor: exactly the pre-processor-install hold the old table missed.
			for (const FKey Key : { EKeys::Tab, EKeys::Enter, EKeys::I, EKeys::F5, EKeys::Comma, EKeys::Colon })
			{
				Controller->InputKey(FInputKeyEventArgs(nullptr, Device, Key, IE_Pressed, 0));
			}
		}, /*bInstallProbe=*/false, /*bArmCapture=*/false, /*bOpenMenu=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			APlayerController* Controller = FixtureController(F);
			if (!Controller) { Test.AddError(TEXT("Bound controller missing")); return; }
			const FInputDeviceId Device = F.Session->GetTargetBinding().InputDevice;
			for (const FKey Key : { EKeys::Tab, EKeys::Enter, EKeys::I, EKeys::F5, EKeys::Comma, EKeys::Colon })
			{
				const FString Label = Key.ToString();
				Test.TestTrue(FString::Printf(TEXT("Selected controller reports %s held"), *Label),
					Controller->IsInputKeyDown(Key));
				const FCortexCommandResult Denied = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
				Test.TestFalse(FString::Printf(TEXT("Pre-held non-table key %s denies admission"), *Label),
					Denied.bSuccess);
				Test.TestEqual(FString::Printf(TEXT("Pre-held %s denial is INVALID_OPERATION"), *Label),
					Denied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
				Test.TestFalse(FString::Printf(TEXT("Pre-held %s human state untouched (re-arm denied)"), *Label),
					F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture)).bSuccess);
				Controller->InputKey(FInputKeyEventArgs(nullptr, Device, Key, IE_Released, 0));
			}

			// A UI-consumed non-table key delivered through the real preprocessor route is held in
			// the observed sets and must deny admission as well.
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState NoModifiers;
			Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::M, NoModifiers, Binding.InputDevice,
				false, 0, 0, Binding.SlateUserIndex));
			const FCortexCommandResult UiDenied = F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestFalse(TEXT("UI-consumed non-table key denies admission"), UiDenied.bSuccess);
			Test.TestTrue(TEXT("UI-consumed key still held after denial"),
				F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture)).bSuccess == false);
			Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::M, NoModifiers, Binding.InputDevice,
				false, 0, 0, Binding.SlateUserIndex));
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitCaptureAdmitted(this, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// CR-08: a tagged runtime selector must uniquely identify its instance within the selected
// scope. A second runtime panel with identical root/control tags is ambiguous: capture must not
// claim Supported coverage and playback must reject the match; removing the duplicate restores
// the unique identity (replacement ordering).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputTaggedSelectorUniquenessTest,
	"Cortex.Editor.PhysicalInputTaggedSelectorUniqueness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputTaggedSelectorUniquenessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("UniquenessBaseline"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { Test.AddError(TEXT("Probe slider has no geometry")); return; }
			const FVector2D Absolute = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));

			// The singly tagged runtime panel resolves as Supported.
			const int32 BaselineIndex = F.Captured.Num();
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Test.TestTrue(TEXT("Baseline tagged press was captured"), F.CapturedContexts.IsValidIndex(BaselineIndex));
			if (F.CapturedContexts.IsValidIndex(BaselineIndex))
			{
				Test.TestEqual(TEXT("Unique tagged control is supported"),
					F.CapturedContexts[BaselineIndex].UICoverage, ECortexEditorUICoverage::Supported);
			}

			// Add a second runtime panel with the identical root/control tags in the same viewport.
			UWorld* World = Binding.World.Get();
			if (!World || !World->GetGameViewport()) { return; }
			TSharedPtr<SSlider> DuplicateSlider;
			const TSharedRef<SWidget> Duplicate = SNew(SBox)
				[
					SAssignNew(DuplicateSlider, SSlider)
				];
			Duplicate->SetTag(FName(TEXT("CortexPhysicalProbeRoot")));
			DuplicateSlider->SetTag(FName(TEXT("CortexPhysicalProbeSlider")));
			F.DuplicateTaggedOverlay = Duplicate;
			World->GetGameViewport()->AddViewportWidgetContent(Duplicate);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { Test.AddError(TEXT("Probe slider has no geometry")); return; }
			const FVector2D Absolute = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));

			const int32 AmbiguousIndex = F.Captured.Num();
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Test.TestTrue(TEXT("Ambiguous tagged press was captured"), F.CapturedContexts.IsValidIndex(AmbiguousIndex));
			if (!F.CapturedContexts.IsValidIndex(AmbiguousIndex)) { return; }
			Test.TestFalse(TEXT("Ambiguous capture is not recorded as Supported"),
				F.CapturedContexts[AmbiguousIndex].UICoverage == ECortexEditorUICoverage::Supported);
			Test.TestEqual(TEXT("Ambiguous capture is Unavailable"),
				F.CapturedContexts[AmbiguousIndex].UICoverage, ECortexEditorUICoverage::Unavailable);
			Test.TestFalse(TEXT("Ambiguous capture carries no target identity"),
				F.CapturedContexts[AmbiguousIndex].UITarget.IsValid());

			// Playback: the same recorded selector must be rejected as ambiguous, never Ready.
			FCortexEditorPhysicalInputWidgetIdentity Expected;
			Expected.RootKind = ECortexEditorUIRootKind::Slate;
			Expected.Surface = ECortexEditorUISurface::Viewport;
			Expected.Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
			Expected.RootTag = TEXT("CortexPhysicalProbeRoot");
			Expected.TargetTag = TEXT("CortexPhysicalProbeSlider");
			FCortexEditorPhysicalInputUIObservation Observation;
			const FCortexCommandResult Observed =
				F.Session->ObserveUI(F.Captured[AmbiguousIndex], Expected, Observation);
			Test.TestFalse(TEXT("ObserveUI rejects the ambiguous match"), Observed.bSuccess);
			Test.TestEqual(TEXT("Ambiguous playback match reports ambiguous state"),
				Observation.State, ECortexEditorUIObservationState::Ambiguous);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!F.DuplicateTaggedOverlay.IsValid() || !World || !World->GetGameViewport()) { return; }
			World->GetGameViewport()->RemoveViewportWidgetContent(F.DuplicateTaggedOverlay.ToSharedRef());
			F.DuplicateTaggedOverlay.Reset();
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			const FGeometry Geometry = F.Slider->GetCachedGeometry();
			const FVector2D Size = Geometry.GetLocalSize();
			if (Size.X <= 0.0) { Test.AddError(TEXT("Probe slider has no geometry")); return; }
			const FVector2D Absolute = Geometry.LocalToAbsolute(FVector2D(Size.X * 0.5, Size.Y * 0.5));

			const int32 RestoredIndex = F.Captured.Num();
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			TSet<FKey> Released;
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
				Absolute, Absolute, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
			Test.TestTrue(TEXT("Restored tagged press was captured"), F.CapturedContexts.IsValidIndex(RestoredIndex));
			if (!F.CapturedContexts.IsValidIndex(RestoredIndex)) { return; }
			Test.TestEqual(TEXT("Removing the duplicate restores Supported coverage"),
				F.CapturedContexts[RestoredIndex].UICoverage, ECortexEditorUICoverage::Supported);
			Test.TestTrue(TEXT("Restored capture carries a target identity"),
				F.CapturedContexts[RestoredIndex].UITarget.IsValid());
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Owned teardown is resolved exactly once: after IsOwnedPIEEnded() is observed, ticking well
// past resolution and re-entering termination must perform no further end-PIE work (no second
// end while the engine tears the level viewport down) and must not crash.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedTeardownResolvedOnceTest,
	"Cortex.Editor.PhysicalInputOwnedTeardownResolvedOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedTeardownResolvedOnceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this, [Fixture](FAutomationTestBase&)
	{
		Fixture->PIEWorld = Fixture->Session->GetTargetBinding().World;
		Fixture->Session->EndOwnedPIE();
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForOwnedInputEnded(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 20,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			// Re-entering termination long after resolution must be a no-op, never another end-PIE.
			F.Session->EndOwnedPIE();
			Test.TestTrue(TEXT("Owned teardown stays resolved after a redundant end"),
				F.Session->IsOwnedPIEEnded());
			Test.TestFalse(TEXT("Resolved session binds no world"),
				F.Session->GetTargetBinding().World.IsValid());
			Test.TestFalse(TEXT("Owned PIE world is gone after resolution"),
				F.PIEWorld.IsValid());
			Test.TestTrue(TEXT("Editor owns no PIE world after resolution"),
				GEditor == nullptr || GEditor->PlayWorld == nullptr);
		}));

	return true;
}

// ---------------------------------------------------------------------------
// A physical non-table key held before the observation processor was installed is resolved by
// the Windows high-bit snapshot and denies admission both before preparation and at arming.
// The snapshot resolver is driven explicitly so the pre-installation path is covered
// deterministically instead of depending on ambient host keyboard input.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputPreInstallSnapshotAdmissionTest,
	"Cortex.Editor.PhysicalInputPreInstallSnapshotAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputPreInstallSnapshotAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	// The supported domain must be complete: no supported key identity is left unresolved (a
	// non-empty result would make admission fail explicitly naming the identity).
#if WITH_DEV_AUTOMATION_TESTS
	TestTrue(TEXT("Snapshot domain resolves every supported key identity"),
		FCortexEditorPhysicalInputSession::GetUnresolvedSupportedKeyNames().Num() == 0);
#endif

	// A physical non-table key (Tab) held in the snapshot, before any target exists and before
	// the processor could have observed a down edge, denies preparation admission.
#if WITH_DEV_AUTOMATION_TESTS
	FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
		[](const FKey& Key) { return Key == EKeys::Tab; });
#endif
	const FCortexCommandResult Denied = Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture));
	TestFalse(TEXT("Pre-held non-table key denies preparation admission through the snapshot"),
		Denied.bSuccess);
	TestEqual(TEXT("Snapshot preparation denial is INVALID_OPERATION"),
		Denied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	// A held shifted punctuation identity (Colon, which shares the Semicolon virtual key) must
	// also be resolvable through the same snapshot channel — it is neither in the observation sets
	// nor delivered to the controller before a target exists.
#if WITH_DEV_AUTOMATION_TESTS
	FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
		[](const FKey& Key) { return Key == EKeys::Colon; });
#endif
	const FCortexCommandResult PunctuationDenied = Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture));
	TestFalse(TEXT("Pre-held shifted punctuation key denies preparation admission through the snapshot"),
		PunctuationDenied.bSuccess);
	TestEqual(TEXT("Snapshot shifted punctuation denial is INVALID_OPERATION"),
		PunctuationDenied.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

	// Once the physical key is released, preparation is admitted.
#if WITH_DEV_AUTOMATION_TESTS
	FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
		[](const FKey&) { return false; });
#endif
	TestTrue(TEXT("Neutral snapshot prepares the same session"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
#if WITH_DEV_AUTOMATION_TESTS
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			Test.TestTrue(TEXT("Neutral snapshot arms the prepared target"),
				F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture)).bSuccess);
			F.bCaptureArmed = true;

			// The same physical key held at arming denies through the snapshot as well; it is not
			// in the observed sets or the controller state, so only the snapshot can catch it.
			FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
				[](const FKey& Key) { return Key == EKeys::Tab; });
			const FCortexCommandResult Armed =
				F.Session->SetCaptureCallback(MakeFixtureCaptureCallback(Fixture));
			Test.TestFalse(TEXT("Pre-held non-table key denies arming through the snapshot"),
				Armed.bSuccess);
			Test.TestEqual(TEXT("Snapshot arming denial is INVALID_OPERATION"),
				Armed.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
				[](const FKey&) { return false; });
		}));
#endif

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Replay ownership requires actual activation: with the application deactivated (the selected
// route is no longer the actually-active route) a scheduled synthetic event must not be delivered
// and the replay must interrupt, rather than forcing inactive-application input into the route.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputReplayOwnershipInactiveApplicationTest,
	"Cortex.Editor.PhysicalInputReplayOwnershipInactiveApplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputReplayOwnershipInactiveApplicationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ReplayInactiveSetup"),
		[](FAutomationTestBase&, FCortexEditorPhysicalInputTestFixture&) {}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			F.Session->SetInterruptionCallback([Fixture](const FCortexCommandResult& Result)
			{
				Fixture->InterruptionCount++;
				Fixture->Interruption = Result;
			});
			Test.TestTrue(TEXT("Replay epoch armed before the first event"),
				F.Session->BeginReplayEpoch().bSuccess);

			// The first event is delivered only while the route is the actually-active route.
			Test.TestTrue(TEXT("Selected route is the active top-level window before the first event"),
				Slate.IsActive() && Slate.GetActiveTopLevelWindow().IsValid());
			FCortexEditorPhysicalInputEvent First;
			First.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			First.Key = EKeys::W;
			Test.TestTrue(TEXT("First event dispatched while the route is active"),
				F.Session->Dispatch(First).bSuccess);
			Test.TestEqual(TEXT("No interruption while the route is active"), F.InterruptionCount, 0);

			// A foreign top-level window becomes the actually-active one while the application
			// stays active, so the selected route is no longer the actually-active route. This
			// uses a supported window-activation API rather than destabilising the shared editor
			// application's activation state.
			const TSharedRef<SWindow> ForeignWindow = SNew(SWindow)
				.ClientSize(FVector2D(240.0f, 140.0f))
				[ SNew(SBox) ];
			F.ForeignKeyHostWindow = ForeignWindow;
			Slate.AddWindow(ForeignWindow);
			ForeignWindow->BringToFront();
			Test.TestTrue(TEXT("Foreign window is the actually-active top-level window"),
				Slate.GetActiveTopLevelWindow() == ForeignWindow);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			Test.TestTrue(TEXT("Non-active route interrupted replay"), F.InterruptionCount >= 1);
			Test.TestFalse(TEXT("Non-active route interruption is a non-success result"),
				F.Interruption.bSuccess);
			Test.TestTrue(TEXT("Non-active route interruption reaches the active-window branch"),
				F.Interruption.ErrorMessage.Contains(TEXT("active top-level window")));

			// A scheduled event while the route is not actually active must not be delivered.
			FCortexEditorPhysicalInputEvent Second;
			Second.Kind = ECortexEditorPhysicalInputKind::KeyDown;
			Second.Key = EKeys::E;
			const int32 InterruptionsBefore = F.InterruptionCount;
			const FCortexCommandResult Delivered = F.Session->Dispatch(Second);
			Test.TestFalse(TEXT("Scheduled event is not delivered while the route is not actually active"),
				Delivered.bSuccess);
			Test.TestEqual(TEXT("Blocked non-active-route event adds no further interruption"),
				F.InterruptionCount, InterruptionsBefore);

			if (F.ForeignKeyHostWindow.IsValid())
			{
				Slate.RequestDestroyWindow(F.ForeignKeyHostWindow.ToSharedRef());
				F.ForeignKeyHostWindow.Reset();
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ===========================================================================
// CR-02 / CR-09 phase-1 fail-closed red tests (tests + inert guarded seam only).
//
// R1/R2 drive the real capture and live-resolution paths with the guarded SHA-256
// provider-failure seam forced: a selector whose digest cannot be produced must never be
// Supported or Ready. R6/R7 build real tagged runtime Slate trees and assert that root/target
// uniqueness is a property of the selected runtime scope (and, for the target, of the resolved
// root subtree) rather than of the whole hosting window. These assertions fail on the current
// production code and are fixed by the phase-2 fail-closed/scope changes. R5 is the in-scope
// regression that must stay green across both phases.
// ===========================================================================

namespace
{
/** Screen-space center of a live widget's cached geometry. */
FVector2D PhysicalTestWidgetAbsoluteCenter(const SWidget& Widget)
{
	const FGeometry& Geometry = Widget.GetCachedGeometry();
	return Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5);
}

/** One real pointer down/up on the selected user/device at a screen-space position. */
void PhysicalTestProcessPointerPress(FSlateApplication& Slate,
	const FCortexEditorPhysicalInputTargetBinding& Binding, const FVector2D& Absolute)
{
	const FModifierKeysState Modifiers;
	const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
	TSet<FKey> Pressed;
	Pressed.Add(EKeys::LeftMouseButton);
	Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Binding.InputDevice, Pointer,
		Absolute, Absolute, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
	TSet<FKey> Released;
	Slate.ProcessMouseButtonUpEvent(FPointerEvent(Binding.InputDevice, Pointer,
		Absolute, Absolute, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, Binding.SlateUserIndex));
}

/** A real tagged Slate selector carrying the canonical digest that capture and load produce. */
TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> PhysicalTestMakeSlateIdentity(
	const FString& RootTag, const FString& TargetTag)
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::Slate;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Identity->RootTag = RootTag;
	Identity->TargetTag = TargetTag;
	Identity->IdentitySha256 = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(*Identity);
	return TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
}
} // namespace

// R1: a real captured pointer press whose selector digest cannot be produced is Unavailable and
// records no supported identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputCaptureDigestFailureTest,
	"Cortex.Editor.PhysicalInputCaptureDigestFailureFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputCaptureDigestFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("DigestFailureCapture"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			if (!F.Slider.IsValid() || F.Slider->GetCachedGeometry().GetLocalSize().X <= 0.0)
			{
				Test.AddError(TEXT("Probe slider geometry unusable"));
				return;
			}
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(*F.Slider);

			// Force the simulated provider failure across the real capture path.
			FCortexEditorPhysicalInputSelectorBuilder::SetSelectorDigestFailureForTests(true);
			const int32 FailedIndex = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			FCortexEditorPhysicalInputSelectorBuilder::ClearSelectorDigestFailureForTests();

			Test.TestTrue(TEXT("Digest-failure press was captured"), F.CapturedContexts.IsValidIndex(FailedIndex));
			if (F.CapturedContexts.IsValidIndex(FailedIndex))
			{
				const FCortexEditorPhysicalInputCaptureContext& Context = F.CapturedContexts[FailedIndex];
				Test.TestTrue(TEXT("A failed digest is never recorded as Supported"),
					Context.UICoverage != ECortexEditorUICoverage::Supported);
				Test.TestEqual(TEXT("A failed digest is Unavailable"),
					Context.UICoverage, ECortexEditorUICoverage::Unavailable);
				Test.TestFalse(TEXT("A failed digest records no supported identity"),
					Context.UITarget.IsValid());
			}

			// Positive control: with the real provider restored the same real press is Supported.
			const int32 HealthyIndex = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			Test.TestTrue(TEXT("Healthy press was captured"), F.CapturedContexts.IsValidIndex(HealthyIndex));
			if (F.CapturedContexts.IsValidIndex(HealthyIndex))
			{
				Test.TestEqual(TEXT("The real provider still yields Supported coverage"),
					F.CapturedContexts[HealthyIndex].UICoverage, ECortexEditorUICoverage::Supported);
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// R2: live resolution of a valid recorded selector must never become Ready when the digest
// provider fails; it must fail closed instead of matching structurally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputLiveDigestFailureTest,
	"Cortex.Editor.PhysicalInputLiveDigestFailureFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputLiveDigestFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("DigestFailureLive"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			if (!F.Slider.IsValid() || F.Slider->GetCachedGeometry().GetLocalSize().X <= 0.0)
			{
				Test.AddError(TEXT("Probe slider geometry unusable"));
				return;
			}
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(*F.Slider);

			// Record a real supported selector with the healthy provider.
			const int32 BaselineIndex = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			if (!F.CapturedContexts.IsValidIndex(BaselineIndex)
				|| !F.CapturedContexts[BaselineIndex].UITarget.IsValid())
			{
				Test.AddError(TEXT("Baseline supported selector was not captured"));
				return;
			}
			const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Expected =
				F.CapturedContexts[BaselineIndex].UITarget;

			// Healthy live resolution of the recorded selector carries the same digest (green
			// regression: capture and live stages agree while the real provider is active).
			{
				FCortexEditorPhysicalInputUIObservation Healthy;
				const FCortexCommandResult HealthyObserved =
					F.Session->ObserveUI(F.Captured[BaselineIndex], *Expected, Healthy);
				Test.TestTrue(TEXT("Healthy live resolution observes Ready"), HealthyObserved.bSuccess);
				Test.TestEqual(TEXT("Healthy live resolution state Ready"),
					Healthy.State, ECortexEditorUIObservationState::Ready);
				if (Healthy.ActualTarget.IsValid())
				{
					Test.TestEqual(TEXT("Healthy live resolution carries the recorded digest"),
						Healthy.ActualTarget->IdentitySha256, Expected->IdentitySha256);
				}
				else
				{
					Test.TestTrue(TEXT("Healthy live resolution carries an actual identity"), false);
				}
			}

			// With the provider forced to fail, the same live route must never resolve Ready.
			FCortexEditorPhysicalInputSelectorBuilder::SetSelectorDigestFailureForTests(true);
			FCortexEditorPhysicalInputUIObservation Observation;
			const FCortexCommandResult Observed =
				F.Session->ObserveUI(F.Captured[BaselineIndex], *Expected, Observation);
			FCortexEditorPhysicalInputSelectorBuilder::ClearSelectorDigestFailureForTests();

			Test.TestFalse(TEXT("A failed live digest does not observe successfully"), Observed.bSuccess);
			Test.TestTrue(TEXT("A failed live digest never resolves Ready"),
				Observation.State != ECortexEditorUIObservationState::Ready);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// R6: same-window editor widgets that carry colliding root/target tags but live OUTSIDE the
// selected runtime scope must not invalidate the in-scope runtime selector.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputSameWindowOutOfScopeTagCollisionTest,
	"Cortex.Editor.PhysicalInputSameWindowOutOfScopeTagCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputSameWindowOutOfScopeTagCollisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("OutOfScopeTagCollision"),
		[](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			if (!F.Slider.IsValid() || F.Slider->GetCachedGeometry().GetLocalSize().X <= 0.0)
			{
				Test.AddError(TEXT("Probe slider geometry unusable"));
				return;
			}
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(*F.Slider);

			// Baseline: the real in-scope probe selector is Supported and observes Ready.
			const int32 BaselineIndex = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			if (!F.CapturedContexts.IsValidIndex(BaselineIndex)
				|| !F.CapturedContexts[BaselineIndex].UITarget.IsValid())
			{
				Test.AddError(TEXT("Baseline probe selector was not captured"));
				return;
			}
			Test.TestEqual(TEXT("Baseline probe press is Supported"),
				F.CapturedContexts[BaselineIndex].UICoverage, ECortexEditorUICoverage::Supported);
			const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Expected =
				F.CapturedContexts[BaselineIndex].UITarget;
			{
				FCortexEditorPhysicalInputUIObservation Observation;
				const FCortexCommandResult Observed =
					F.Session->ObserveUI(F.Captured[BaselineIndex], *Expected, Observation);
				Test.TestTrue(TEXT("Baseline probe observes Ready"), Observed.bSuccess);
				Test.TestEqual(TEXT("Baseline probe observation state Ready"),
					Observation.State, ECortexEditorUIObservationState::Ready);
			}

			// A real same-window editor widget that is an ancestor of (and therefore outside) the
			// selected runtime scope carries the colliding tags.
			const TSharedPtr<SWidget> Viewport = Binding.ViewportWidget.Pin();
			if (!Viewport.IsValid()) { Test.AddError(TEXT("Viewport widget missing")); return; }
			TSharedPtr<SWidget> Carrier = Viewport->GetParentWidget();
			if (!Carrier.IsValid())
			{
				Carrier = Slate.FindWidgetWindow(Viewport.ToSharedRef());
			}
			if (!Carrier.IsValid()) { Test.AddError(TEXT("Out-of-scope carrier missing")); return; }
			const FName PreviousTag = Carrier->GetTag();

			auto PressAndObserve = [&](const TCHAR* Label)
			{
				const int32 Index = F.Captured.Num();
				PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
				Test.TestTrue(FString::Printf(TEXT("%s press was captured"), Label),
					F.CapturedContexts.IsValidIndex(Index));
				if (!F.CapturedContexts.IsValidIndex(Index)) { return; }
				Test.TestEqual(FString::Printf(TEXT("%s keeps capture Supported"), Label),
					F.CapturedContexts[Index].UICoverage, ECortexEditorUICoverage::Supported);
				Test.TestTrue(FString::Printf(TEXT("%s keeps a target identity"), Label),
					F.CapturedContexts[Index].UITarget.IsValid());
				FCortexEditorPhysicalInputUIObservation Observation;
				const FCortexCommandResult Observed =
					F.Session->ObserveUI(F.Captured[Index], *Expected, Observation);
				Test.TestTrue(FString::Printf(TEXT("%s observes Ready"), Label), Observed.bSuccess);
				Test.TestEqual(FString::Printf(TEXT("%s observation state Ready"), Label),
					Observation.State, ECortexEditorUIObservationState::Ready);
			};

			Carrier->SetTag(FName(TEXT("CortexPhysicalProbeRoot")));
			PressAndObserve(TEXT("Out-of-scope root-tag collision"));
			Carrier->SetTag(PreviousTag);

			Carrier->SetTag(FName(TEXT("CortexPhysicalProbeSlider")));
			PressAndObserve(TEXT("Out-of-scope target-tag collision"));
			Carrier->SetTag(PreviousTag);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// R5: duplicate root tag or duplicate target tag INSIDE the selected runtime scope keeps capture
// Unavailable and playback Ambiguous.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputTaggedSelectorScopeUniquenessTest,
	"Cortex.Editor.PhysicalInputTaggedSelectorScopeUniqueness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputTaggedSelectorScopeUniquenessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	const TSharedPtr<TSharedPtr<SSlider>> Phase1Control = MakeShared<TSharedPtr<SSlider>>();
	const TSharedPtr<TSharedPtr<SSlider>> Phase2Control = MakeShared<TSharedPtr<SSlider>>();
	const TSharedPtr<TSharedPtr<SWidget>> CurrentOverlay = MakeShared<TSharedPtr<SWidget>>();

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("ScopeUniquenessSetup"),
		[Phase1Control, CurrentOverlay](FAutomationTestBase& Test,
			FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { Test.AddError(TEXT("Viewport missing")); return; }

			// Phase 1: one root tag with two controls sharing the same target tag. The root tag
			// lives on the hit-testable container so the root is genuinely present on the hit path.
			TSharedPtr<SSlider> First, Second;
			const TSharedPtr<SBox> FirstBox = SNew(SBox).WidthOverride(160.0f).HeightOverride(60.0f)
				[
					SAssignNew(First, SSlider)
				];
			const TSharedPtr<SBox> SecondBox = SNew(SBox).WidthOverride(160.0f).HeightOverride(60.0f)
				[
					SAssignNew(Second, SSlider)
				];
			const TSharedRef<SBox> RootBox = SNew(SBox)
				[
					SNew(SOverlay)
					+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top)
					[
						FirstBox.ToSharedRef()
					]
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom)
					[
						SecondBox.ToSharedRef()
					]
				];
			RootBox->SetTag(FName(TEXT("CortexScopeRoot")));
			First->SetTag(FName(TEXT("CortexScopeControl")));
			Second->SetTag(FName(TEXT("CortexScopeControl")));
			World->GetGameViewport()->AddViewportWidgetContent(RootBox);
			*Phase1Control = First;
			*CurrentOverlay = RootBox;
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture, Phase1Control, Phase2Control, CurrentOverlay](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			UWorld* World = Binding.World.Get();
			if (!World || !World->GetGameViewport() || !Phase1Control->IsValid())
			{
				Test.AddError(TEXT("Phase-1 scope tree missing"));
				return;
			}
			if ((*Phase1Control)->GetCachedGeometry().GetLocalSize().X <= 0.0)
			{
				Test.AddError(TEXT("Phase-1 control geometry unusable"));
				return;
			}

			// Duplicate target tag inside the resolved root subtree: capture Unavailable, playback Ambiguous.
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(**Phase1Control);
			const int32 Index = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			Test.TestTrue(TEXT("Duplicate-target-tag press was captured"),
				F.CapturedContexts.IsValidIndex(Index));
			if (F.CapturedContexts.IsValidIndex(Index))
			{
				Test.TestEqual(TEXT("Duplicate target tag in the root subtree is Unavailable"),
					F.CapturedContexts[Index].UICoverage, ECortexEditorUICoverage::Unavailable);
				Test.TestFalse(TEXT("Duplicate target tag records no supported identity"),
					F.CapturedContexts[Index].UITarget.IsValid());
			}
			{
				const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Expected =
					PhysicalTestMakeSlateIdentity(TEXT("CortexScopeRoot"), TEXT("CortexScopeControl"));
				FCortexEditorPhysicalInputUIObservation Observation;
				const FCortexCommandResult Observed = F.Session->ObserveUI(
					F.Captured.IsValidIndex(Index) ? F.Captured[Index] : FCortexEditorPhysicalInputEvent(),
					*Expected, Observation);
				Test.TestFalse(TEXT("Duplicate target tag playback is rejected"), Observed.bSuccess);
				Test.TestEqual(TEXT("Duplicate target tag playback is Ambiguous"),
					Observation.State, ECortexEditorUIObservationState::Ambiguous);
			}

			// Swap to phase 2: two roots sharing one root tag, each with a distinct target tag.
			World->GetGameViewport()->RemoveViewportWidgetContent(CurrentOverlay->ToSharedRef());
			CurrentOverlay->Reset();

			TSharedPtr<SSlider> ControlA, ControlB;
			const TSharedPtr<SBox> RootA = SNew(SBox).WidthOverride(160.0f).HeightOverride(60.0f)
				[
					SAssignNew(ControlA, SSlider)
				];
			const TSharedPtr<SBox> RootB = SNew(SBox).WidthOverride(160.0f).HeightOverride(60.0f)
				[
					SAssignNew(ControlB, SSlider)
				];
			const TSharedRef<SOverlay> Phase2 = SNew(SOverlay)
				+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top)
				[
					RootA.ToSharedRef()
				]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom)
				[
					RootB.ToSharedRef()
				];
			RootA->SetTag(FName(TEXT("CortexScopeDupRoot")));
			RootB->SetTag(FName(TEXT("CortexScopeDupRoot")));
			ControlA->SetTag(FName(TEXT("CortexScopeTargetA")));
			ControlB->SetTag(FName(TEXT("CortexScopeTargetB")));
			World->GetGameViewport()->AddViewportWidgetContent(Phase2);
			*Phase2Control = ControlA;
			*CurrentOverlay = Phase2;
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture, Phase2Control, CurrentOverlay](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			if (!Phase2Control->IsValid() || (*Phase2Control)->GetCachedGeometry().GetLocalSize().X <= 0.0)
			{
				Test.AddError(TEXT("Phase-2 control geometry unusable"));
				return;
			}

			// Duplicate root tag inside the runtime scope: capture Unavailable, playback Ambiguous.
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(**Phase2Control);
			const int32 Index = F.Captured.Num();
			PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
			Test.TestTrue(TEXT("Duplicate-root-tag press was captured"),
				F.CapturedContexts.IsValidIndex(Index));
			if (F.CapturedContexts.IsValidIndex(Index))
			{
				Test.TestEqual(TEXT("Duplicate root tag in scope is Unavailable"),
					F.CapturedContexts[Index].UICoverage, ECortexEditorUICoverage::Unavailable);
				Test.TestFalse(TEXT("Duplicate root tag records no supported identity"),
					F.CapturedContexts[Index].UITarget.IsValid());
			}
			{
				const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Expected =
					PhysicalTestMakeSlateIdentity(TEXT("CortexScopeDupRoot"), TEXT("CortexScopeTargetA"));
				FCortexEditorPhysicalInputUIObservation Observation;
				const FCortexCommandResult Observed = F.Session->ObserveUI(
					F.Captured.IsValidIndex(Index) ? F.Captured[Index] : FCortexEditorPhysicalInputEvent(),
					*Expected, Observation);
				Test.TestFalse(TEXT("Duplicate root tag playback is rejected"), Observed.bSuccess);
				Test.TestEqual(TEXT("Duplicate root tag playback is Ambiguous"),
					Observation.State, ECortexEditorUIObservationState::Ambiguous);
			}

			if (CurrentOverlay->IsValid())
			{
				if (UWorld* World = Binding.World.Get())
				{
					if (UGameViewportClient* Viewport = World->GetGameViewport())
					{
						Viewport->RemoveViewportWidgetContent(CurrentOverlay->ToSharedRef());
					}
				}
				CurrentOverlay->Reset();
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// R7: two distinct uniquely-tagged runtime roots in the same runtime scope may reuse the SAME
// target tag; each root resolves independently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputDistinctRootsSharedTargetTagTest,
	"Cortex.Editor.PhysicalInputDistinctRootsSharedTargetTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputDistinctRootsSharedTargetTagTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	const TSharedPtr<TSharedPtr<SSlider>> ControlA = MakeShared<TSharedPtr<SSlider>>();
	const TSharedPtr<TSharedPtr<SSlider>> ControlB = MakeShared<TSharedPtr<SSlider>>();
	const TSharedPtr<TSharedPtr<SWidget>> Overlay = MakeShared<TSharedPtr<SWidget>>();

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("DistinctRootsSetup"),
		[ControlA, ControlB, Overlay](FAutomationTestBase& Test,
			FCortexEditorPhysicalInputTestFixture& F)
		{
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!World || !World->GetGameViewport()) { Test.AddError(TEXT("Viewport missing")); return; }

			TSharedPtr<SSlider> A, B;
			const TSharedPtr<SBox> RootA = SNew(SBox).WidthOverride(220.0f).HeightOverride(80.0f)
				[
					SAssignNew(A, SSlider)
				];
			const TSharedPtr<SBox> RootB = SNew(SBox).WidthOverride(220.0f).HeightOverride(80.0f)
				[
					SAssignNew(B, SSlider)
				];
			const TSharedRef<SOverlay> Built = SNew(SOverlay)
				+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top)
				[
					RootA.ToSharedRef()
				]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom)
				[
					RootB.ToSharedRef()
				];
			RootA->SetTag(FName(TEXT("CortexSharedTargetRootA")));
			RootB->SetTag(FName(TEXT("CortexSharedTargetRootB")));
			A->SetTag(FName(TEXT("CortexSharedTargetControl")));
			B->SetTag(FName(TEXT("CortexSharedTargetControl")));
			World->GetGameViewport()->AddViewportWidgetContent(Built);
			*ControlA = A;
			*ControlB = B;
			*Overlay = Built;
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture, ControlA, ControlB, Overlay](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			if (!ControlA->IsValid() || !ControlB->IsValid())
			{
				Test.AddError(TEXT("Shared-target-tag controls missing"));
				return;
			}

			auto PressRoot = [&](const TSharedPtr<SSlider>& Control, const TCHAR* RootTag, const TCHAR* Label)
			{
				if (Control->GetCachedGeometry().GetLocalSize().X <= 0.0)
				{
					Test.AddError(FString::Printf(TEXT("%s geometry unusable"), Label));
					return;
				}
				const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(*Control);
				const int32 Index = F.Captured.Num();
				PhysicalTestProcessPointerPress(Slate, Binding, Absolute);
				Test.TestTrue(FString::Printf(TEXT("%s press was captured"), Label),
					F.CapturedContexts.IsValidIndex(Index));
				if (!F.CapturedContexts.IsValidIndex(Index)) { return; }
				Test.TestEqual(FString::Printf(TEXT("%s is Supported"), Label),
					F.CapturedContexts[Index].UICoverage, ECortexEditorUICoverage::Supported);
				if (!F.CapturedContexts[Index].UITarget.IsValid())
				{
					Test.AddError(FString::Printf(TEXT("%s records no target identity"), Label));
					return;
				}
				Test.TestEqual(FString::Printf(TEXT("%s resolves its own root tag"), Label),
					F.CapturedContexts[Index].UITarget->RootTag, FString(RootTag));

				const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Expected =
					PhysicalTestMakeSlateIdentity(RootTag, TEXT("CortexSharedTargetControl"));
				FCortexEditorPhysicalInputUIObservation Observation;
				const FCortexCommandResult Observed =
					F.Session->ObserveUI(F.Captured[Index], *Expected, Observation);
				Test.TestTrue(FString::Printf(TEXT("%s observes Ready"), Label), Observed.bSuccess);
				Test.TestEqual(FString::Printf(TEXT("%s observation state Ready"), Label),
					Observation.State, ECortexEditorUIObservationState::Ready);
			};

			PressRoot(*ControlA, TEXT("CortexSharedTargetRootA"), TEXT("Shared-target root A"));
			PressRoot(*ControlB, TEXT("CortexSharedTargetRootB"), TEXT("Shared-target root B"));

			if (Overlay->IsValid())
			{
				if (UWorld* World = Binding.World.Get())
				{
					if (UGameViewportClient* Viewport = World->GetGameViewport())
					{
						Viewport->RemoveViewportWidgetContent(Overlay->ToSharedRef());
					}
				}
				Overlay->Reset();
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A foreign popup over the viewport must not contribute presses to its capture stream.
// Matching releases of genuine route presses still balance after the pointer leaves the route.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputForeignPopupPressNotCapturedTest,
	"Cortex.Editor.PhysicalInputForeignPopupPressNotCaptured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputForeignPopupPressNotCapturedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	const auto PopupPresses = MakeShared<int32>(0);
	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	// Arm capture with no probe and no menu: only the overlapping popup is under test.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture, TEXT("OverlappingPopupSetup"),
		[Fixture, PopupPresses](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const TSharedPtr<SWidget> Viewport = Binding.ViewportWidget.Pin();
			if (!Viewport.IsValid()) { Test.AddError(TEXT("Viewport widget missing")); return; }
			const FVector2D Center = PhysicalTestWidgetAbsoluteCenter(*Viewport);

			// A distinct top-level popup is added last (topmost) directly over the viewport centre.
			const TSharedRef<SButton> PopupButton = SNew(SButton)
				.OnPressed_Lambda([PopupPresses] { ++*PopupPresses; });
			const TSharedRef<SWindow> Popup = SNew(SWindow)
				.AutoCenter(EAutoCenter::None)
				.ClientSize(FVector2D(160.0f, 90.0f))
				.ScreenPosition(Center - FVector2D(80.0f, 45.0f))
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				[
					PopupButton
				];
			F.CapturingWidget = PopupButton;
			F.ForeignKeyHostWindow = Popup;
			Slate.AddWindow(Popup, /*bShowImmediately=*/true);
			Test.TestTrue(TEXT("Popup was added as a top-level window"),
				Slate.GetTopLevelWindows().Contains(Popup));
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true, /*bOpenMenu=*/false));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Fixture, PopupPresses](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			auto& Slate = FSlateApplication::Get();
			const auto& Binding = F.Session->GetTargetBinding();
			const int32 User = Binding.SlateUserIndex;
			const FInputDeviceId Device = Binding.InputDevice;
			const TSharedPtr<SWidget> Viewport = Binding.ViewportWidget.Pin();
			if (!Viewport.IsValid() || !F.ForeignKeyHostWindow.IsValid())
			{
				Test.AddError(TEXT("Overlapping-popup fixture missing"));
				if (F.ForeignKeyHostWindow.IsValid())
				{
					Slate.RequestDestroyWindow(F.ForeignKeyHostWindow.ToSharedRef());
				}
				F.ForeignKeyHostWindow.Reset();
				F.CapturingWidget.Reset();
				return;
			}
			const TSharedRef<SWindow> Popup = F.ForeignKeyHostWindow.ToSharedRef();
			const FGeometry Geometry = Viewport->GetCachedGeometry();
			const FVector2D ViewportSize = Geometry.GetLocalSize();
			const FVector2D PopupPoint = Geometry.LocalToAbsolute(ViewportSize * 0.5);
			// A point inside the viewport but clear of the small centred popup.
			const FVector2D RoutePoint =
				Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.15, ViewportSize.Y * 0.85));

			const bool bSavedInactiveInputHandling =
				Slate.GetHandleDeviceInputWhenApplicationNotActive();
			Slate.SetHandleDeviceInputWhenApplicationNotActive(true);

			const FSlateRect PopupRect = Popup->GetRectInScreen();
			Test.TestTrue(TEXT("Foreign popup covers the viewport centre"),
				PopupRect.ContainsPoint(PopupPoint));
			Test.TestFalse(TEXT("Chosen route point is clear of the popup"),
				PopupRect.ContainsPoint(RoutePoint));

			const FModifierKeysState Modifiers;
			const uint32 Pointer = FSlateApplicationBase::CursorPointerIndex;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			TSet<FKey> Released;
			auto PressAt = [&](const FVector2D& Where)
			{
				Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Device, Pointer,
					Where, Where, Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			};
			auto ReleaseAt = [&](const FVector2D& Where)
			{
				Slate.ProcessMouseButtonUpEvent(FPointerEvent(Device, Pointer,
					Where, Where, Released, EKeys::LeftMouseButton, 0.0f, Modifiers, User));
			};

			// 1. The press belongs to the foreign popup, so it must not be captured even though the
			//    popup overlaps the selected route window's viewport.
			const int32 Begin = F.Captured.Num();
			PressAt(PopupPoint);
			Test.TestEqual(TEXT("Foreign popup consumes the overlapping press"), *PopupPresses, 1);
			Test.TestEqual(TEXT("Press over the foreign popup is not captured"),
				F.Captured.Num(), Begin);
			const TSharedPtr<FSlateUser> PopupUser = Slate.GetUser(User);
			Test.TestTrue(TEXT("Foreign popup owns cursor capture"),
				PopupUser.IsValid() && PopupUser->GetPointerCaptor(Pointer) == F.CapturingWidget);
			Slate.ProcessMouseMoveEvent(FPointerEvent(Device, Pointer,
				RoutePoint, RoutePoint - FVector2D(1.0, 0.0), Pressed, FKey(), 0.0f, Modifiers, User), false);
			Test.TestEqual(TEXT("Foreign-captured movement over the route is excluded"),
				F.Captured.Num(), Begin);
			TSet<FKey> ForeignButtons = Pressed;
			ForeignButtons.Add(EKeys::RightMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Device, Pointer,
				RoutePoint, RoutePoint, ForeignButtons, EKeys::RightMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Foreign-captured press over the route is excluded"),
				F.Captured.Num(), Begin);
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Device, Pointer,
				RoutePoint, RoutePoint, Pressed, EKeys::RightMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Foreign-captured release over the route is excluded"),
				F.Captured.Num(), Begin);
			ReleaseAt(PopupPoint);

			// 2. A genuine route press away from the popup is still captured.
			const int32 AfterPopup = F.Captured.Num();
			PressAt(RoutePoint);
			Test.TestEqual(TEXT("Genuine route press is captured"), F.Captured.Num(), AfterPopup + 1);
			if (F.Captured.Num() > AfterPopup)
			{
				Test.TestEqual(TEXT("Captured route press records a pointer down"),
					F.Captured[AfterPopup].Kind, ECortexEditorPhysicalInputKind::PointerDown);
			}
			const TSharedPtr<FSlateUser> SlateUser = Slate.GetUser(User);
			Test.TestTrue(TEXT("Genuine route press owns cursor capture"),
				SlateUser.IsValid() && SlateUser->GetPointerCaptor(Pointer) == Viewport);
			Slate.ProcessMouseMoveEvent(FPointerEvent(Device, Pointer,
				PopupPoint, PopupPoint - FVector2D(1.0, 0.0), Pressed, FKey(), 0.0f, Modifiers, User), false);
			Test.TestEqual(TEXT("Route-captured movement over the popup is recorded"),
				F.Captured.Num(), AfterPopup + 2);
			// An existing route captor receives additional buttons even over the foreign popup.
			TSet<FKey> BothButtons = Pressed;
			BothButtons.Add(EKeys::RightMouseButton);
			Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Device, Pointer,
				PopupPoint, PopupPoint, BothButtons, EKeys::RightMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Additional route-captured button press is recorded"),
				F.Captured.Num(), AfterPopup + 3);
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Device, Pointer,
				PopupPoint, PopupPoint, Pressed, EKeys::RightMouseButton, 0.0f, Modifiers, User));
			Test.TestEqual(TEXT("Additional route-captured button release is recorded"),
				F.Captured.Num(), AfterPopup + 4);

			// 3. The matching release after the pointer moves onto the popup is still recorded, so the
			//    genuine route press stays balanced.
			ReleaseAt(PopupPoint);
			Test.TestEqual(TEXT("Matching release over the popup is recorded"),
				F.Captured.Num(), AfterPopup + 5);
			Test.TestTrue(TEXT("Balanced genuine route press leaves the capture neutral"),
				F.Session->CanWaitForUI());

			Slate.SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			Slate.RequestDestroyWindow(Popup);
			F.ForeignKeyHostWindow.Reset();
			F.CapturingWidget.Reset();
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Focus regressions for the owned replay ARM. The owned replay acquires the initial route focus
// at BeginReplayEpoch (never during the shared owned capture preparation).
// ---------------------------------------------------------------------------

/** The owned PIE route root: the exact widget the engine registers as the PIE game viewport. */
TSharedPtr<SViewport> ResolveOwnedRouteRoot(const FCortexEditorPhysicalInputSession& Session)
{
	UWorld* PIE = Session.GetTargetBinding().World.Get();
	UGameViewportClient* ViewportClient = PIE ? PIE->GetGameViewport() : nullptr;
	return ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
}

// ---------------------------------------------------------------------------
// An owned replay ARM whose route is ALREADY keyboard-focused must arm successfully.
// FSlateApplication::SetUserFocus returns false when the target is already focused
// (SlateApplication.cpp:3029-3032), and an existing on-route focus must be preserved, so the arm
// must not treat an already-focused route as an acquisition failure.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedReplayAlreadyFocusedTest,
	"Cortex.Editor.PhysicalInputOwnedReplayAlreadyFocused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedReplayAlreadyFocusedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping owned replay already-focused test"));
		return true;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	// The engine's conditional auto-focus is off so the already-focused state is established by the
	// test (and restored) rather than by the play setting.
	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	const bool bSavedGameGetsMouseControl = PlaySettings->GameGetsMouseControl;
	PlaySettings->GameGetsMouseControl = false;

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this,
		[Fixture, PlaySettings, bSavedGameGetsMouseControl](FAutomationTestBase& T)
		{
			auto& Slate = FSlateApplication::Get();
			const uint32 User = static_cast<uint32>(Fixture->Session->GetTargetBinding().SlateUserIndex);
			const TSharedPtr<SViewport> Route = ResolveOwnedRouteRoot(*Fixture->Session);
			T.TestTrue(TEXT("Owned route root resolvable before arm"), Route.IsValid());
			if (Route.IsValid())
			{
				TSharedPtr<SWidget> RouteWidget = Route;
				Slate.SetUserFocus(User, RouteWidget, EFocusCause::SetDirectly);
				T.TestTrue(TEXT("Route is already keyboard-focused before arm"),
					Slate.GetUserFocusedWidget(User).Get() == Route.Get());
			}

			const FCortexCommandResult Arm = Fixture->Session->BeginReplayEpoch();
			T.TestTrue(TEXT("Already-focused owned replay arm succeeds"), Arm.bSuccess);
			T.TestTrue(TEXT("Already-focused owned replay epoch is armed"),
				Fixture->Session->IsReplayEpochArmed());

			PlaySettings->GameGetsMouseControl = bSavedGameGetsMouseControl;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// A global focus-change to the owned route during the owned replay ARM
// (FSlateApplication::OnFocusChanging fires synchronously from SetUserFocus) that invalidates the
// operation (here Session->Shutdown) must fail the arm: no epoch may be armed on a stale target.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputOwnedReplayFocusCancellationTest,
	"Cortex.Editor.PhysicalInputOwnedReplayFocusCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputOwnedReplayFocusCancellationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping owned replay focus-cancellation test"));
		return true;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	// The engine focuses the PIE route on its own with EFocusCause::WindowActivate (viewport
	// activation) or Mouse (capture-on-focus); the plugin's arm acquisition uses SetDirectly. Gate
	// on that cause so only the plugin's synchronous acquisition triggers the cancellation, and
	// force the 'Game Gets Mouse Control' auto-focus path off (it would otherwise also use SetDirectly).
	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	const bool bSavedGameGetsMouseControl = PlaySettings->GameGetsMouseControl;
	PlaySettings->GameGetsMouseControl = false;

	const TSharedPtr<bool> bFocusCallbackFired = MakeShared<bool>(false);
	const FDelegateHandle FocusHandle = FSlateApplication::Get().OnFocusChanging().AddLambda(
		[WeakFixture = TWeakPtr<FCortexEditorPhysicalInputTestFixture>(Fixture), bFocusCallbackFired](
			const FFocusEvent& FocusEvent, const FWeakWidgetPath&, const TSharedPtr<SWidget>&,
			const FWidgetPath&, const TSharedPtr<SWidget>& NewFocused)
		{
			if (*bFocusCallbackFired) { return; }
			if (FocusEvent.GetCause() != EFocusCause::SetDirectly) { return; }
			const TSharedPtr<FCortexEditorPhysicalInputTestFixture> Pinned = WeakFixture.Pin();
			if (!Pinned.IsValid()) { return; }
			const TSharedPtr<SViewport> Route = ResolveOwnedRouteRoot(*Pinned->Session);
			if (!Route.IsValid() || NewFocused.Get() != Route.Get()) { return; }
			*bFocusCallbackFired = true;
			// The synchronous focus change invalidates the in-flight owned replay arm.
			Pinned->Session->Shutdown();
		});

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this,
		[Fixture, bFocusCallbackFired, FocusHandle, PlaySettings, bSavedGameGetsMouseControl](FAutomationTestBase& T)
		{
			auto& Slate = FSlateApplication::Get();
			const uint32 User = static_cast<uint32>(Fixture->Session->GetTargetBinding().SlateUserIndex);
			// Ensure the route is not already focused so the arm actually requests the focus and the
			// synchronous focus callback is exercised.
			Slate.ClearUserFocus(User);

			const FCortexCommandResult Arm = Fixture->Session->BeginReplayEpoch();
			T.TestTrue(TEXT("Focus-change callback fired during the owned replay arm"),
				*bFocusCallbackFired);
			T.TestFalse(TEXT("Owned replay arm fails after synchronous focus cancellation"), Arm.bSuccess);
			T.TestFalse(TEXT("No replay epoch is armed after focus cancellation"),
				Fixture->Session->IsReplayEpochArmed());
			T.TestFalse(TEXT("Cancelled arm leaves no bound successor target"),
				Fixture->Session->GetTargetBinding().World.IsValid());

			Slate.OnFocusChanging().Remove(FocusHandle);
			PlaySettings->GameGetsMouseControl = bSavedGameGetsMouseControl;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputHumanReplayForeignInputTest,
	"Cortex.Editor.PhysicalInput.HumanReplayForeignInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputHumanReplayForeignInputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("HumanReplayForeignInput"),
		[Fixture](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
			F.Session->SetInterruptionCallback([WeakFixture](const FCortexCommandResult& Result)
			{
				if (const auto Pinned = WeakFixture.Pin())
				{
					++Pinned->InterruptionCount;
					Pinned->Interruption = Result;
				}
			});
			EnsureSelectedRouteWindowActive(*F.Session);
			const FCortexCommandResult Armed = F.Session->BeginReplayEpoch();
			Test.TestTrue(TEXT("Human replay epoch armed"), Armed.bSuccess);
			if (!Armed.bSuccess) { return; }

			const auto& Binding = F.Session->GetTargetBinding();
			const FModifierKeysState Modifiers;
			FSlateApplication::Get().ProcessKeyDownEvent(
				FKeyEvent(EKeys::E, Modifiers, Binding.InputDevice, false, 0, 0,
					Binding.SlateUserIndex));
			Test.TestEqual(TEXT("Human replay foreign packet interrupts before consumer commit"),
				F.InterruptionCount, 1);
			FSlateApplication::Get().ProcessKeyUpEvent(
				FKeyEvent(EKeys::E, Modifiers, Binding.InputDevice, false, 0, 0,
					Binding.SlateUserIndex));
		}, /*bInstallProbe=*/false, /*bArmCapture=*/true, /*bOpenMenu=*/false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockDeltaFidelityTest,
	"Cortex.Editor.PhysicalInput.NativeClockDeltaFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockDeltaFidelityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }
	struct FNativeDeltaProbe
	{
		FDelegateHandle WorldTickHandle;
		FCortexEditorEngineClockLease Clock;
		IConsoleVariable* FrameCap = nullptr;
		FString SavedFrameCap;
		uint32 SavedSetBy = 0;
		float ConsumedDeltas[4] = {};
		int32 Count = 0;

		~FNativeDeltaProbe()
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
			if (FrameCap)
			{
				FrameCap->Set(*SavedFrameCap, ECVF_SetByConsole);
				FrameCap->ClearFlags(ECVF_SetByMask);
				FrameCap->SetFlags(static_cast<EConsoleVariableFlags>(SavedSetBy));
			}
		}
	};
	const auto Probe = MakeShared<FNativeDeltaProbe>();
	Probe->FrameCap = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
	if (!TestNotNull(TEXT("Normal host FPS cap exists"), Probe->FrameCap))
	{
		return false;
	}
	Probe->SavedFrameCap = Probe->FrameCap->GetString();
	Probe->SavedSetBy = Probe->FrameCap->GetFlags() & ECVF_SetByMask;
	Probe->FrameCap->Set(120.0f, ECVF_SetByConsole);
	const FCortexCommandResult ClockResult = Probe->Clock.Acquire(*GEngine, 1,
		[](FCortexEditorAppClockStep& Step)
		{
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			FCortexCommandResult Result;
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Clock acquired (%s: %s)"),
		*ClockResult.ErrorCode, *ClockResult.ErrorMessage), ClockResult.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockDeltaFidelity"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			const TWeakPtr<FNativeDeltaProbe> WeakProbe = Probe;
			const TWeakObjectPtr<UWorld> SelectedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[WeakProbe, SelectedWorld](UWorld* World, ELevelTick, float DeltaSeconds)
				{
					if (World != SelectedWorld.Get()) { return; }
					if (const auto Pinned = WeakProbe.Pin())
					{
						if (Pinned->Count < UE_ARRAY_COUNT(Pinned->ConsumedDeltas))
						{
							Pinned->ConsumedDeltas[Pinned->Count++] = DeltaSeconds;
						}
					}
				});
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestEqual(TEXT("Four selected-world consumer frames observed"), Probe->Count, 4);
			for (int32 Index = 0; Index < Probe->Count; ++Index)
			{
				Test.TestEqual(FString::Printf(TEXT("Consumer frame %d uses recorded delta"), Index),
					Probe->ConsumedDeltas[Index], static_cast<float>(1.0 / 30.0), 0.0f);
			}
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Clock handoff admitted only after owned PIE ended"),
				Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Real default-clock handoff completed"), Probe->Clock.IsHandoffComplete());
			Test.TestTrue(TEXT("Default delta is bounded and positive"),
				FApp::GetDeltaTime() > 0.0 && FApp::GetDeltaTime() <= 0.117);
			Test.TestTrue(TEXT("Default current and last do not move backward"),
				FApp::GetCurrentTime() >= FApp::GetLastTime());
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockHandoffOwnershipTest,
	"Cortex.Editor.PhysicalInput.NativeClockHandoffOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockHandoffOwnershipTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	struct FClockFailureProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle WorldTickHandle;
		TWeakObjectPtr<UWorld> SelectedWorld;
		bool bRequestFailure = false;
		bool bFailureObserved = false;
		int32 SelectedTicksAfterFailure = 0;
		float LastExtraDeltaSeconds = 0.0f;

		~FClockFailureProbe()
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
		}
	};
	const auto Probe = MakeShared<FClockFailureProbe>();
	const TWeakPtr<FClockFailureProbe> WeakProbe = Probe;
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	// UE attaches the provider before Initialize returns: a failed initialization
	// must remove only that partial owned attachment and preserve the normal profile.
	const bool SavedFixedRate = GEngine->bUseFixedFrameRate;
	const bool SavedFixedStep = FApp::UseFixedTimeStep();
	const double SavedFixedDelta = FApp::GetFixedDeltaTime();
	Probe->Clock.SetForceInitializeFailureForTests(true);
	const FCortexCommandResult Partial = Probe->Clock.Acquire(*GEngine, 1,
		[](FCortexEditorAppClockStep&)
		{
			FCortexCommandResult Result;
			Result.bSuccess = true;
			return Result;
		});
	TestFalse(TEXT("Real partial initialization is rejected"), Partial.bSuccess);
	TestNull(TEXT("Failed owned attachment is removed"), GEngine->GetCustomTimeStep());
	TestEqual(TEXT("Partial initialization preserves engine fixed-rate setting"),
		!!GEngine->bUseFixedFrameRate, SavedFixedRate);
	TestEqual(TEXT("Partial initialization preserves application fixed-step setting"),
		FApp::UseFixedTimeStep(), SavedFixedStep);
	TestEqual(TEXT("Partial initialization preserves fixed delta"),
		FApp::GetFixedDeltaTime(), SavedFixedDelta);
	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 2,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto Pinned = WeakProbe.Pin();
			const auto OwnedFixture = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!Pinned.IsValid() || !OwnedFixture.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Native fault probe owner expired");
				return Result;
			}
			if (Pinned->bRequestFailure)
			{
				Pinned->bFailureObserved = true;
				OwnedFixture->Session->EndOwnedPIE();
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Controlled recorded-clock producer failure");
				return Result;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Owned clock admitted (%s: %s)"),
		*Acquired.ErrorCode, *Acquired.ErrorMessage), Acquired.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockHandoffOwnership"),
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestFalse(TEXT("A live PIE cannot enter cleanup-only handoff"), Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockFaultContainment"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Probe->SelectedWorld = Fixture->Session->GetTargetBinding().World;
			const TWeakPtr<FClockFailureProbe> Weak = Probe;
			Probe->WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[Weak](UWorld* World, ELevelTick, float DeltaSeconds)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bFailureObserved
						&& World == Pinned->SelectedWorld.Get())
					{
						++Pinned->SelectedTicksAfterFailure;
						Pinned->LastExtraDeltaSeconds = DeltaSeconds;
					}
				});
			Probe->bRequestFailure = true;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Real producer failure occurred before engine world processing"),
				Probe->bFailureObserved);
			Test.TestFalse(TEXT("Clock preserves the typed failure"), Probe->Clock.GetLastFailure().bSuccess);
			Test.TestEqual(FString::Printf(TEXT("No unrecorded selected-world tick after clock failure (extra delta %.9f)"),
				Probe->LastExtraDeltaSeconds), Probe->SelectedTicksAfterFailure, 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Failure teardown admits real clock handoff"),
				Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Failure cleanup observes the next normal clock frame"),
				Probe->Clock.IsHandoffComplete());
			Test.TestTrue(TEXT("Failure cleanup default clock remains bounded"),
				FApp::GetDeltaTime() > 0.0 && FApp::GetDeltaTime() <= 0.117);
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeSlateWaitTest,
	"Cortex.Editor.PhysicalInput.NativeSlateWaitNoWorldAdvance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeSlateWaitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	struct FSlateWaitProbe
	{
		FCortexEditorEngineFrameObserver Observer;
		bool bTimerRan = false;
		bool bNestedRejected = false;
	};
	const auto Probe = MakeShared<FSlateWaitProbe>();
	TestTrue(TEXT("Observer installed before owned request"),
		Probe->Observer.Install(*Fixture->Session,
			[](const FCortexEditorEngineFrameRecord&) {},
			[](const FCortexEditorEngineFrameRecord&) {}).bSuccess);
	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexDrivePhysicalInput(this, Fixture,
		TEXT("NativeSlateWaitNoWorldAdvance"),
		[Fixture, Probe](FAutomationTestBase& Test, FCortexEditorPhysicalInputTestFixture& F)
		{
			EnsureSelectedRouteWindowActive(*F.Session);
			UWorld* World = F.Session->GetTargetBinding().World.Get();
			if (!Test.TestNotNull(TEXT("Exact selected world"), World) || !F.Slider.IsValid())
			{
				return;
			}
			// The existing real tagged slider has cached geometry. Hide it, then permit
			// only Slate's active timer to make the recorded target ready again.
			const FVector2D Absolute = PhysicalTestWidgetAbsoluteCenter(*F.Slider);
			FCortexEditorPhysicalInputEvent Press;
			Press.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			Press.Key = EKeys::LeftMouseButton;
			Press.ViewportPosition = ToViewportLocal(F, Absolute);
			const auto Identity = PhysicalTestMakeSlateIdentity(
				TEXT("CortexPhysicalProbeRoot"), TEXT("CortexPhysicalProbeSlider"));
			F.Slider->SetVisibility(EVisibility::Collapsed);
			Test.TestTrue(TEXT("Hidden layout is refreshed using Slate-only work"),
				Probe->Observer.RunSlateOnlyWaitWork().bSuccess);
			FCortexEditorPhysicalInputUIObservation Before;
			F.Session->ObserveUI(Press, *Identity, Before);
			Test.TestTrue(TEXT("Hidden target is not ready"),
				Before.State != ECortexEditorUIObservationState::Ready);
			const uint64 FrameBefore = GFrameCounter;
			const double RealBefore = World->GetRealTimeSeconds();
			const double GameBefore = World->GetTimeSeconds();
			const float DeltaBefore = World->GetDeltaSeconds();
			const double AppCurrentBefore = FApp::GetCurrentTime();
			const double AppLastBefore = FApp::GetLastTime();
			const double AppDeltaBefore = FApp::GetDeltaTime();
			const int32 TicksBefore = Probe->Observer.GetSelectedWorldTickCount();
			FCortexEditorNativeMouseFilterState FilterBefore;
			Test.TestTrue(TEXT("Native filter observed before wait"),
				F.Session->ReadNativeMouseFilterState(FilterBefore).bSuccess);
			const TWeakPtr<FSlateWaitProbe> WeakProbe = Probe;
			const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
			F.Overlay->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda(
				[WeakProbe, WeakFixture](double, float)
				{
					const auto P = WeakProbe.Pin();
					const auto Owned = WeakFixture.Pin();
					if (P.IsValid() && Owned.IsValid())
					{
						P->bTimerRan = true;
						P->bNestedRejected = !P->Observer.RunSlateOnlyWaitWork().bSuccess;
						Owned->Slider->SetVisibility(EVisibility::Visible);
					}
					return EActiveTimerReturnType::Stop;
				}));
			const FCortexCommandResult Wait = Probe->Observer.RunSlateOnlyWaitWork();
			Test.TestTrue(FString::Printf(TEXT("Slate-only wait succeeds (%s: %s)"),
				*Wait.ErrorCode, *Wait.ErrorMessage), Wait.bSuccess);
			Test.TestTrue(TEXT("Actual Slate active timer made the target visible"), Probe->bTimerRan);
			Test.TestTrue(TEXT("Nested Slate work is rejected"), Probe->bNestedRejected);
			// Active timers run after the first prepass. The next permitted
			// Slate-only pass lays out the newly-visible control.
			Test.TestTrue(TEXT("Newly visible target receives its next layout pass"),
				Probe->Observer.RunSlateOnlyWaitWork().bSuccess);
			FCortexEditorPhysicalInputUIObservation After;
			const FCortexCommandResult Observed = F.Session->ObserveUI(Press, *Identity, After);
			Test.TestTrue(TEXT("Delayed real target resolves"), Observed.bSuccess);
			Test.TestEqual(TEXT("Delayed target is ready without a world tick"),
				After.State, ECortexEditorUIObservationState::Ready);
			Test.TestEqual(TEXT("Global frame unchanged"), GFrameCounter, FrameBefore);
			Test.TestEqual(TEXT("Selected world tick count unchanged"),
				Probe->Observer.GetSelectedWorldTickCount(), TicksBefore);
			Test.TestEqual(TEXT("World real time unchanged"), World->GetRealTimeSeconds(), RealBefore);
			Test.TestEqual(TEXT("World game time unchanged"), World->GetTimeSeconds(), GameBefore);
			Test.TestEqual(TEXT("World delta unchanged"), World->GetDeltaSeconds(), DeltaBefore, 0.0f);
			Test.TestEqual(TEXT("App current unchanged"), FApp::GetCurrentTime(), AppCurrentBefore);
			Test.TestEqual(TEXT("App last unchanged"), FApp::GetLastTime(), AppLastBefore);
			Test.TestEqual(TEXT("App delta unchanged"), FApp::GetDeltaTime(), AppDeltaBefore);
			FCortexEditorNativeMouseFilterState FilterAfter;
			Test.TestTrue(TEXT("Native filter observed after wait"),
				F.Session->ReadNativeMouseFilterState(FilterAfter).bSuccess);
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				Test.TestEqual(TEXT("Native zero time unchanged"),
					FilterAfter.ZeroTimeSeconds[Axis], FilterBefore.ZeroTimeSeconds[Axis], 0.0f);
				Test.TestEqual(TEXT("Native smoothed mouse unchanged"),
					FilterAfter.SmoothedMouse[Axis], FilterBefore.SmoothedMouse[Axis], 0.0f);
			}
			Test.TestEqual(TEXT("Native mouse samples unchanged"),
				FilterAfter.SampleCount, FilterBefore.SampleCount);
			Test.TestEqual(TEXT("Native sampling total unchanged"),
				FilterAfter.SamplingTotalSeconds, FilterBefore.SamplingTotalSeconds, 0.0f);
			Test.TestEqual(TEXT("Effective dilation unchanged"),
				FilterAfter.EffectiveTimeDilation, FilterBefore.EffectiveTimeDilation, 0.0f);
			// A target with no Slate-side producer stays pending: waiting does not
			// manufacture readiness by ticking the game world.
			F.Slider->SetVisibility(EVisibility::Collapsed);
			Test.TestTrue(TEXT("A second Slate-only pass remains safe"),
				Probe->Observer.RunSlateOnlyWaitWork().bSuccess);
			FCortexEditorPhysicalInputUIObservation WorldDependent;
			F.Session->ObserveUI(Press, *Identity, WorldDependent);
			Test.TestTrue(TEXT("No game producer means the target remains unready"),
				WorldDependent.State != ECortexEditorUIObservationState::Ready);
			Test.TestEqual(TEXT("Waiting did not advance game time"),
				World->GetTimeSeconds(), GameBefore);
		}, true, false, true));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase&) { Probe->Observer.Uninstall(); }));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeFirstWorldFrameTest,
	"Cortex.Editor.PhysicalInput.NativeFirstWorldFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeFirstWorldFrameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	// One shared, game-thread-confined probe owns both the native observer and the
	// independent engine observations, so latent commands keep them alive across
	// frames and teardown is explicit and ordered.
	struct FFirstWorldProbe
	{
		FCortexEditorEngineFrameObserver Observer;

		// ---- independent engine observation (never routed through the observer) ----
		FDelegateHandle BeginFrameHandle;
		FDelegateHandle SamplingHandle;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;

		struct FSampledClock
		{
			double DeltaSeconds = 0.0;
			double CurrentSeconds = 0.0;
			double LastSeconds = 0.0;
			bool bPIEWorldPresent = false;
		};
		TMap<uint64, FSampledClock> SampledClocks;
		bool bDuplicateSampling = false;

		// Independent per-global-frame count of the selected world's real ticks (zero/duplicate invariant).
		TMap<uint64, int32> SelectedTicksPerFrame;
		bool bDuplicateTickFrame = false;
		// Set if the observer's latched last-closed record is mutated after it was delivered.
		bool bLatchedFrameMutated = false;

		bool bFirstTickObserved = false;
		TWeakObjectPtr<UWorld> FirstTickWorld;
		uint64 FirstTickFrameCounter = 0;
		float FirstTickRealDelta = 0.0f;
		float FirstTickEndDelta = 0.0f;
		ELevelTick FirstTickLevelType = LEVELTICK_All;
		bool bFirstTickEnded = false;
		int32 SelectedTickCount = 0;

		// ---- observer callbacks ----
		struct FBoundarySnapshot
		{
			bool bValid = false;
			bool bWorldBound = false;
			bool bPawnBound = false;
			bool bReadySeen = false;
			double AppDeltaSeconds = 0.0;
			double AppCurrentSeconds = 0.0;
			double AppLastSeconds = 0.0;
			double InputBoundarySeconds = 0.0;
		};
		TMap<uint64, FBoundarySnapshot> BoundarySnapshots;

		TArray<FCortexEditorEngineFrameRecord> ClosedFrames;
		int32 FirstSelectedClosedIndex = INDEX_NONE;
		FCortexEditorEngineFrameRecord BirthFrame;
		bool bReadyLatched = false;
		int32 ReadyLatchedClosedFrameCount = 0;

		void RemoveIndependentDelegates()
		{
			FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
			FCoreDelegates::OnSamplingInput.Remove(SamplingHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
			BeginFrameHandle.Reset();
			SamplingHandle.Reset();
			WorldStartHandle.Reset();
			WorldEndHandle.Reset();
		}

		void Shutdown()
		{
			Observer.Uninstall();
			RemoveIndependentDelegates();
		}

		~FFirstWorldProbe()
		{
			Shutdown();
		}
	};

	const auto Probe = MakeShared<FFirstWorldProbe>();
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;

	// Independent observers in place before the native observer so they see the same frames.
	Probe->SamplingHandle = FCoreDelegates::OnSamplingInput.AddLambda([Probe]()
	{
		const uint64 Counter = GFrameCounter;
		if (Probe->SampledClocks.Contains(Counter))
		{
			// The native observer assumes exactly one OnSamplingInput per global frame; record
			// any independent evidence that this assumption was violated.
			Probe->bDuplicateSampling = true;
			return;
		}
		FFirstWorldProbe::FSampledClock Clock;
		Clock.DeltaSeconds = FApp::GetDeltaTime();
		Clock.CurrentSeconds = FApp::GetCurrentTime();
		Clock.LastSeconds = FApp::GetLastTime();
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			Clock.bPIEWorldPresent |= Context.WorldType == EWorldType::PIE
				&& Context.World() != nullptr;
		}
		Probe->SampledClocks.Add(Counter, Clock);
	});
	Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[Probe](UWorld* World, ELevelTick TickType, float DeltaSeconds)
		{
			if (!World || World->WorldType != EWorldType::PIE) { return; }
			if (!Probe->bFirstTickObserved)
			{
				Probe->bFirstTickObserved = true;
				Probe->FirstTickWorld = World;
				Probe->FirstTickFrameCounter = GFrameCounter;
				Probe->FirstTickRealDelta = DeltaSeconds;
				Probe->FirstTickLevelType = TickType;
			}
			if (World == Probe->FirstTickWorld.Get())
			{
				++Probe->SelectedTickCount;
				int32& FrameTicks = Probe->SelectedTicksPerFrame.FindOrAdd(GFrameCounter);
				++FrameTicks;
				if (FrameTicks > 1)
				{
					Probe->bDuplicateTickFrame = true;
				}
			}
		});
	Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
		[Probe](UWorld* World, ELevelTick, float DeltaSeconds)
		{
			if (!Probe->bFirstTickEnded && World == Probe->FirstTickWorld.Get())
			{
				Probe->bFirstTickEnded = true;
				Probe->FirstTickEndDelta = DeltaSeconds;
			}
		});

	// Latched-frame integrity probe: registered before the observer's own BeginFrame so it runs
	// first and can observe whether the just-closed record was mutated after it was delivered
	// (the observer invalidates only the rolling current record, so a stray world tick outside an
	// open frame would corrupt the record a consumer already read).
	Probe->BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddLambda([Probe]()
	{
		const FCortexEditorEngineFrameRecord* Live = Probe->Observer.GetLastClosedFrame();
		if (Live != nullptr && Probe->ClosedFrames.Num() > 0)
		{
			const FCortexEditorEngineFrameRecord& Copy = Probe->ClosedFrames.Last();
			if (Live->CaptureFrameCounter != Copy.CaptureFrameCounter
				|| Live->SelectedWorldTickCount != Copy.SelectedWorldTickCount
				|| Live->WorldTick.IsSet() != Copy.WorldTick.IsSet())
			{
				Probe->bLatchedFrameMutated = true;
			}
		}
	});

	// ---- install the native observer BEFORE the owned PIE request ----
	const FCortexCommandResult Installed = Probe->Observer.Install(*Fixture->Session,
		[Probe, WeakFixture](const FCortexEditorEngineFrameRecord& Record)
		{
			FFirstWorldProbe::FBoundarySnapshot Snapshot;
			Snapshot.bValid = true;
			Snapshot.InputBoundarySeconds = Record.InputBoundarySeconds;
			Snapshot.AppDeltaSeconds = Record.AppDeltaSeconds;
			Snapshot.AppCurrentSeconds = Record.AppCurrentSeconds;
			Snapshot.AppLastSeconds = Record.AppLastSeconds;
			if (const auto Pinned = WeakFixture.Pin())
			{
				Snapshot.bReadySeen = Pinned->bReadySeen;
				Snapshot.bWorldBound = Pinned->Session->GetTargetBinding().World.IsValid();
				Snapshot.bPawnBound = Pinned->Session->GetTargetBinding().Pawn.IsValid();
			}
			Probe->BoundarySnapshots.Add(Record.CaptureFrameCounter, Snapshot);
		},
		[Probe](const FCortexEditorEngineFrameRecord& Record)
		{
			Probe->ClosedFrames.Add(Record);
			if (Probe->FirstSelectedClosedIndex == INDEX_NONE && Record.SelectedWorldTickCount > 0)
			{
				Probe->FirstSelectedClosedIndex = Probe->ClosedFrames.Num() - 1;
				Probe->BirthFrame = Record;
			}
		});
	if (!TestTrue(FString::Printf(TEXT("Native observer installs before the owned PIE request (%s: %s)"),
		*Installed.ErrorCode, *Installed.ErrorMessage), Installed.bSuccess))
	{
		Probe->Shutdown();
		return false;
	}
	TestTrue(TEXT("Observer has no selected world before the owned request"),
		!Probe->Observer.HasSelectedWorld());

	// Readiness latch wrapper: record how many frames had already closed so the birth frame can
	// be proven to precede the ready target.
	const TFunction<void(const FCortexCommandResult&)> InnerReady = MakeFixtureReadyCallback(Fixture);
	TFunction<void(const FCortexCommandResult&)> ReadyCallback =
		[InnerReady, Probe](const FCortexCommandResult& Ready)
		{
			Probe->bReadyLatched = true;
			Probe->ReadyLatchedClosedFrameCount = Probe->ClosedFrames.Num();
			InnerReady(Ready);
		};
	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MoveTemp(ReadyCallback)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));

	// Real controlled late observer installation while the owned PIE is already running.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeFirstWorldFrameLateInstall"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorEngineFrameObserver LateObserver;
			const FCortexCommandResult Late = LateObserver.Install(*Fixture->Session,
				[](const FCortexEditorEngineFrameRecord&) {},
				[](const FCortexEditorEngineFrameRecord&) {});
			Test.TestFalse(TEXT("A fresh late observer cannot adopt a running owned PIE"), Late.bSuccess);
			Test.TestEqual(TEXT("Late install reports the native timing error"),
				Late.ErrorCode, FString(TEXT("REPLAY_TIMING_ERROR")));
			LateObserver.Uninstall();

			const FCortexCommandResult Reinstall = Probe->Observer.Install(*Fixture->Session,
				[](const FCortexEditorEngineFrameRecord&) {},
				[](const FCortexEditorEngineFrameRecord&) {});
			Test.TestFalse(TEXT("The installed observer cannot reinstall itself"), Reinstall.bSuccess);
			Test.TestEqual(TEXT("Reinstall reports the native timing error"),
				Reinstall.ErrorCode, FString(TEXT("REPLAY_TIMING_ERROR")));

			Test.TestTrue(TEXT("Observer pinned its selected world at birth"),
				Probe->Observer.HasSelectedWorld());
			Test.TestEqual(TEXT("Observer selected world is the session's exact owned world"),
				Probe->Observer.GetSelectedWorld(), Fixture->Session->GetTargetBinding().World.Get());
			const FCortexEditorEngineFrameRecord* LastClosed = Probe->Observer.GetLastClosedFrame();
			Test.TestNotNull(TEXT("Observer exposes a last closed frame"), LastClosed);
			if (LastClosed != nullptr && Probe->ClosedFrames.Num() > 0)
			{
				Test.TestEqual(TEXT("Last-closed getter matches the recorded close"),
					static_cast<int64>(LastClosed->CaptureFrameCounter),
					static_cast<int64>(Probe->ClosedFrames.Last().CaptureFrameCounter));
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			// ---- independent observation present ----
			Test.TestTrue(TEXT("Independent probe observed the first owned PIE world tick"),
				Probe->bFirstTickObserved);
			Test.TestTrue(TEXT("Independent probe observed that tick's end"),
				Probe->bFirstTickEnded);
			Test.TestFalse(TEXT("OnSamplingInput fired at most once per global frame during the run"),
				Probe->bDuplicateSampling);
			Test.TestTrue(TEXT("Independent probe counted the selected world's ticks"),
				Probe->SelectedTickCount >= 1);
			Test.TestEqual(TEXT("Independent tick type is a full native level tick"),
				static_cast<int32>(Probe->FirstTickLevelType), static_cast<int32>(LEVELTICK_All));

			// ---- observer never invalidated a frame: no duplicate/missing/null/foreign topology ----
			Test.TestTrue(TEXT("Observer closed at least one frame"), Probe->ClosedFrames.Num() > 0);
			int64 PreviousCounter = -1;
			bool bMonotonic = true;
			bool bAllBoundary = true;
			bool bAllEnd = true;
			bool bAnyInvalid = false;
			for (const FCortexEditorEngineFrameRecord& Frame : Probe->ClosedFrames)
			{
				if (PreviousCounter >= 0
					&& static_cast<int64>(Frame.CaptureFrameCounter) <= PreviousCounter)
				{
					bMonotonic = false;
				}
				PreviousCounter = static_cast<int64>(Frame.CaptureFrameCounter);
				bAllBoundary &= Frame.bBoundaryObserved;
				bAllEnd &= Frame.bEndObserved;
				bAnyInvalid |= Frame.SelectedWorldTickCount < 0;
			}
			Test.TestTrue(TEXT("Closed frame counters strictly increase (no duplicate/out-of-order boundary)"),
				bMonotonic);
			Test.TestTrue(TEXT("Every closed frame retained its sampled boundary"), bAllBoundary);
			Test.TestTrue(TEXT("Every closed frame closed after its boundary"), bAllEnd);
			Test.TestFalse(TEXT("No frame was invalidated (no duplicate/missing/null/foreign topology detected)"),
				bAnyInvalid);

			// ---- runtime zero/duplicate tick invariant against independent per-frame ticks ----
			TArray<FString> TickMismatches;
			for (const FCortexEditorEngineFrameRecord& Frame : Probe->ClosedFrames)
			{
				const int32* Independent = Probe->SelectedTicksPerFrame.Find(Frame.CaptureFrameCounter);
				const int32 IndependentCount = Independent != nullptr ? *Independent : 0;
				if (Frame.SelectedWorldTickCount != IndependentCount)
				{
					TickMismatches.Add(FString::Printf(TEXT("frame %llu observer %d independent %d"),
						static_cast<unsigned long long>(Frame.CaptureFrameCounter),
						Frame.SelectedWorldTickCount, IndependentCount));
				}
			}
			FString TickMismatchDetail;
			if (TickMismatches.Num() > 0)
			{
				TickMismatchDetail = FString::Printf(TEXT(" (mismatches: %s)"),
					*FString::Join(TickMismatches, TEXT("; ")));
			}
			Test.TestTrue(FString::Printf(
				TEXT("Observer zero/duplicate tick channel matches independent per-frame selected ticks%s"),
				*TickMismatchDetail), TickMismatches.Num() == 0);
			Test.TestFalse(TEXT("No global frame carried duplicate selected-world ticks"),
				Probe->bDuplicateTickFrame);
			Test.TestFalse(TEXT("The latched last-closed frame was never mutated after close"),
				Probe->bLatchedFrameMutated);

			// ---- recording-local ordinal 0: the first selected-world frame ----
			if (!Test.TestTrue(TEXT("Observer attributed a selected-world frame"),
				Probe->FirstSelectedClosedIndex >= 0))
			{
				return;
			}
			for (int32 Index = 0; Index < Probe->FirstSelectedClosedIndex; ++Index)
			{
				Test.TestEqual(TEXT("Pre-birth frame carries a null world tick (distinct from zero delta)"),
					Probe->ClosedFrames[Index].SelectedWorldTickCount, 0);
				Test.TestFalse(TEXT("Pre-birth frame has no world tick record"),
					Probe->ClosedFrames[Index].WorldTick.IsSet());
			}
			const FCortexEditorEngineFrameRecord& Birth = Probe->BirthFrame;
			Test.TestEqual(TEXT("Birth frame is recording-local ordinal 0: exactly one selected tick"),
				Birth.SelectedWorldTickCount, 1);
			if (!Test.TestTrue(TEXT("Birth frame carries the world tick record"), Birth.WorldTick.IsSet()))
			{
				return;
			}

			// ---- identity: observer, independent observation and session agree on the world ----
			Test.TestTrue(TEXT("Independent first PIE tick is the observer's selected world"),
				Probe->FirstTickWorld.Get() == Probe->Observer.GetSelectedWorld());
			Test.TestEqual(TEXT("Observer selected world is the session's exact owned world"),
				Probe->Observer.GetSelectedWorld(), Fixture->Session->GetTargetBinding().World.Get());
			Test.TestEqual(TEXT("Birth frame is the same global frame as the independent first tick"),
				static_cast<int64>(Birth.CaptureFrameCounter),
				static_cast<int64>(Probe->FirstTickFrameCounter));

			// ---- native consumed delta vs observer boundary and independent observation ----
			Test.TestEqual(TEXT("Birth frame native consumed delta matches the independent world tick"),
				Birth.WorldTick->RealDeltaSeconds, Probe->FirstTickRealDelta, 0.0f);
			Test.TestEqual(TEXT("Birth frame world end delta matches the independent observation"),
				Birth.WorldTick->DeltaSeconds, Probe->FirstTickEndDelta, 0.0f);
			Test.TestEqual(TEXT("Sampled app clock reaches the selected world tick unchanged"),
				Birth.WorldTick->RealDeltaSeconds, static_cast<float>(Birth.AppDeltaSeconds), 0.0f);
			Test.TestEqual(TEXT("Birth frame world tick type is a full native level tick"),
				Birth.WorldTick->TickType.ToString(), FString(TEXT("LEVELTICK_All")));
			Test.TestFalse(TEXT("Birth frame world tick is not paused"), Birth.WorldTick->bPaused);
			Test.TestTrue(TEXT("Birth frame effective dilation is positive"),
				Birth.WorldTick->EffectiveTimeDilation > 0.0f);

			// ---- observer baseline origin is the birth frame's own pre-tick instant ----
			Test.TestEqual(TEXT("Birth frame real-time offset is its own consumed real delta"),
				Birth.WorldTick->RealTimeOffsetSeconds,
				static_cast<double>(Birth.WorldTick->RealDeltaSeconds), 0.0);
			Test.TestEqual(TEXT("Birth frame time offset is its own consumed world delta"),
				Birth.WorldTick->TimeOffsetSeconds,
				static_cast<double>(Birth.WorldTick->DeltaSeconds), 0.0);

			// ---- boundary sample identity and the world-born-after-sampling contract ----
			if (const FFirstWorldProbe::FBoundarySnapshot* Snapshot =
				Probe->BoundarySnapshots.Find(Birth.CaptureFrameCounter))
			{
				Test.TestFalse(TEXT("Birth frame was sampled before the owned world existed"),
					Snapshot->bWorldBound);
				Test.TestFalse(TEXT("Birth frame was sampled before the owned pawn was bound"),
					Snapshot->bPawnBound);
				Test.TestFalse(TEXT("Birth frame boundary precedes the readiness latch"),
					Snapshot->bReadySeen);
				Test.TestTrue(TEXT("Frame begin precedes its sampled input boundary"),
					Birth.FrameBeginSeconds <= Snapshot->InputBoundarySeconds);
				Test.TestEqual(TEXT("Closed birth frame retains the sampled app delta"),
					Birth.AppDeltaSeconds, Snapshot->AppDeltaSeconds, 0.0);
				Test.TestEqual(TEXT("Closed birth frame retains the sampled app current"),
					Birth.AppCurrentSeconds, Snapshot->AppCurrentSeconds, 0.0);
				Test.TestEqual(TEXT("Closed birth frame retains the sampled app last"),
					Birth.AppLastSeconds, Snapshot->AppLastSeconds, 0.0);
			}
			else
			{
				Test.AddError(TEXT("Birth frame boundary snapshot missing"));
			}

			// ---- independent FApp clock sample for the same global frame ----
			if (const FFirstWorldProbe::FSampledClock* Clock =
				Probe->SampledClocks.Find(Birth.CaptureFrameCounter))
			{
				Test.TestFalse(TEXT("Actual PIE world is born after this frame's sampling"),
					Clock->bPIEWorldPresent);
				Test.TestEqual(TEXT("Observer app delta equals the independent FApp sample"),
					Birth.AppDeltaSeconds, Clock->DeltaSeconds, 0.0);
				Test.TestEqual(TEXT("Observer app current equals the independent FApp sample"),
					Birth.AppCurrentSeconds, Clock->CurrentSeconds, 0.0);
				Test.TestEqual(TEXT("Observer app last equals the independent FApp sample"),
					Birth.AppLastSeconds, Clock->LastSeconds, 0.0);
			}
			else
			{
				Test.AddError(TEXT("Independent FApp sample for the birth frame is missing"));
			}

			Test.TestTrue(TEXT("Readiness latched after the observer was installed"),
				Probe->bReadyLatched);
		}));

	// Explicit teardown before the fixture shuts its session down (observer holds a raw session
	// pointer until Uninstall clears it).
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase&)
		{
			Probe->Shutdown();
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockExternalClearTest,
	"Cortex.Editor.PhysicalInput.NativeClockExternalClearContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockExternalClearTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	struct FClearProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle SamplingHandle;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;
		TWeakObjectPtr<UWorld> SelectedWorld;
		bool bSelectedWorldCaptured = false;
		int32 SelectedTicksAfterLoss = 0;
		int32 SelectedTickEndsAfterLoss = 0;
		int32 StepCount = 0;
		bool bCleared = false;
		bool bFirstDefaultObserved = false;
		double FirstDefaultDelta = 0.0;
		double FirstDefaultCurrent = 0.0;
		double FirstDefaultLast = 0.0;
		double CleanupBaseCurrent = 0.0;
		double FixedCleanupCurrent = 0.0;
		bool bFixedCleanupObserved = false;
		bool SavedFixedRate = false;
		bool SavedFixedStep = false;
		double SavedFixedDelta = 0.0;
		~FClearProbe()
		{
			FCoreDelegates::OnSamplingInput.Remove(SamplingHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		}
	};
	const auto Probe = MakeShared<FClearProbe>();
	Probe->SavedFixedRate = GEngine->bUseFixedFrameRate;
	Probe->SavedFixedStep = FApp::UseFixedTimeStep();
	Probe->SavedFixedDelta = FApp::GetFixedDeltaTime();
	const TWeakPtr<FClearProbe> WeakProbe = Probe;
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (P.IsValid() && P->bCleared && World == P->SelectedWorld.Get())
			{
				++P->SelectedTicksAfterLoss;
			}
		});
	Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (P.IsValid() && P->bCleared && World == P->SelectedWorld.Get())
			{
				++P->SelectedTickEndsAfterLoss;
			}
		});
	Probe->SamplingHandle = FCoreDelegates::OnSamplingInput.AddLambda([WeakProbe]()
	{
		const auto P = WeakProbe.Pin();
		if (P.IsValid() && P->bCleared && GEngine->GetCustomTimeStep() == nullptr)
		{
			constexpr double CleanupDelta = static_cast<double>(static_cast<float>(1.0 / 60.0));
			if (FApp::UseFixedTimeStep()
				&& FApp::GetDeltaTime() == CleanupDelta
				&& FApp::GetLastTime() == P->CleanupBaseCurrent
				&& FApp::GetCurrentTime() == P->CleanupBaseCurrent + CleanupDelta)
			{
				P->bFixedCleanupObserved = true;
				P->FixedCleanupCurrent = FApp::GetCurrentTime();
			}
			else if (!FApp::UseFixedTimeStep() && !P->bFirstDefaultObserved)
			{
				P->bFirstDefaultObserved = true;
				P->FirstDefaultDelta = FApp::GetDeltaTime();
				P->FirstDefaultCurrent = FApp::GetCurrentTime();
				P->FirstDefaultLast = FApp::GetLastTime();
			}
		}
	});
	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 3,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			const auto Owned = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid() || !Owned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Provider-clear probe owner expired");
				return Result;
			}
			if (++P->StepCount == 40)
			{
				// End the exact owned consumer before external provider loss. The
				// engine still has its normal deferred PIE destruction to perform.
				P->SelectedWorld = Owned->Session->GetTargetBinding().World;
				P->bSelectedWorldCaptured = P->SelectedWorld.IsValid();
				Owned->Session->EndOwnedPIE();
				P->bCleared = true;
				P->CleanupBaseCurrent = FApp::GetCurrentTime();
				GEngine->SetCustomTimeStep(nullptr);
				Result.bSuccess = true;
				return Result;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(TEXT("Sustained clock acquired"), Acquired.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 48,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Actual provider was cleared after forty recorded updates"), Probe->bCleared);
			Test.TestTrue(TEXT("Exact selected world captured before binding invalidation"), Probe->bSelectedWorldCaptured);
			Test.TestEqual(TEXT("No selected-world tick admitted after loss"), Probe->SelectedTicksAfterLoss, 0);
			Test.TestEqual(TEXT("No selected-world tick ended after loss"), Probe->SelectedTickEndsAfterLoss, 0);
			Test.TestTrue(TEXT("Actual fixed cleanup was observed"), Probe->bFixedCleanupObserved);
			Test.TestTrue(TEXT("Actual resumed default frame was observed"), Probe->bFirstDefaultObserved);
			constexpr double CleanupDelta = static_cast<double>(static_cast<float>(1.0 / 60.0));
			Test.TestTrue(FString::Printf(TEXT("First default delta after sustained provider clear is positive and bounded: %.9f"),
				Probe->FirstDefaultDelta), FMath::IsFinite(Probe->FirstDefaultDelta)
				&& Probe->FirstDefaultDelta > 0.0 && Probe->FirstDefaultDelta <= 0.100 + CleanupDelta);
			Test.TestTrue(TEXT("First default current/last remain non-backward from cleanup"),
				Probe->FirstDefaultLast >= Probe->FixedCleanupCurrent
				&& Probe->FirstDefaultCurrent >= Probe->FirstDefaultLast);
			Test.TestEqual(TEXT("Provider loss remains truthfully Lost"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
			Test.TestNull(TEXT("No provider resurrected after external clear"), GEngine->GetCustomTimeStep());
			Test.TestEqual(TEXT("Original engine fixed-rate setting unchanged"),
				!!GEngine->bUseFixedFrameRate, Probe->SavedFixedRate);
			Test.TestEqual(TEXT("Original fixed-step setting unchanged"), FApp::UseFixedTimeStep(), Probe->SavedFixedStep);
			Test.TestEqual(TEXT("Original fixed delta unchanged"), FApp::GetFixedDeltaTime(), Probe->SavedFixedDelta);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Repeated lost cleanup is admitted without resurrecting ownership"),
				Probe->Clock.BeginHandoff().bSuccess);
			Test.TestEqual(TEXT("Cleanup never fabricates Released after loss"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
		}));
	return true;
}

namespace
{
/** Alternating recorded application deltas the owned clock lease reproduces here. */
constexpr double CortexNativePacketSampleDeltas[3] = { 1.0 / 30.0, 1.0 / 120.0, 1.0 / 45.0 };

/**
 * Owns the exclusive recorded-frame clock and the native observations of one packet bundle.
 *
 * The selected-world tick-start hook is the exact native boundary that runs after the engine's
 * pointer flush (UPlayerInput::KeyStateMap accumulators are live) and before that frame's
 * UPlayerInput::ProcessInputStack flushes them into the consumer-visible RawValue/Value and the
 * PlayerController applies the axis to the control rotation. Reading there yields the real
 * per-axis sample counts; the immediately following boundary yields the flushed raw values.
 */
struct FCortexNativePacketSampleProbe
{
	FCortexEditorEngineClockLease Clock;
	int32 DeltaIndex = 0;

	TWeakObjectPtr<UWorld> SelectedWorld;
	TWeakObjectPtr<APlayerController> BoundController;
	FDelegateHandle WorldEndHandle;
	bool bSeenRecordedDelta[3] = {};
	bool bUnexpectedRecordedDelta = false;
	bool bControllerChanged = false;
	float ConsumedWorldDeltas[3] = {};
	float ConsumedDilations[3] = {};
	FDelegateHandle WorldTickHandle;

	bool bArmed = false;

	// Sample-frame boundary (accumulators live, pre-flush).
	bool bSawSamples = false;
	int32 SamplesX = -1;
	int32 SamplesY = -1;
	double RawAccX = 0.0;
	double RawAccY = 0.0;
	double SampleAppDelta = 0.0;
	float SampleWorldDelta = 0.0f;
	FRotator RotationBeforeInput = FRotator::ZeroRotator;

	// Immediately following selected-world boundary (the sample frame has flushed).
	bool bFlushObserved = false;
	double FlushedRawX = 0.0;
	double FlushedRawY = 0.0;
	double ConsumedX = 0.0;
	double ConsumedY = 0.0;
	double SmoothedXAtFlush = 0.0;
	double ZeroTimeXAtFlush = 0.0;
	FRotator RotationAfterInput = FRotator::ZeroRotator;

	// Baseline native state + live identity captured on the exact bound instance.
	bool bBaselineRead = false;
	FCortexEditorNativeMouseFilterState BaselineFilter;
	FString BaselineClass;
	FString BaselineSha;
	TWeakObjectPtr<UPlayerInput> BoundInput;
	FVector2D DownViewportPosition = FVector2D::ZeroVector;

	void RemoveHook()
	{
		if (WorldTickHandle.IsValid())
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickHandle);
			WorldTickHandle.Reset();
		}
		FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		WorldEndHandle.Reset();
	}

	~FCortexNativePacketSampleProbe()
	{
		RemoveHook();
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativePacketSamplesTest,
	"Cortex.Editor.PhysicalInput.NativePacketSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexPhysicalInputNativePacketSamplesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Slate must be initialized for the native feasibility gate"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	const auto Probe = MakeShared<FCortexNativePacketSampleProbe>();
	const TWeakPtr<FCortexNativePacketSampleProbe> WeakProbe = Probe;

	// Acquire the recorded-frame clock before requesting the owned PIE, alternating the deltas.
	const FCortexCommandResult ClockResult = Probe->Clock.Acquire(*GEngine, 7,
		[WeakProbe](FCortexEditorAppClockStep& Step)
		{
			const TSharedPtr<FCortexNativePacketSampleProbe> Pinned = WeakProbe.Pin();
			FCortexCommandResult Result;
			if (!Pinned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Native packet sample probe owner expired");
				return Result;
			}
			const double Delta = CortexNativePacketSampleDeltas[
				Pinned->DeltaIndex++ % UE_ARRAY_COUNT(CortexNativePacketSampleDeltas)];
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + Delta;
			Step.DeltaSeconds = Delta;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Recorded clock admitted (%s: %s)"),
		*ClockResult.ErrorCode, *ClockResult.ErrorMessage), ClockResult.bSuccess))
	{
		return false;
	}

	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));

	// Install the native boundary hook and read the exact baseline native state/identity.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativePacketSamplesInstall"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			APlayerController* const Controller = F.Session->GetTargetBinding().Controller.Get();
			UPlayerInput* const PlayerInput = Controller ? Controller->PlayerInput : nullptr;
			Test.TestNotNull(TEXT("Bound controller exists"), Controller);
			if (Controller == nullptr || PlayerInput == nullptr)
			{
				return;
			}
			Probe->SelectedWorld = F.Session->GetTargetBinding().World;
			Probe->BoundController = Controller;

			Test.TestTrue(TEXT("Baseline native mouse-filter state read"),
				F.Session->ReadNativeMouseFilterState(Probe->BaselineFilter).bSuccess);
			Test.TestTrue(TEXT("Baseline live input config read"),
				F.Session->ReadNativeInputConfig(Probe->BaselineClass, Probe->BaselineSha).bSuccess);
			Probe->bBaselineRead = true;
			Probe->BoundInput = PlayerInput;
			Test.TestEqual(TEXT("Live PlayerInput class identity matches the bound instance"),
				Probe->BaselineClass, PlayerInput->GetClass()->GetPathName());

			const TWeakPtr<FCortexNativePacketSampleProbe> LocalWeak = Probe;
			Probe->WorldTickHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[LocalWeak](UWorld* World, ELevelTick, float DeltaSeconds)
				{
					const TSharedPtr<FCortexNativePacketSampleProbe> Pinned = LocalWeak.Pin();
					if (!Pinned.IsValid() || World != Pinned->SelectedWorld.Get())
					{
						return;
					}
					APlayerController* const TickController = Pinned->BoundController.Get();
					UPlayerInput* const TickInput = TickController ? TickController->PlayerInput : nullptr;
					if (TickController == nullptr || TickController->GetWorld() != World
						|| TickInput != Pinned->BoundInput.Get())
					{
						Pinned->bControllerChanged = true;
						return;
					}
					const FKeyState* const XState = TickInput->GetKeyState(EKeys::MouseX);
					const FKeyState* const YState = TickInput->GetKeyState(EKeys::MouseY);
					if (XState == nullptr || YState == nullptr)
					{
						return;
					}
					if (Pinned->bArmed && !Pinned->bSawSamples
						&& (XState->SampleCountAccumulator > 0 || YState->SampleCountAccumulator > 0))
					{
						Pinned->bSawSamples = true;
						Pinned->SamplesX = XState->SampleCountAccumulator;
						Pinned->SamplesY = YState->SampleCountAccumulator;
						Pinned->RawAccX = XState->RawValueAccumulator.X;
						Pinned->RawAccY = YState->RawValueAccumulator.X;
						Pinned->SampleAppDelta = FApp::GetDeltaTime();
						Pinned->SampleWorldDelta = DeltaSeconds;
						Pinned->RotationBeforeInput = TickController->GetControlRotation();
					}
					else if (Pinned->bSawSamples && !Pinned->bFlushObserved)
					{
						Pinned->bFlushObserved = true;
						Pinned->FlushedRawX = XState->RawValue.X;
						Pinned->FlushedRawY = YState->RawValue.X;
						Pinned->ConsumedX = TickInput->GetKeyValue(EKeys::MouseX);
						Pinned->ConsumedY = TickInput->GetKeyValue(EKeys::MouseY);
						Pinned->SmoothedXAtFlush = TickInput->SmoothedMouse[0];
						Pinned->ZeroTimeXAtFlush = TickInput->ZeroTime[0];
						Pinned->RotationAfterInput = TickController->GetControlRotation();
					}
				});
			Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
				[LocalWeak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = LocalWeak.Pin();
					if (!Pinned.IsValid() || World != Pinned->SelectedWorld.Get())
					{
						return;
					}
					bool bMatched = false;
					for (int32 Index = 0; Index < UE_ARRAY_COUNT(CortexNativePacketSampleDeltas); ++Index)
					{
						if (FApp::GetDeltaTime() == CortexNativePacketSampleDeltas[Index])
						{
							bMatched = true;
							Pinned->bSeenRecordedDelta[Index] = true;
							Pinned->ConsumedWorldDeltas[Index] = World->GetDeltaSeconds();
							Pinned->ConsumedDilations[Index] = World->GetWorldSettings()->GetEffectiveTimeDilation();
						}
					}
					Pinned->bUnexpectedRecordedDelta |= !bMatched;
				});
		}));

	// A real viewport button down (engine default CapturePermanently_IncludingInitialMouseDown)
	// acquires native mouse capture, then the two relative packets land in one native flush window.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			EnsureSelectedRouteWindowActive(*F.Session);
			const TSharedPtr<SViewport> Route = ResolveOwnedRouteRoot(*F.Session);
			Test.TestTrue(TEXT("Owned route root resolvable"), Route.IsValid());
			if (!Route.IsValid())
			{
				return;
			}
			const FGeometry RouteGeometry = Route->GetCachedGeometry();
			const FVector2D RouteSize = RouteGeometry.GetLocalSize();
			if (RouteSize.X <= 0.0 || RouteSize.Y <= 0.0)
			{
				Test.AddError(TEXT("Owned route root has no usable geometry"));
				return;
			}
			const FVector2D CenterScreen = RouteGeometry.LocalToAbsolute(RouteSize * 0.5);
			Probe->DownViewportPosition = ToViewportLocal(F, CenterScreen);

			FCortexEditorPhysicalInputEvent Down;
			Down.Kind = ECortexEditorPhysicalInputKind::PointerDown;
			Down.Key = EKeys::LeftMouseButton;
			Down.ViewportPosition = Probe->DownViewportPosition;
			Test.TestTrue(TEXT("Owned pointer down dispatched"), F.Session->Dispatch(Down).bSuccess);
			Test.TestTrue(TEXT("Owned viewport holds native mouse capture (sample accumulation path)"),
				Route->HasMouseCapture());

			Probe->bArmed = true;

			FCortexEditorPhysicalInputEvent MoveX;
			MoveX.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
			MoveX.Key = EKeys::Mouse2D;
			MoveX.Delta = FVector2D(4.0, 0.0);
			Test.TestTrue(TEXT("Relative packet (4,0) dispatched"), F.Session->Dispatch(MoveX).bSuccess);

			FCortexEditorPhysicalInputEvent MoveY;
			MoveY.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
			MoveY.Key = EKeys::Mouse2D;
			MoveY.Delta = FVector2D(0.0, 3.0);
			Test.TestTrue(TEXT("Relative packet (0,3) dispatched"), F.Session->Dispatch(MoveY).bSuccess);
		}));

	// Assertions on the exact bound native consumer.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 5,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			FCortexEditorPhysicalInputTestFixture& F = *Fixture;
			APlayerController* const Controller = F.Session->GetTargetBinding().Controller.Get();
			UPlayerInput* const PlayerInput = Controller ? Controller->PlayerInput : nullptr;
			Test.TestNotNull(TEXT("Bound controller still exists"), Controller);
			if (Controller == nullptr || PlayerInput == nullptr)
			{
				Probe->RemoveHook();
				return;
			}

			// --- native pre-flush per-axis sample counts (zero-component packet counted) ---
			Test.TestTrue(TEXT("Native pre-flush sample boundary observed"), Probe->bSawSamples);
			Test.TestEqual(TEXT("Native MouseX sample count in one flush"), Probe->SamplesX, 2);
			Test.TestEqual(TEXT("Native MouseY sample count in one flush (zero-axis packet counted)"),
				Probe->SamplesY, 2);
			Test.TestEqual(TEXT("Raw accumulated MouseX in one flush"), Probe->RawAccX, 4.0, 0.0);
			Test.TestEqual(TEXT("Raw accumulated MouseY in one flush"), Probe->RawAccY, -3.0, 0.0);

			Test.TestFalse(TEXT("Native consumption stayed on the exact bound controller/input"), Probe->bControllerChanged);
			Test.TestFalse(TEXT("Native world consumed no unrecorded application delta"), Probe->bUnexpectedRecordedDelta);
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(CortexNativePacketSampleDeltas); ++Index)
			{
				Test.TestTrue(FString::Printf(TEXT("Recorded cadence %d was actually consumed"), Index),
					Probe->bSeenRecordedDelta[Index]);
				Test.TestEqual(FString::Printf(TEXT("Recorded cadence %d preserved effective dilation"), Index),
					Probe->ConsumedDilations[Index], Probe->BaselineFilter.EffectiveTimeDilation, 0.0f);
				Test.TestEqual(FString::Printf(TEXT("Recorded cadence %d world delta is native recorded delta times dilation"), Index),
					Probe->ConsumedWorldDeltas[Index],
					static_cast<float>(CortexNativePacketSampleDeltas[Index]) * Probe->ConsumedDilations[Index], 0.0f);
			}
			bool bKnownDelta = false;
			for (const double Candidate : CortexNativePacketSampleDeltas)
			{
				bKnownDelta |= Probe->SampleAppDelta == Candidate;
			}
			Test.TestTrue(TEXT("Bundle frame application delta exactly matches a recorded delta"), bKnownDelta);
			Test.TestEqual(TEXT("Selected-world tick-start argument is the native application delta"),
				Probe->SampleWorldDelta, static_cast<float>(Probe->SampleAppDelta), 0.0f);

			// --- post-flush consumer-visible raw axis values on the bound UPlayerInput ---
			Test.TestTrue(TEXT("Consumer flush boundary observed"), Probe->bFlushObserved);
			Test.TestEqual(TEXT("Post-flush native raw MouseX"), Probe->FlushedRawX, 4.0, 0.0);
			Test.TestEqual(TEXT("Post-flush native raw MouseY"), Probe->FlushedRawY, -3.0, 0.0);
			Test.TestTrue(TEXT("Consumed MouseX axis value is positive"), Probe->ConsumedX > 0.0);
			Test.TestTrue(TEXT("Consumed MouseY axis value is negative"), Probe->ConsumedY < 0.0);

			// --- native smoothing memory consumed the samples (first-filter-call behavior) ---
			const UInputSettings* const Settings = GetDefault<UInputSettings>();
			if (Settings->bEnableMouseSmoothing)
			{
				Test.TestFalse(TEXT("Native mouse smoothing consumed the samples (SmoothedMouse X changed)"),
					FMath::IsNearlyEqual(Probe->SmoothedXAtFlush,
						static_cast<double>(Probe->BaselineFilter.SmoothedMouse[0]), 1e-9));
			}
			else
			{
				Test.TestTrue(TEXT("Mouse smoothing disabled: smoothing memory is untouched"),
					FMath::IsNearlyEqual(Probe->SmoothedXAtFlush,
						static_cast<double>(Probe->BaselineFilter.SmoothedMouse[0]), 1e-9));
			}

			// --- actual post-world control rotation, not merely registered events ---
			const double YawScale = Settings->bEnableLegacyInputScales
				? static_cast<double>(Controller->InputYawScale_DEPRECATED) : 1.0;
			const double PitchScale = Settings->bEnableLegacyInputScales
				? static_cast<double>(Controller->InputPitchScale_DEPRECATED) : 1.0;
			const double YawDelta = FMath::FindDeltaAngleDegrees(
				Probe->RotationBeforeInput.Yaw, Probe->RotationAfterInput.Yaw);
			const double PitchDelta = FMath::FindDeltaAngleDegrees(
				Probe->RotationBeforeInput.Pitch, Probe->RotationAfterInput.Pitch);
			Test.TestEqual(TEXT("Yaw delta equals the consumed axis value times the live yaw scale"),
				YawDelta, Probe->ConsumedX * 1.0 * YawScale, 0.05);
			Test.TestEqual(TEXT("Pitch delta equals the consumed axis value times the live pitch scale"),
				PitchDelta, Probe->ConsumedY * -1.0 * PitchScale, 0.05);

			// --- live PlayerInput class/config identity across the bundle ---
			Test.TestTrue(TEXT("The exact bound PlayerInput instance handled the bundle"),
				PlayerInput == Probe->BoundInput.Get());
			FString LiveClass;
			FString LiveSha;
			Test.TestTrue(TEXT("Live input config re-read after the bundle"),
				F.Session->ReadNativeInputConfig(LiveClass, LiveSha).bSuccess);
			Test.TestEqual(TEXT("Live PlayerInput class unchanged"), LiveClass, Probe->BaselineClass);
			Test.TestEqual(TEXT("Live PlayerInput class equals the bound instance class"),
				LiveClass, PlayerInput->GetClass()->GetPathName());
			Test.TestEqual(TEXT("Live input config hash unchanged"), LiveSha, Probe->BaselineSha);

			// --- exact native mouse-filter restore/readback on the still-original instance ---
			Test.TestTrue(TEXT("Baseline native mouse-filter state was read"), Probe->bBaselineRead);
			Test.TestTrue(TEXT("Native mouse-filter restore accepted the exact baseline"),
				F.Session->RestoreNativeMouseFilterState(Probe->BaselineFilter).bSuccess);
			FCortexEditorNativeMouseFilterState Readback;
			Test.TestTrue(TEXT("Restored native mouse-filter state reads back"),
				F.Session->ReadNativeMouseFilterState(Readback).bSuccess);
			Test.TestEqual(TEXT("Restored sample count"), Readback.SampleCount,
				Probe->BaselineFilter.SampleCount);
			Test.TestEqual(TEXT("Restored sampling total"), Readback.SamplingTotalSeconds,
				Probe->BaselineFilter.SamplingTotalSeconds, 0.0f);
			Test.TestEqual(TEXT("Restored zero time X"), Readback.ZeroTimeSeconds[0],
				Probe->BaselineFilter.ZeroTimeSeconds[0], 0.0f);
			Test.TestEqual(TEXT("Restored zero time Y"), Readback.ZeroTimeSeconds[1],
				Probe->BaselineFilter.ZeroTimeSeconds[1], 0.0f);
			Test.TestEqual(TEXT("Restored smoothed mouse X"), Readback.SmoothedMouse[0],
				Probe->BaselineFilter.SmoothedMouse[0], 0.0f);
			Test.TestEqual(TEXT("Restored smoothed mouse Y"), Readback.SmoothedMouse[1],
				Probe->BaselineFilter.SmoothedMouse[1], 0.0f);
			Test.TestEqual(TEXT("Restored effective dilation"), Readback.EffectiveTimeDilation,
				Probe->BaselineFilter.EffectiveTimeDilation, 0.0f);

			// Stop observing and release the owned synthetic button before teardown.
			Probe->RemoveHook();
			FCortexEditorPhysicalInputEvent Up;
			Up.Kind = ECortexEditorPhysicalInputKind::PointerUp;
			Up.Key = EKeys::LeftMouseButton;
			Up.ViewportPosition = Probe->DownViewportPosition;
			Test.TestTrue(TEXT("Owned pointer up dispatched"), F.Session->Dispatch(Up).bSuccess);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Recorded clock handoff admitted after owned PIE ended"),
				Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Real default-clock handoff completed"), Probe->Clock.IsHandoffComplete());
			Test.TestTrue(TEXT("Default delta is bounded and positive"),
				FApp::GetDeltaTime() > 0.0
					&& FApp::GetDeltaTime() <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds);
			Test.TestTrue(TEXT("Default current and last do not move backward"),
				FApp::GetCurrentTime() >= FApp::GetLastTime());
		}));
	return true;
}
// ---------------------------------------------------------------------------
// Case 1: a producer failure must quiesce the exact owned session with no further
// selected-world admission, even though the caller never calls EndOwnedPIE.
// Current lease: RunTimedFrame returns false on a failed read (engine skip path keeps
// advancing application GameTime by the previous delta), so the still-live owned world
// is ticked once more in the SAME engine update -> RED on the direct tick counters.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockProducerFailureOwnedQuiescenceTest,
	"Cortex.Editor.PhysicalInput.NativeClockProducerFailureOwnedQuiescence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockProducerFailureOwnedQuiescenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();

	struct FProducerFailureProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;
		// Exact selected world weak identity, captured before the failure invalidates the route.
		TWeakObjectPtr<UWorld> SelectedWorld;
		bool bSelectedWorldCaptured = false;
		// Producer bookkeeping: progress must be frozen once the read fails.
		int32 ProducerReadCount = 0;
		int32 AppliedStepCount = 0;
		int32 ReadCountAtFailure = 0;
		int32 AppliedCountAtFailure = 0;
		bool bRequestFailure = false;
		bool bFailureObserved = false;
		FString FailureCode;
		FString FailureMessage;
		// Direct engine-observation counters for the exact selected world.
		int32 TicksAfterFailure = 0;
		int32 TickEndsAfterFailure = 0;
		float LastExtraDeltaSeconds = 0.0f;
		double LastExtraCurrentSeconds = 0.0;
		~FProducerFailureProbe()
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		}
	};
	const auto Probe = MakeShared<FProducerFailureProbe>();
	const TWeakPtr<FProducerFailureProbe> WeakProbe = Probe;

	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 1,
		[WeakProbe](FCortexEditorAppClockStep& Step)
		{
			const auto Pinned = WeakProbe.Pin();
			FCortexCommandResult Result;
			if (!Pinned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Producer-failure probe owner expired");
				return Result;
			}
			++Pinned->ProducerReadCount;
			if (Pinned->bRequestFailure)
			{
				// A real failed producer read. No Session.EndOwnedPIE here: the amendment must
				// stop the terminal owned consumer from the retained exact-session authority.
				Pinned->bFailureObserved = true;
				Pinned->ReadCountAtFailure = Pinned->ProducerReadCount;
				Pinned->AppliedCountAtFailure = Pinned->AppliedStepCount;
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Controlled recorded-clock producer failure (no owned-session end)");
				Pinned->FailureCode = Result.ErrorCode;
				Pinned->FailureMessage = Result.ErrorMessage;
				return Result;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			++Pinned->AppliedStepCount;
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Owned clock admitted (%s: %s)"),
		*Acquired.ErrorCode, *Acquired.ErrorMessage), Acquired.bSuccess))
	{
		return false;
	}

	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockProducerFailureOwnedQuiescence"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Probe->Clock.SetReplaying();
			Probe->SelectedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->bSelectedWorldCaptured = Probe->SelectedWorld.IsValid();
			const TWeakPtr<FProducerFailureProbe> Weak = Probe;
			Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[Weak](UWorld* World, ELevelTick, float DeltaSeconds)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bFailureObserved && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TicksAfterFailure;
						Pinned->LastExtraDeltaSeconds = DeltaSeconds;
						Pinned->LastExtraCurrentSeconds = FApp::GetCurrentTime();
					}
				});
			Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bFailureObserved && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TickEndsAfterFailure;
					}
				});
			// Trigger the real failed read on the next owned engine frame.
			Probe->bRequestFailure = true;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Real producer failure occurred before engine world processing"),
				Probe->bFailureObserved);
			Test.TestTrue(TEXT("Exact selected world captured before route invalidation"),
				Probe->bSelectedWorldCaptured);
			Test.TestTrue(TEXT("Producer failure preserved its typed error"),
				Probe->Clock.GetLastFailure().ErrorCode == Probe->FailureCode
				&& Probe->Clock.GetLastFailure().ErrorMessage == Probe->FailureMessage);
			Test.TestFalse(TEXT("Clock still carries a failure, not a success"), Probe->Clock.GetLastFailure().bSuccess);
			Test.TestEqual(TEXT("Producer progress is frozen after the failed read"),
				Probe->AppliedStepCount, Probe->AppliedCountAtFailure);
			Test.TestEqual(TEXT("The failed read is not re-invoked after the failure"),
				Probe->ProducerReadCount, Probe->ReadCountAtFailure);
			Test.TestEqual(FString::Printf(
				TEXT("No unrecorded selected-world tick after the producer failure (extra delta %.9f, app current %.9f)"),
				Probe->LastExtraDeltaSeconds, Probe->LastExtraCurrentSeconds),
				Probe->TicksAfterFailure, 0);
			Test.TestEqual(TEXT("No unrecorded selected-world tick end after the producer failure"),
				Probe->TickEndsAfterFailure, 0);
		}));
	// Cleanup happens only after the assertions: end the exact owned session, then the normal
	// post-owned-PIE handoff must still complete against a real default clock frame.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Fixture->Session->EndOwnedPIE();
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Held-provider failure teardown admits the normal post-owned-PIE handoff"),
				Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Failure cleanup observes the next real default clock frame"),
				Probe->Clock.IsHandoffComplete());
			Test.TestTrue(TEXT("Failure cleanup default delta remains positive and bounded"),
				FMath::IsFinite(FApp::GetDeltaTime()) && FApp::GetDeltaTime() > 0.0
				&& FApp::GetDeltaTime() <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds);
		}));
	return true;
}

// ---------------------------------------------------------------------------
// Case 2a: sustained forty recorded updates, then an external provider clear performed at the
// sampling boundary (FCoreDelegates::OnSamplingInput), i.e. OUTSIDE the provider's own update
// callback. The cleanup frame must reanchor application time; the following real default frame
// must be finite, positive and bounded.
//
// Independent timing oracle: a frame-closure (OnEndFrame) observer registered before the lease can
// ever register its cleanup observer. It classifies:
//   * fixed cleanup      -> exact native signature (delta == double(float(1/60)) and
//                           last == CleanupBaseCurrent and current == CleanupBaseCurrent + delta),
//                           the signature is unique because CleanupBaseCurrent is the recorded
//                           current captured immediately before the actual clear;
//   * first real default -> the first provider-null frame that is NOT that signature.
// The classification does NOT gate on FApp::UseFixedTimeStep so it is correct whether or not the
// owned fixed-step settings have already been restored by the time the frame closes.
//
// NOTE (reported): the exact owned consumer is ended here (EndOwnedPIE) only to isolate the
// already-observed clear boundary, because terminating the terminal input/consumer authority is
// the amendment's responsibility. Case 1 is the separate zero-world-tick, unassisted case.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockExternalClearAtSamplingTest,
	"Cortex.Editor.PhysicalInput.NativeClockExternalClearAtSampling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockExternalClearAtSamplingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	const double CleanupDelta = static_cast<double>(static_cast<float>(CortexEditorClockLease::CleanupDeltaSeconds));

	struct FSamplingClearProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle SamplingClearHandle;
		FDelegateHandle EndFrameClassifyHandle;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;
		TWeakObjectPtr<UWorld> SelectedWorld;
		bool bSelectedWorldCaptured = false;
		int32 StepCount = 0;
		bool bArmClear = false;
		bool bCleared = false;
		double CleanupBaseCurrent = 0.0;
		// The clear frame's application time was already applied before the clear
		// (LaunchEngineLoop 5701 precedes OnSamplingInput 5833), so its closure still carries the
		// last recorded state; classification must start on a strictly later global frame.
		uint64 ClearFrameCounter = 0;
		// Independent fixed-vs-default oracle.
		bool bFixedCleanupObserved = false;
		double FixedCleanupCurrent = 0.0;
		bool bFirstDefaultObserved = false;
		double FirstDefaultDelta = 0.0;
		double FirstDefaultCurrent = 0.0;
		double FirstDefaultLast = 0.0;
		// Direct selected-world tick admission after the clear.
		int32 TicksAfterClear = 0;
		int32 TickEndsAfterClear = 0;
		bool SavedFixedRate = false;
		bool SavedFixedStep = false;
		double SavedFixedDelta = 0.0;
		~FSamplingClearProbe()
		{
			FCoreDelegates::OnSamplingInput.Remove(SamplingClearHandle);
			FCoreDelegates::OnEndFrame.Remove(EndFrameClassifyHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		}
	};
	const auto Probe = MakeShared<FSamplingClearProbe>();
	const TWeakPtr<FSamplingClearProbe> WeakProbe = Probe;
	Probe->SavedFixedRate = GEngine->bUseFixedFrameRate;
	Probe->SavedFixedStep = FApp::UseFixedTimeStep();
	Probe->SavedFixedDelta = FApp::GetFixedDeltaTime();

	Probe->SamplingClearHandle = FCoreDelegates::OnSamplingInput.AddLambda([WeakProbe, WeakFixture]()
	{
		const auto P = WeakProbe.Pin();
		if (!P.IsValid() || P->bCleared || !P->bArmClear)
		{
			return;
		}
		const auto Owned = WeakFixture.Pin();
		if (!Owned.IsValid())
		{
			return;
		}
		P->CleanupBaseCurrent = FApp::GetCurrentTime();
		P->ClearFrameCounter = GFrameCounter;
		P->bCleared = true;
		// Isolating clear boundary (reported): end the exact owned consumer, then perform the
		// actual external provider clear. The engine still has its own normal deferred PIE
		// destruction to perform.
		Owned->Session->EndOwnedPIE();
		GEngine->SetCustomTimeStep(nullptr);
	});

	Probe->EndFrameClassifyHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakProbe, CleanupDelta]()
	{
		const auto P = WeakProbe.Pin();
		if (!P.IsValid() || !P->bCleared || GEngine->GetCustomTimeStep() != nullptr)
		{
			return;
		}
		if (GFrameCounter == P->ClearFrameCounter)
		{
			// The clear frame's own closure still carries the pre-clear recorded application
			// time; a later frame must actually run before any fixed/default classification.
			return;
		}
		const double Delta = FApp::GetDeltaTime();
		const double Current = FApp::GetCurrentTime();
		const double Last = FApp::GetLastTime();
		if (Delta == CleanupDelta && Last == P->CleanupBaseCurrent && Current == P->CleanupBaseCurrent + CleanupDelta)
		{
			P->bFixedCleanupObserved = true;
			P->FixedCleanupCurrent = Current;
			return;
		}
		if (!P->bFirstDefaultObserved)
		{
			P->bFirstDefaultObserved = true;
			P->FirstDefaultDelta = Delta;
			P->FirstDefaultCurrent = Current;
			P->FirstDefaultLast = Last;
		}
	});

	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 3,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			const auto Owned = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid() || !Owned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Sampling-clear probe owner expired");
				return Result;
			}
			if (++P->StepCount == 40)
			{
				// Forty real recorded updates: arm the external clear for the sampling boundary of
				// the next engine frame (outside this provider update callback).
				P->bArmClear = true;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(TEXT("Sustained clock acquired"), Acquired.bSuccess))
	{
		return false;
	}

	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockExternalClearAtSampling"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Probe->Clock.SetReplaying();
			Probe->SelectedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->bSelectedWorldCaptured = Probe->SelectedWorld.IsValid();
			const TWeakPtr<FSamplingClearProbe> Weak = Probe;
			Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bCleared && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TicksAfterClear;
					}
				});
			Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bCleared && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TickEndsAfterClear;
					}
				});
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 48,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("External provider clear happened at the sampling boundary"), Probe->bCleared);
			Test.TestTrue(TEXT("Exact selected world captured before binding invalidation"), Probe->bSelectedWorldCaptured);
			Test.TestTrue(TEXT("Exact native fixed 1/60 cleanup signature was observed"),
				Probe->bFixedCleanupObserved);
			Test.TestTrue(TEXT("First real default frame was observed"), Probe->bFirstDefaultObserved);
			Test.TestTrue(FString::Printf(
				TEXT("First default delta after the sampling clear is finite, positive and bounded: %.9f"),
				Probe->FirstDefaultDelta),
				FMath::IsFinite(Probe->FirstDefaultDelta) && Probe->FirstDefaultDelta > 0.0
				&& Probe->FirstDefaultDelta <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds);
			Test.TestTrue(TEXT("Default current/last do not move backward from the cleanup frame"),
				Probe->FirstDefaultLast >= Probe->FixedCleanupCurrent
				&& Probe->FirstDefaultCurrent >= Probe->FirstDefaultLast);
			Test.TestEqual(TEXT("External clear remains truthfully Lost"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
			Test.TestFalse(TEXT("A lost external clear is not a successful Released handoff"),
				Probe->Clock.IsHandoffComplete());
			Test.TestNull(TEXT("No provider resurrected after the sampling clear"), GEngine->GetCustomTimeStep());
			Test.TestEqual(TEXT("Original engine fixed-rate setting unchanged"),
				!!GEngine->bUseFixedFrameRate, Probe->SavedFixedRate);
			Test.TestEqual(TEXT("Original fixed-step setting unchanged"),
				FApp::UseFixedTimeStep(), Probe->SavedFixedStep);
			Test.TestEqual(TEXT("Original fixed delta unchanged"),
				FApp::GetFixedDeltaTime(), Probe->SavedFixedDelta);
			Test.TestEqual(TEXT("No selected-world tick admitted after the sampling clear (with explicit owned end)"),
				Probe->TicksAfterClear, 0);
			Test.TestEqual(TEXT("No selected-world tick end after the sampling clear (with explicit owned end)"),
				Probe->TickEndsAfterClear, 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Repeated lost cleanup is admitted without resurrecting ownership"),
				Probe->Clock.BeginHandoff().bSuccess);
			Test.TestEqual(TEXT("Cleanup never fabricates Released after loss"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
		}));
	return true;
}

// ---------------------------------------------------------------------------
// Case 2b: identical sustained forty-update load, but the external clear is performed at the frame
// CLOSURE boundary (FCoreDelegates::OnEndFrame) instead of in the provider callback. The clear frame
// itself still carries the last recorded state, so the oracle skips exactly that one frame before
// classifying; everything else matches case 2a.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockExternalClearAtClosureTest,
	"Cortex.Editor.PhysicalInput.NativeClockExternalClearAtClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockExternalClearAtClosureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;
	const double CleanupDelta = static_cast<double>(static_cast<float>(CortexEditorClockLease::CleanupDeltaSeconds));

	struct FClosureClearProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle ClosureClearHandle;
		FDelegateHandle EndFrameClassifyHandle;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;
		TWeakObjectPtr<UWorld> SelectedWorld;
		bool bSelectedWorldCaptured = false;
		int32 StepCount = 0;
		bool bArmClear = false;
		bool bCleared = false;
		// The clear frame still carries the last recorded state; classify from the next frame on.
		uint64 ClearFrameCounter = 0;
		double CleanupBaseCurrent = 0.0;
		bool bFixedCleanupObserved = false;
		double FixedCleanupCurrent = 0.0;
		bool bFirstDefaultObserved = false;
		double FirstDefaultDelta = 0.0;
		double FirstDefaultCurrent = 0.0;
		double FirstDefaultLast = 0.0;
		int32 TicksAfterClear = 0;
		int32 TickEndsAfterClear = 0;
		bool SavedFixedRate = false;
		bool SavedFixedStep = false;
		double SavedFixedDelta = 0.0;
		~FClosureClearProbe()
		{
			FCoreDelegates::OnEndFrame.Remove(ClosureClearHandle);
			FCoreDelegates::OnEndFrame.Remove(EndFrameClassifyHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		}
	};
	const auto Probe = MakeShared<FClosureClearProbe>();
	const TWeakPtr<FClosureClearProbe> WeakProbe = Probe;
	Probe->SavedFixedRate = GEngine->bUseFixedFrameRate;
	Probe->SavedFixedStep = FApp::UseFixedTimeStep();
	Probe->SavedFixedDelta = FApp::GetFixedDeltaTime();

	// Registered first so the clear is injected exactly once at a frame closure.
	Probe->ClosureClearHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakProbe, WeakFixture]()
	{
		const auto P = WeakProbe.Pin();
		if (!P.IsValid() || P->bCleared || !P->bArmClear)
		{
			return;
		}
		const auto Owned = WeakFixture.Pin();
		if (!Owned.IsValid())
		{
			return;
		}
		P->CleanupBaseCurrent = FApp::GetCurrentTime();
		P->ClearFrameCounter = GFrameCounter;
		P->bCleared = true;
		// Isolating clear boundary (reported): the amendment owns terminal input/consumer authority.
		Owned->Session->EndOwnedPIE();
		GEngine->SetCustomTimeStep(nullptr);
	});

	Probe->EndFrameClassifyHandle = FCoreDelegates::OnEndFrame.AddLambda([WeakProbe, CleanupDelta]()
	{
		const auto P = WeakProbe.Pin();
		if (!P.IsValid() || !P->bCleared || GEngine->GetCustomTimeStep() != nullptr)
		{
			return;
		}
		if (GFrameCounter == P->ClearFrameCounter)
		{
			// The clear frame's own closure still carries the pre-clear recorded application time.
			return;
		}
		const double Delta = FApp::GetDeltaTime();
		const double Current = FApp::GetCurrentTime();
		const double Last = FApp::GetLastTime();
		if (Delta == CleanupDelta && Last == P->CleanupBaseCurrent && Current == P->CleanupBaseCurrent + CleanupDelta)
		{
			P->bFixedCleanupObserved = true;
			P->FixedCleanupCurrent = Current;
			return;
		}
		if (!P->bFirstDefaultObserved)
		{
			P->bFirstDefaultObserved = true;
			P->FirstDefaultDelta = Delta;
			P->FirstDefaultCurrent = Current;
			P->FirstDefaultLast = Last;
		}
	});

	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 4,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			const auto Owned = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid() || !Owned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Closure-clear probe owner expired");
				return Result;
			}
			if (++P->StepCount == 40)
			{
				// Arm the external clear for the closure of this same engine frame.
				P->bArmClear = true;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(TEXT("Sustained clock acquired"), Acquired.bSuccess))
	{
		return false;
	}

	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockExternalClearAtClosure"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Probe->Clock.SetReplaying();
			Probe->SelectedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->bSelectedWorldCaptured = Probe->SelectedWorld.IsValid();
			const TWeakPtr<FClosureClearProbe> Weak = Probe;
			Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bCleared && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TicksAfterClear;
					}
				});
			Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto Pinned = Weak.Pin();
					if (Pinned.IsValid() && Pinned->bCleared && World == Pinned->SelectedWorld.Get())
					{
						++Pinned->TickEndsAfterClear;
					}
				});
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 48,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("External provider clear happened at the frame-closure boundary"), Probe->bCleared);
			Test.TestTrue(TEXT("Exact selected world captured before binding invalidation"), Probe->bSelectedWorldCaptured);
			Test.TestTrue(TEXT("Exact native fixed 1/60 cleanup signature was observed"),
				Probe->bFixedCleanupObserved);
			Test.TestTrue(TEXT("First real default frame was observed"), Probe->bFirstDefaultObserved);
			Test.TestTrue(FString::Printf(
				TEXT("First default delta after the closure clear is finite, positive and bounded: %.9f"),
				Probe->FirstDefaultDelta),
				FMath::IsFinite(Probe->FirstDefaultDelta) && Probe->FirstDefaultDelta > 0.0
				&& Probe->FirstDefaultDelta <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds);
			Test.TestTrue(TEXT("Default current/last do not move backward from the cleanup frame"),
				Probe->FirstDefaultLast >= Probe->FixedCleanupCurrent
				&& Probe->FirstDefaultCurrent >= Probe->FirstDefaultLast);
			Test.TestEqual(TEXT("External clear remains truthfully Lost"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
			Test.TestFalse(TEXT("A lost external clear is not a successful Released handoff"),
				Probe->Clock.IsHandoffComplete());
			Test.TestNull(TEXT("No provider resurrected after the closure clear"), GEngine->GetCustomTimeStep());
			Test.TestEqual(TEXT("Original engine fixed-rate setting unchanged"),
				!!GEngine->bUseFixedFrameRate, Probe->SavedFixedRate);
			Test.TestEqual(TEXT("Original fixed-step setting unchanged"),
				FApp::UseFixedTimeStep(), Probe->SavedFixedStep);
			Test.TestEqual(TEXT("Original fixed delta unchanged"),
				FApp::GetFixedDeltaTime(), Probe->SavedFixedDelta);
			Test.TestEqual(TEXT("No selected-world tick admitted after the closure clear (with explicit owned end)"),
				Probe->TicksAfterClear, 0);
			Test.TestEqual(TEXT("No selected-world tick end after the closure clear (with explicit owned end)"),
				Probe->TickEndsAfterClear, 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Repeated lost cleanup is admitted without resurrecting ownership"),
				Probe->Clock.BeginHandoff().bSuccess);
			Test.TestEqual(TEXT("Cleanup never fabricates Released after loss"),
				Probe->Clock.GetState(), ECortexEditorClockState::Lost);
		}));
	return true;
}

// ---------------------------------------------------------------------------
// Case 4: destroy the last lease wrapper inside its executing producer callback, then collect
// garbage before the callback returns. A pinned C++ lease state alone does not prove that the
// transient UObject provider or exact owned session survives reentrancy. Verify provider liveness
// through GC, zero additional exact-world ticks, no further producer calls, and eventual cleanup.
// IsCleanupComplete assertions require the approved API amendment.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockCleanupLifetimeTest,
	"Cortex.Editor.PhysicalInput.NativeClockCleanupLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockCleanupLifetimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();

	struct FRetainedClockOwner
	{
		FCortexEditorEngineClockLease Clock;
	};
	struct FLifetimeProbe
	{
		bool bArmDestroy = false;
		bool bWrapperDestroyed = false;
		bool bProviderPresentAtDestruction = false;
		int32 ProducerCalls = 0;
		int32 CallsAfterDestruction = 0;
		TWeakObjectPtr<UWorld> SelectedWorld;
		FDelegateHandle WorldStartHandle;
		FDelegateHandle WorldEndHandle;
		int32 TicksAfterDestruction = 0;
		int32 TickEndsAfterDestruction = 0;
		TWeakObjectPtr<UEngineCustomTimeStep> ExecutingProvider;
		bool bExecutingProviderRetainedThroughGC = false;
		bool SavedFixedRate = false;
		bool SavedFixedStep = false;
		double SavedFixedDelta = 0.0;
		~FLifetimeProbe()
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldEndHandle);
		}
	};

	const auto Probe = MakeShared<FLifetimeProbe>();
	const TWeakPtr<FLifetimeProbe> WeakProbe = Probe;
	// The callback pins the heap slot independently before resetting its last wrapper reference:
	// the destructor may clear the stored callback and its captured copy during that reset.
	const TSharedPtr<TSharedPtr<FRetainedClockOwner>> OwnerSlot =
		MakeShared<TSharedPtr<FRetainedClockOwner>>();
	*OwnerSlot = MakeShared<FRetainedClockOwner>();

	Probe->SavedFixedRate = GEngine->bUseFixedFrameRate;
	Probe->SavedFixedStep = FApp::UseFixedTimeStep();
	Probe->SavedFixedDelta = FApp::GetFixedDeltaTime();

	const FCortexCommandResult Acquired = (*OwnerSlot)->Clock.Acquire(*GEngine, 5,
		[WeakProbe, OwnerSlot](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Lifetime probe owner expired");
				return Result;
			}
			++P->ProducerCalls;
			if (P->bWrapperDestroyed)
			{
				++P->CallsAfterDestruction;
			}
			if (P->bArmDestroy && !P->bWrapperDestroyed)
			{
				P->bWrapperDestroyed = true;
				P->bProviderPresentAtDestruction = GEngine->GetCustomTimeStep() != nullptr;
				// Destroy the wrapper from inside the producer callback. The transient clock's
				// UpdateTimeStep retains the lease state via a local TSharedPtr for this whole call.
				P->ExecutingProvider = GEngine->GetCustomTimeStep();
				const auto PinnedSlot = OwnerSlot;
				PinnedSlot->Reset();
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				P->bExecutingProviderRetainedThroughGC = P->ExecutingProvider.IsValid();
				Result.bSuccess = true;
				return Result;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Retained-owner clock admitted (%s: %s)"),
		*Acquired.ErrorCode, *Acquired.ErrorMessage), Acquired.bSuccess))
	{
		return false;
	}

	TestTrue(TEXT("Owned PIE admitted"), Fixture->Session->BeginOwnedPIE(
		Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockCleanupLifetime"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Probe->SelectedWorld = Fixture->Session->GetTargetBinding().World;
			const TWeakPtr<FLifetimeProbe> Weak = Probe;
			Probe->WorldStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto P = Weak.Pin();
					if (P.IsValid() && P->bWrapperDestroyed && World == P->SelectedWorld.Get())
					{
						++P->TicksAfterDestruction;
					}
				});
			Probe->WorldEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
				[Weak](UWorld* World, ELevelTick, float)
				{
					const auto P = Weak.Pin();
					if (P.IsValid() && P->bWrapperDestroyed && World == P->SelectedWorld.Get())
					{
						++P->TickEndsAfterDestruction;
					}
				});
			Probe->bArmDestroy = true;
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 3,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Lease wrapper was destroyed inside the producer callback"),
				Probe->bWrapperDestroyed);
			Test.TestTrue(TEXT("Owned provider was attached at the moment of reentrant destruction"),
				Probe->bProviderPresentAtDestruction);
			Test.TestNull(TEXT("Reentrant destruction released the owned engine provider"),
				GEngine->GetCustomTimeStep());
			Test.TestEqual(TEXT("Destroyed lease never invokes its read callback again"),
				Probe->CallsAfterDestruction, 0);
			Test.TestTrue(TEXT("Executing native provider survived reentrant wrapper destruction and GC"),
				Probe->bExecutingProviderRetainedThroughGC);
			Test.TestEqual(TEXT("Retired wrapper admits no additional exact owned-world tick"),
				Probe->TicksAfterDestruction, 0);
			Test.TestEqual(TEXT("Retired wrapper admits no additional exact owned-world tick end"),
				Probe->TickEndsAfterDestruction, 0);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 0,
		[Fixture](FAutomationTestBase& Test)
		{
			(void)Test;
			Fixture->Session->EndOwnedPIE();
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestNull(TEXT("Provider stays released after exact owned teardown"),
				GEngine->GetCustomTimeStep());
			Test.TestEqual(TEXT("Original engine fixed-rate setting unchanged"),
				!!GEngine->bUseFixedFrameRate, Probe->SavedFixedRate);
			Test.TestEqual(TEXT("Original fixed-step setting unchanged"),
				FApp::UseFixedTimeStep(), Probe->SavedFixedStep);
			Test.TestEqual(TEXT("Original fixed delta unchanged"),
				FApp::GetFixedDeltaTime(), Probe->SavedFixedDelta);
			// Explicit GC after the owned teardown/lifetime constraints: the released transient
			// clock must collect with no dangling owner and no access violation.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			Test.TestNull(TEXT("No provider after GC of the released transient clock"),
				GEngine->GetCustomTimeStep());
			Test.TestFalse(TEXT("Released transient provider collects after terminal teardown"),
				Probe->ExecutingProvider.IsValid());
		}));
	return true;
}
// ===========================================================================
// 1) Cortex.Editor.PhysicalInput.NativeClockForeignTeardownSafety  (mandatory gate)
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockForeignTeardownSafetyTest,
	"Cortex.Editor.PhysicalInput.NativeClockForeignTeardownSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockForeignTeardownSafetyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	struct FForeignTeardownProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle PieStartedHandle;
		FDelegateHandle WorldTickStartHandle;

		/** Armed only for the post-quiescence foreign instance so the owned birth is not captured. */
		bool bArmForeignCapture = false;
		/** Set in the same latent frame the owned engine-wide end is queued. */
		bool bOwnedEndRequested = false;

		TWeakObjectPtr<UWorld> OwnedWorld;
		FName OwnedContextHandle = NAME_None;

		// ---- actual native birth/tick observations of the late-join instance ----
		bool bForeignWorldlessObserved = false;
		int32 ForeignWorldlessCount = 0;
		FName ForeignContextHandle = NAME_None;
		TWeakObjectPtr<UWorld> ForeignWorld;
		bool bForeignTickedBeforeEnd = false;
		int32 ForeignTicksBeforeEnd = 0;

		// ---- owned terminal world must admit zero further ticks ----
		int32 OwnedTicksAfterEnd = 0;

		bool bLateJoinRequestedSeen = false;

		bool SavedFixedRate = false;
		bool SavedFixedStep = false;
		double SavedFixedDelta = 0.0;
		/** Exact attached provider identity captured before the queued end. */
		UEngineCustomTimeStep* SavedProvider = nullptr;

		void RemoveDelegates()
		{
			FWorldDelegates::OnPIEStarted.Remove(PieStartedHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
			PieStartedHandle.Reset();
			WorldTickStartHandle.Reset();
		}
		~FForeignTeardownProbe() { RemoveDelegates(); }
	};

	const auto Probe = MakeShared<FForeignTeardownProbe>();
	const TWeakPtr<FForeignTeardownProbe> WeakProbe = Probe;
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;

	// The native engine clock provider is acquired before the owned request so the probe can
	// prove the session's engine-wide end does not touch the provider.
	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 4,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			const auto Owned = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid() || !Owned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Native foreign-teardown probe owner expired");
				return Result;
			}
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 60.0;
			Step.DeltaSeconds = 1.0 / 60.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Native clock acquired (%s: %s)"),
		*Acquired.ErrorCode, *Acquired.ErrorMessage), Acquired.bSuccess))
	{
		return false;
	}

	// Independent native observers. Neither reads private engine state; both read only the
	// public world-context list and the world's own tick boundary.
	Probe->PieStartedHandle = FWorldDelegates::OnPIEStarted.AddLambda(
		[WeakProbe](UGameInstance*)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmForeignCapture || GEngine == nullptr)
			{
				return;
			}
			int32 WorldlessPIEContexts = 0;
			FName Handle = NAME_None;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == nullptr)
				{
					++WorldlessPIEContexts;
					Handle = Context.ContextHandle;
				}
			}
			P->ForeignWorldlessCount = WorldlessPIEContexts;
			if (WorldlessPIEContexts == 1)
			{
				P->bForeignWorldlessObserved = true;
				P->ForeignContextHandle = Handle;
			}
		});
	Probe->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmForeignCapture || World == nullptr)
			{
				return;
			}
			if (World == P->OwnedWorld.Get())
			{
				if (P->bOwnedEndRequested)
				{
					++P->OwnedTicksAfterEnd;
				}
				return;
			}
			if (World->WorldType == EWorldType::PIE)
			{
				if (!P->ForeignWorld.IsValid())
				{
					P->ForeignWorld = World;
				}
				if (World == P->ForeignWorld.Get())
				{
					P->bForeignTickedBeforeEnd = true;
					++P->ForeignTicksBeforeEnd;
				}
			}
		});

	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));

	// Injection: queue the OWNED end through the ownership-scoped authority, then request a late join
	// in the same latent frame. The scoped end retires only the owned instance, so the late-joined
	// foreign context/world is born and keeps ticking — the aggregate end flag must stay clear, since
	// raising it is exactly what would tear down the foreign instance.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeClockForeignTeardownSafety.Inject"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			Probe->OwnedWorld = Fixture->Session->GetTargetBinding().World;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == Probe->OwnedWorld.Get())
				{
					Probe->OwnedContextHandle = Context.ContextHandle;
				}
			}
			Probe->SavedFixedRate = !!GEngine->bUseFixedFrameRate;
			Probe->SavedFixedStep = FApp::UseFixedTimeStep();
			Probe->SavedFixedDelta = FApp::GetFixedDeltaTime();
			Probe->SavedProvider = GEngine->GetCustomTimeStep();

			// Arm only now: the owned birth already happened and must not be captured.
			Probe->bArmForeignCapture = true;

			Fixture->Session->EndOwnedPIE();
			Probe->bOwnedEndRequested = true;

			// The scoped owned end must NOT raise the engine-wide aggregate end flag: doing so is
			// exactly what would tear down the foreign late-joined instance.
			Test.TestFalse(TEXT("Owned end does not raise the engine-wide end flag"), GEditor->ShouldEndPlayMap());
			Test.TestTrue(TEXT("Owned terminal world no longer ticks"),
				Probe->OwnedWorld.IsValid() && !Probe->OwnedWorld->ShouldTick());

			const TOptional<FPlayInEditorSessionInfo> InfoBefore = GEditor->GetPlayInEditorSessionInfo();
			Test.TestTrue(TEXT("PIE session info is still present while the end is only queued"),
				InfoBefore.IsSet());

			GEditor->RequestLateJoin();
			const TOptional<FPlayInEditorSessionInfo> InfoAfter = GEditor->GetPlayInEditorSessionInfo();
			Probe->bLateJoinRequestedSeen = InfoAfter.IsSet() && InfoAfter->bLateJoinRequested;
			Test.TestTrue(TEXT("Engine recorded the late-join request"), Probe->bLateJoinRequestedSeen);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("A second real PIE context was born during the queued engine-wide end"),
				Probe->bForeignWorldlessObserved);
			Test.TestTrue(TEXT("The foreign world was observed ticking before the engine-wide end"),
				Probe->bForeignTickedBeforeEnd);
			Test.TestEqual(TEXT("Owned terminal world admitted no tick after the queued end"),
				Probe->OwnedTicksAfterEnd, 0);

			bool bForeignContextPresent = false;
			bool bForeignWorldAlive = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE
					&& Context.ContextHandle == Probe->ForeignContextHandle)
				{
					bForeignContextPresent = true;
					bForeignWorldAlive = Context.World() != nullptr
						&& Context.World() == Probe->ForeignWorld.Get();
				}
			}
			Test.TestTrue(TEXT("Foreign PIE context survives an owned queued engine-wide end"),
				bForeignContextPresent);
			Test.TestTrue(TEXT("Exact foreign PIE world survives and remains the live world"),
				bForeignWorldAlive);
			Test.TestTrue(TEXT("Foreign world still admits ticks after the owned end"),
				Probe->ForeignWorld.IsValid() && Probe->ForeignWorld->ShouldTick());

			Test.TestTrue(TEXT("Engine clock provider identity is untouched by the foreign coexistence"),
				Probe->SavedProvider != nullptr && GEngine->GetCustomTimeStep() == Probe->SavedProvider);
			Test.TestEqual(TEXT("Original engine fixed-rate setting unchanged"),
				!!GEngine->bUseFixedFrameRate, Probe->SavedFixedRate);
			Test.TestEqual(TEXT("Original fixed-step setting unchanged"),
				FApp::UseFixedTimeStep(), Probe->SavedFixedStep);
			Test.TestEqual(TEXT("Original fixed delta unchanged"),
				FApp::GetFixedDeltaTime(), Probe->SavedFixedDelta);

			FCortexCommandResult EndError;
			Test.TestFalse(TEXT("Ended session validates no foreign target"),
				Fixture->Session->ValidateTarget(EndError));
			Test.TestFalse(TEXT("Ended session binds no world"),
				Fixture->Session->GetTargetBinding().World.IsValid());
		}));

	// Fixture-owned cleanup only, after the survival assertions.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			// Retain the fixture while the producer can still read its weak dependency.
			(void)Fixture;
			Test.TestTrue(TEXT("Owned clock releases after the foreign fixture is cleaned up"),
				Probe->Clock.BeginHandoff().bSuccess);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Owned clock observed its completed handoff"),
				Probe->Clock.IsHandoffComplete());
		}));
	return true;
}

// ===========================================================================
// 2) Cortex.Editor.PhysicalInput.NativeClockRequestAuthority
//    Byte-identical replacement of the still-queued accepted request must never be
//    cancelled/ adopted by the old session.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockRequestAuthorityTest,
	"Cortex.Editor.PhysicalInput.NativeClockRequestAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockRequestAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	struct FRequestAuthorityProbe
	{
		ULevelEditorPlaySettings* ReplacementSettings = nullptr;
		FDelegateHandle WorldTickStartHandle;
		bool bForeignWorldTicked = false;
		TWeakObjectPtr<UWorld> ForeignWorld;
		void RemoveDelegates()
		{
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
			WorldTickStartHandle.Reset();
		}
		~FRequestAuthorityProbe() { RemoveDelegates(); }
	};
	const auto Probe = MakeShared<FRequestAuthorityProbe>();
	const TWeakPtr<FRequestAuthorityProbe> WeakProbe = Probe;
	// No PIE world exists while the accepted request is only queued, so any PIE world tick the
	// probe observes later is the actually-started foreign replacement.
	Probe->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || World == nullptr || World->WorldType != EWorldType::PIE) { return; }
			if (!P->ForeignWorld.IsValid()) { P->ForeignWorld = World; }
			if (World == P->ForeignWorld.Get()) { P->bForeignWorldTicked = true; }
		});

	TestTrue(TEXT("PIE preparation admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	const TOptional<FRequestPlaySessionParams> OwnRequest = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Owned request is still queued"), OwnRequest.IsSet());
	if (!OwnRequest.IsSet())
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
		return true;
	}
	const ULevelEditorPlaySettings* AcceptedSettings = OwnRequest->EditorPlaySettings.Get();
	TestNotNull(TEXT("Engine-queued request carries a duplicated settings object"), AcceptedSettings);

	// Snapshot the engine-queued copy and re-issue it byte-for-byte through the public API.
	// RequestPlaySession duplicates EditorPlaySettings again (PlayLevel 985-1000), so the new
	// request has a different settings UObject with fingerprint-equivalent values.
	FRequestPlaySessionParams Replacement = OwnRequest.GetValue();
	TestTrue(TEXT("Copied request keeps the accepted destination"),
		Replacement.SessionDestination == OwnRequest->SessionDestination);
	TestTrue(TEXT("Copied request keeps the accepted world type"),
		Replacement.WorldType == OwnRequest->WorldType);
	TestEqual(TEXT("Copied request keeps the accepted map"),
		Replacement.GlobalMapOverride, OwnRequest->GlobalMapOverride);
	TestEqual(TEXT("Copied request keeps the accepted destination-viewport state"),
		Replacement.DestinationSlateViewport.IsSet(), OwnRequest->DestinationSlateViewport.IsSet());
	TestEqual(TEXT("Copied request keeps the accepted start-location state"),
		Replacement.StartLocation.IsSet(), OwnRequest->StartLocation.IsSet());

	GEditor->RequestPlaySession(Replacement);

	const TOptional<FRequestPlaySessionParams> NewQueued = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Byte-identical replacement is queued"), NewQueued.IsSet());
	ULevelEditorPlaySettings* ReplacementSettings =
		NewQueued.IsSet() ? NewQueued->EditorPlaySettings.Get() : nullptr;
	TestNotNull(TEXT("Replacement carries its own duplicated settings object"), ReplacementSettings);
	TestTrue(TEXT("Replacement settings identity differs from the accepted request's object"),
		ReplacementSettings != nullptr && ReplacementSettings != AcceptedSettings);

	// Fingerprint-equivalent values: every field the lease fingerprint reads must match.
	TestEqual(TEXT("Replacement map value equals the accepted map"),
		NewQueued.IsSet() ? NewQueued->GlobalMapOverride : FString(), Fixture->RequestedMap);
	TestTrue(TEXT("Replacement destination equals the accepted destination"),
		NewQueued.IsSet() && NewQueued->SessionDestination == OwnRequest->SessionDestination);
	TestEqual(TEXT("Replacement destination-viewport presence equals the accepted one"),
		NewQueued.IsSet() && NewQueued->DestinationSlateViewport.IsSet(),
		OwnRequest->DestinationSlateViewport.IsSet());
	TestEqual(TEXT("Replacement start-location presence equals the accepted one"),
		NewQueued.IsSet() && NewQueued->StartLocation.IsSet(), OwnRequest->StartLocation.IsSet());
	if (ReplacementSettings != nullptr && AcceptedSettings != nullptr)
	{
		bool bAcceptedRunOne = false;
		bool bReplacementRunOne = false;
		AcceptedSettings->GetRunUnderOneProcess(bAcceptedRunOne);
		ReplacementSettings->GetRunUnderOneProcess(bReplacementRunOne);
		TestEqual(TEXT("Replacement RunUnderOneProcess value equals the accepted one"),
			bReplacementRunOne, bAcceptedRunOne);

		EPlayNetMode AcceptedNetMode = PIE_Standalone;
		EPlayNetMode ReplacementNetMode = PIE_Standalone;
		AcceptedSettings->GetPlayNetMode(AcceptedNetMode);
		ReplacementSettings->GetPlayNetMode(ReplacementNetMode);
		TestTrue(TEXT("Replacement play net mode equals the accepted one"),
			ReplacementNetMode == AcceptedNetMode);

		int32 AcceptedClients = 0;
		int32 ReplacementClients = 0;
		AcceptedSettings->GetPlayNumberOfClients(AcceptedClients);
		ReplacementSettings->GetPlayNumberOfClients(ReplacementClients);
		TestEqual(TEXT("Replacement local-client count equals the accepted one"),
			ReplacementClients, AcceptedClients);
		TestEqual(TEXT("Replacement separate-server flag equals the accepted one"),
			!!ReplacementSettings->bLaunchSeparateServer, !!AcceptedSettings->bLaunchSeparateServer);
	}

	// Preserve the foreign identity across an engine-referenced GC.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const TOptional<FRequestPlaySessionParams> AfterGC = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Replacement survived engine-referenced GC"), AfterGC.IsSet());
	TestTrue(TEXT("Replacement settings identity is preserved across GC"),
		AfterGC.IsSet() && AfterGC->EditorPlaySettings.Get() == ReplacementSettings);

	// The old session must never cancel or adopt an otherwise identical foreign replacement.
	// A weak-UObject identity guard is intentionally absent in the current API: this Red is
	// behavioural, not a missing-API failure.
	Probe->ReplacementSettings = ReplacementSettings;

	Fixture->Session->EndOwnedPIE();

	const TOptional<FRequestPlaySessionParams> Surviving = GEditor->GetPlaySessionRequest();
	TestTrue(TEXT("Old session did not cancel the byte-identical foreign replacement"),
		Surviving.IsSet());
	if (Surviving.IsSet())
	{
		TestEqual(TEXT("Surviving replacement keeps its own map identity"),
			Surviving->GlobalMapOverride, Fixture->RequestedMap);
	}
	FCortexCommandResult RelinquishError;
	TestFalse(TEXT("Old session does not adopt the replacement"),
		Fixture->Session->ValidateTarget(RelinquishError));
	TestFalse(TEXT("Old session binds no world"),
		Fixture->Session->GetTargetBinding().World.IsValid());

	// The replacement must actually start as an independent foreign PIE session that keeps the
	// engine-referenced settings identity, while the old session neither adopts nor binds it.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			Test.TestTrue(TEXT("Byte-identical foreign request started a real PIE session"),
				Info.IsSet());
			Test.TestTrue(TEXT("Started foreign session retains the replacement settings identity"),
				Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings.Get()
					== Probe->ReplacementSettings);
			Test.TestTrue(TEXT("Started foreign PIE world actually ticks"),
				Probe->bForeignWorldTicked);

			FCortexCommandResult AdoptError;
			Test.TestFalse(TEXT("Old session never adopts the started foreign session"),
				Fixture->Session->ValidateTarget(AdoptError));
			Test.TestFalse(TEXT("Old session binds no world"),
				Fixture->Session->GetTargetBinding().World.IsValid());
		}));

	// Fixture cleanup ends only the session this test started; the old session's own cleanup must
	// still resolve without touching the foreign session.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 4,
		[Fixture](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Old session's own cleanup resolves"),
				Fixture->Session->IsOwnedPIEEnded());
		}));
	return true;
}

// ===========================================================================
// 3) Cortex.Editor.PhysicalInput.NativeClockLossWorldBirth
//    The actual consumed owned PIE context is observed World()==nullptr at the public
//    OnPIEStarted boundary, the engine provider is then cleared, and the exact born world
//    must admit zero ticks with a bounded real default transition.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockLossWorldBirthTest,
	"Cortex.Editor.PhysicalInput.NativeClockLossWorldBirth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockLossWorldBirthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	struct FWorldBirthProbe
	{
		FCortexEditorEngineClockLease Clock;
		FDelegateHandle PieStartedHandle;
		FDelegateHandle PostPieStartedHandle;
		FDelegateHandle WorldTickStartHandle;
		FDelegateHandle WorldTickEndHandle;
		FDelegateHandle SamplingHandle;

		bool bActualWorldlessObserved = false;
		FName WorldlessContextHandle = NAME_None;
		double CleanupBaseCurrent = 0.0;
		bool bCleared = false;

		bool bPostPieStartedObserved = false;
		bool bProviderNullAtPostPieStarted = false;

		/** Exact born world resolved from the world-less context handle at the PostPIEStarted boundary. */
		TWeakObjectPtr<UWorld> BornWorld;
		bool bBornWorldCaptured = false;
		int32 PreparationUpdates = 0;
		int32 BornWorldTickStarts = 0;
		int32 BornWorldTickEnds = 0;

		/** Exact fixed cleanup frame signature, then the first genuine (non-fixed, null-provider) default. */
		bool bFixedCleanupObserved = false;
		double FixedCleanupCurrent = 0.0;
		double FixedCleanupDelta = 0.0;

		bool bFirstDefaultObserved = false;
		double FirstDefaultDelta = 0.0;
		double FirstDefaultCurrent = 0.0;
		double FirstDefaultLast = 0.0;

		void RemoveDelegates()
		{
			FWorldDelegates::OnPIEStarted.Remove(PieStartedHandle);
			FEditorDelegates::PostPIEStarted.Remove(PostPieStartedHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
			FWorldDelegates::OnWorldTickEnd.Remove(WorldTickEndHandle);
			FCoreDelegates::OnSamplingInput.Remove(SamplingHandle);
			PieStartedHandle.Reset();
			PostPieStartedHandle.Reset();
			WorldTickStartHandle.Reset();
			WorldTickEndHandle.Reset();
			SamplingHandle.Reset();
		}
		~FWorldBirthProbe() { RemoveDelegates(); }
	};

	const auto Probe = MakeShared<FWorldBirthProbe>();
	const TWeakPtr<FWorldBirthProbe> WeakProbe = Probe;
	const TWeakPtr<FCortexEditorPhysicalInputTestFixture> WeakFixture = Fixture;

	// Acquire BEFORE the owned request: the lease is the engine provider while the owned PIE
	// is prepared. The producer paces so preparation timing is real wall-clock time.
	const FCortexCommandResult Acquired = Probe->Clock.Acquire(*GEngine, 6,
		[WeakProbe, WeakFixture](FCortexEditorAppClockStep& Step)
		{
			const auto P = WeakProbe.Pin();
			const auto Owned = WeakFixture.Pin();
			FCortexCommandResult Result;
			if (!P.IsValid() || !Owned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Native world-birth probe owner expired");
				return Result;
			}
			++P->PreparationUpdates;
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});
	if (!TestTrue(FString::Printf(TEXT("Native clock acquired before the owned request (%s: %s)"),
		*Acquired.ErrorCode, *Acquired.ErrorMessage), Acquired.bSuccess))
	{
		return false;
	}

	// Independent source-grounded world-less injection: at the public OnPIEStarted boundary the
	// consumed owned context exists but has no world (GameInstance.cpp:281 broadcasts before the
	// context receives its world). Clear the actual engine provider there.
	Probe->PieStartedHandle = FWorldDelegates::OnPIEStarted.AddLambda(
		[WeakProbe](UGameInstance*)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || P->bCleared || GEngine == nullptr)
			{
				return;
			}
			int32 WorldlessPIEContexts = 0;
			FName Handle = NAME_None;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == nullptr)
				{
					++WorldlessPIEContexts;
					Handle = Context.ContextHandle;
				}
			}
			P->bActualWorldlessObserved = WorldlessPIEContexts == 1;
			if (P->bActualWorldlessObserved)
			{
				P->WorldlessContextHandle = Handle;
				P->CleanupBaseCurrent = FApp::GetCurrentTime();
				P->bCleared = true;
				GEngine->SetCustomTimeStep(nullptr);
			}
		});
	// Independent PostPIEStarted boundary observation (PlayLevel 2991). This is the subject's
	// pre-census stop boundary, never the injection point.
	Probe->PostPieStartedHandle = FEditorDelegates::PostPIEStarted.AddLambda(
		[WeakProbe](const bool)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid())
			{
				return;
			}
			P->bPostPieStartedObserved = true;
			P->bProviderNullAtPostPieStarted = GEngine != nullptr
				&& GEngine->GetCustomTimeStep() == nullptr;
			// Resolve the EXACT born world from the recorded world-less context handle now that the
			// instance has been created, BEFORE the engine's tick census. The world is never adopted
			// from "first PIE world seen at tick start" (a correct zero-tick outcome would leave it absent).
			if (P->WorldlessContextHandle != NAME_None && GEngine != nullptr)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE
						&& Context.ContextHandle == P->WorldlessContextHandle)
					{
						P->BornWorld = Context.World();
						P->bBornWorldCaptured = P->BornWorld.IsValid();
					}
				}
			}
		});
	Probe->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || World == nullptr || !P->BornWorld.IsValid())
			{
				return;
			}
			if (World == P->BornWorld.Get())
			{
				++P->BornWorldTickStarts;
			}
		});
	Probe->WorldTickEndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || World == nullptr || !P->BornWorld.IsValid())
			{
				return;
			}
			if (World == P->BornWorld.Get())
			{
				++P->BornWorldTickEnds;
			}
		});
	// Observe the exact native fixed cleanup signature and the first genuine default update.
	// A missing cleanup frame must not hide the actual first default frame from the oracle.
	Probe->SamplingHandle = FCoreDelegates::OnSamplingInput.AddLambda(
		[WeakProbe]()
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bCleared)
			{
				return;
			}
			if (!P->bFixedCleanupObserved)
			{
				const double CleanupDelta =
					static_cast<double>(static_cast<float>(CortexEditorClockLease::CleanupDeltaSeconds));
				if (GEngine != nullptr && GEngine->GetCustomTimeStep() == nullptr
					&& FApp::UseFixedTimeStep() && FApp::GetDeltaTime() == CleanupDelta
					&& FApp::GetLastTime() == P->CleanupBaseCurrent
					&& FApp::GetCurrentTime() == P->CleanupBaseCurrent + CleanupDelta)
				{
					P->bFixedCleanupObserved = true;
					P->FixedCleanupCurrent = FApp::GetCurrentTime();
					P->FixedCleanupDelta = FApp::GetDeltaTime();
					return;
				}
			}
			if (!P->bFirstDefaultObserved
				&& GEngine != nullptr && GEngine->GetCustomTimeStep() == nullptr
				&& !FApp::UseFixedTimeStep())
			{
				P->bFirstDefaultObserved = true;
				P->FirstDefaultDelta = FApp::GetDeltaTime();
				P->FirstDefaultCurrent = FApp::GetCurrentTime();
				P->FirstDefaultLast = FApp::GetLastTime();
			}
		});

	// Sustain real native clock preparation BEFORE the owned request so the accumulated cache debt is
	// actually exercised (an immediate Begin would only produce a single preparation step). The clock
	// producer runs once per engine frame during these bounded latent frames.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 40,
		[Fixture, Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("At least forty real preparation clock updates preceded the request"),
				Probe->PreparationUpdates >= 40);
			Test.TestTrue(TEXT("Owned PIE admitted after sustained clock preparation"),
				Fixture->Session->BeginOwnedPIE(
					Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Actual world-less owned PIE birth was observed"),
				Probe->bActualWorldlessObserved);
			Test.TestTrue(TEXT("The engine provider was actually cleared at world birth"),
				Probe->bCleared);
			Test.TestTrue(TEXT("PostPIEStarted boundary was observed independently"),
				Probe->bPostPieStartedObserved);
			Test.TestTrue(TEXT("Provider is null at the PostPIEStarted boundary"),
				Probe->bProviderNullAtPostPieStarted);
			Test.TestTrue(TEXT("The exact born owned world was captured from the world-less context"),
				Probe->bBornWorldCaptured);
			Test.TestEqual(TEXT("No tick reached the exact born owned world"),
				Probe->BornWorldTickStarts, 0);
			Test.TestEqual(TEXT("No tick end reached the exact born owned world"),
				Probe->BornWorldTickEnds, 0);
			Test.TestTrue(TEXT("Exact fixed cleanup frame signature was observed"),
				Probe->bFixedCleanupObserved);
			Test.TestTrue(TEXT("First genuine non-fixed null-provider default was observed"),
				Probe->bFirstDefaultObserved);
			Test.TestTrue(FString::Printf(
				TEXT("First real default after world-birth loss is finite/positive/bounded: %.9f"),
				Probe->FirstDefaultDelta),
				FMath::IsFinite(Probe->FirstDefaultDelta)
				&& Probe->FirstDefaultDelta > 0.0
				&& Probe->FirstDefaultDelta <= CortexEditorClockLease::DefaultFrameMaxDeltaSeconds);
			Test.TestTrue(TEXT("Default is non-backward from the fixed cleanup frame"),
				Probe->FirstDefaultLast >= Probe->FixedCleanupCurrent
				&& Probe->FirstDefaultCurrent >= Probe->FirstDefaultLast);
			Test.TestNull(TEXT("Provider is not resurrected after the world-birth clear"),
				GEngine->GetCustomTimeStep());
		}));

	// Fixture-owned cleanup only; Probe stays alive across it so the clock owner is not released
	// while the born world still exists.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this,
		[Fixture, Probe](FAutomationTestBase&)
		{
			(void)Probe;
			Fixture->Session->EndOwnedPIE();
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 4,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("World-birth loss remained truthfully observed through cleanup"),
				Probe->bActualWorldlessObserved);
		}));
	return true;
}

// ===========================================================================
// 4) Cortex.Editor.PhysicalInput.NativeForeignBeforeQuiescenceLateJoin  (control)
//    A foreign late-join context that appears while the owned session is still fully live
//    must survive the later owned end.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeForeignBeforeQuiescenceLateJoinTest,
	"Cortex.Editor.PhysicalInput.NativeForeignBeforeQuiescenceLateJoin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeForeignBeforeQuiescenceLateJoinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	struct FBeforeQuiescenceProbe
	{
		FDelegateHandle PieStartedHandle;
		FDelegateHandle WorldTickStartHandle;
		bool bArmForeignCapture = false;
		TWeakObjectPtr<UWorld> OwnedWorld;
		bool bForeignWorldlessObserved = false;
		FName ForeignContextHandle = NAME_None;
		TWeakObjectPtr<UWorld> ForeignWorld;
		bool bForeignTickedWhileOwnedLive = false;
		void RemoveDelegates()
		{
			FWorldDelegates::OnPIEStarted.Remove(PieStartedHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
			PieStartedHandle.Reset();
			WorldTickStartHandle.Reset();
		}
		~FBeforeQuiescenceProbe() { RemoveDelegates(); }
	};

	const auto Probe = MakeShared<FBeforeQuiescenceProbe>();
	const TWeakPtr<FBeforeQuiescenceProbe> WeakProbe = Probe;

	Probe->PieStartedHandle = FWorldDelegates::OnPIEStarted.AddLambda(
		[WeakProbe](UGameInstance*)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmForeignCapture || GEngine == nullptr) { return; }
			int32 Worldless = 0;
			FName Handle = NAME_None;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == nullptr)
				{
					++Worldless;
					Handle = Context.ContextHandle;
				}
			}
			if (Worldless == 1)
			{
				P->bForeignWorldlessObserved = true;
				P->ForeignContextHandle = Handle;
			}
		});
	Probe->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmForeignCapture || World == nullptr) { return; }
			if (World == P->OwnedWorld.Get()) { return; }
			if (World->WorldType == EWorldType::PIE)
			{
				if (!P->ForeignWorld.IsValid()) { P->ForeignWorld = World; }
				if (World == P->ForeignWorld.Get()) { P->bForeignTickedWhileOwnedLive = true; }
			}
		});

	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeForeignBeforeQuiescenceLateJoin.Request"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			Probe->OwnedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->bArmForeignCapture = true;
			Test.TestTrue(TEXT("Owned session is still live before the late join"),
				GEditor->IsPlayingSessionInEditor());
			GEditor->RequestLateJoin();
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			Test.TestTrue(TEXT("Engine recorded the pre-quiescence late-join request"),
				Info.IsSet() && Info->bLateJoinRequested);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 6,
		[Probe](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Foreign context was born while the owned session was live"),
				Probe->bForeignWorldlessObserved);
			Test.TestTrue(TEXT("Foreign world ticked while the owned session was live"),
				Probe->bForeignTickedWhileOwnedLive);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunOnceCommand(this,
		[Fixture](FAutomationTestBase&)
		{
			Fixture->Session->EndOwnedPIE();
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe](FAutomationTestBase& Test)
		{
			bool bForeignContextPresent = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE
					&& Context.ContextHandle == Probe->ForeignContextHandle
					&& Context.World() == Probe->ForeignWorld.Get())
				{
					bForeignContextPresent = true;
				}
			}
			Test.TestTrue(TEXT("Pre-quiescence foreign context survives the owned end"),
				bForeignContextPresent);
			Test.TestTrue(TEXT("Pre-quiescence foreign world still admits ticks"),
				Probe->ForeignWorld.IsValid() && Probe->ForeignWorld->ShouldTick());
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexEndOwnedInputFixture(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	return true;
}

// ===========================================================================
// 5) Cortex.Editor.PhysicalInput.NativeForeignStandardNewInProcessRequest  (control)
//    The standard new in-process request takes a different engine path: it ends the old
//    session before starting the new one (PlayLevel 1125-1132) and EndPlayMap clears the
//    queued-end flag. This documents the path distinction; it is expected Green.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeForeignStandardNewInProcessRequestTest,
	"Cortex.Editor.PhysicalInput.NativeForeignStandardNewInProcessRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeForeignStandardNewInProcessRequestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine) { AddError(TEXT("Editor engine missing")); return false; }
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid()) { AddError(TEXT("Editor world missing")); return false; }

	struct FStandardReplacementProbe
	{
		FDelegateHandle PieStartedHandle;
		FDelegateHandle WorldTickStartHandle;
		bool bArmCapture = false;
		TWeakObjectPtr<UWorld> OwnedWorld;
		bool bNewContextBorn = false;
		FName NewContextHandle = NAME_None;
		TWeakObjectPtr<UWorld> NewWorld;
		bool bNewWorldTicked = false;
		void RemoveDelegates()
		{
			FWorldDelegates::OnPIEStarted.Remove(PieStartedHandle);
			FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
			PieStartedHandle.Reset();
			WorldTickStartHandle.Reset();
		}
		~FStandardReplacementProbe() { RemoveDelegates(); }
	};

	const auto Probe = MakeShared<FStandardReplacementProbe>();
	const TWeakPtr<FStandardReplacementProbe> WeakProbe = Probe;

	Probe->PieStartedHandle = FWorldDelegates::OnPIEStarted.AddLambda(
		[WeakProbe](UGameInstance*)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmCapture || GEngine == nullptr) { return; }
			int32 Worldless = 0;
			FName Handle = NAME_None;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World() == nullptr)
				{
					++Worldless;
					Handle = Context.ContextHandle;
				}
			}
			if (Worldless == 1)
			{
				P->bNewContextBorn = true;
				P->NewContextHandle = Handle;
			}
		});
	Probe->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[WeakProbe](UWorld* World, ELevelTick, float)
		{
			const auto P = WeakProbe.Pin();
			if (!P.IsValid() || !P->bArmCapture || World == nullptr) { return; }
			if (World == P->OwnedWorld.Get()) { return; }
			if (World->WorldType == EWorldType::PIE)
			{
				if (!P->NewWorld.IsValid()) { P->NewWorld = World; }
				if (World == P->NewWorld.Get()) { P->bNewWorldTicked = true; }
			}
		});

	TestTrue(TEXT("Owned PIE admitted"),
		Fixture->Session->BeginOwnedPIE(Fixture->RequestedMap, 0, MakeFixtureReadyCallback(Fixture)).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitOwnedInputReady(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunWhenOwnedInputReady(this, Fixture,
		TEXT("NativeForeignStandardNewInProcessRequest.Request"),
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			Probe->OwnedWorld = Fixture->Session->GetTargetBinding().World;
			Probe->bArmCapture = true;

			FRequestPlaySessionParams NewRequest;
			NewRequest.SessionDestination = EPlaySessionDestinationType::InProcess;
			NewRequest.WorldType = EPlaySessionWorldType::PlayInEditor;
			NewRequest.GlobalMapOverride = Fixture->RequestedMap;
			GEditor->RequestPlaySession(NewRequest);
			Test.TestTrue(TEXT("Standard new in-process request was queued"),
				GEditor->IsPlaySessionRequestQueued());
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 8,
		[Probe, Fixture](FAutomationTestBase& Test)
		{
			Test.TestTrue(TEXT("Standard replacement born a real PIE context"),
				Probe->bNewContextBorn);
			Test.TestTrue(TEXT("Standard replacement world ticks"),
				Probe->bNewWorldTicked);

			int32 PIEContextCount = 0;
			bool bOwnedWorldStillPresent = false;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE)
				{
					++PIEContextCount;
					if (Context.World() == Probe->OwnedWorld.Get())
					{
						bOwnedWorldStillPresent = true;
					}
				}
			}
			Test.TestFalse(TEXT("Standard replacement ended the old owned world"),
				bOwnedWorldStillPresent);
			Test.TestEqual(TEXT("Exactly the standard replacement PIE context remains"),
				PIEContextCount, 1);
			Test.TestFalse(TEXT("Engine-wide queued-end flag is clear after the standard path"),
				GEditor->ShouldEndPlayMap());

			FCortexCommandResult EndError;
			Test.TestFalse(TEXT("Old session does not adopt the standard replacement"),
				Fixture->Session->ValidateTarget(EndError));
		}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCleanupPIERequests(this));
	return true;
}

// ===========================================================================
// 6) Cortex.Editor.PhysicalInput.NativeClockUnsupportedEngineAdmission
//    Stock-only leaf: an engine that does not declare the scoped-PIE session
//    capability must refuse recorded-frame clock admission before allocating a
//    provider, attaching it, mutating the timing profile or submitting PIE work.
//
// The engine declares the capability with the compile-time macro below. On a
// capability-enabled engine this leaf's premise (unsupported admission) does not
// exist, so the leaf is compiled out instead of skipped at runtime: a runtime
// skip would let an unestablished admission precondition pass silently. The leaf
// deliberately stays on the current three-argument Acquire signature.
// ===========================================================================
#if !(defined(UE_SCOPED_PIE_SESSION_API_VERSION) && UE_SCOPED_PIE_SESSION_API_VERSION == 1)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPhysicalInputNativeClockUnsupportedEngineAdmissionTest,
	"Cortex.Editor.PhysicalInput.NativeClockUnsupportedEngineAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPhysicalInputNativeClockUnsupportedEngineAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || !GEngine)
	{
		AddError(TEXT("Editor engine missing"));
		return false;
	}
	const auto Fixture = MakeShared<FCortexEditorPhysicalInputTestFixture>();
	if (!Fixture->EditorWorldBefore.IsValid())
	{
		AddError(TEXT("Editor world missing"));
		return false;
	}

	struct FUnsupportedAdmissionProbe
	{
		TUniquePtr<FCortexEditorEngineClockLease> Clock = MakeUnique<FCortexEditorEngineClockLease>();
		int32 ReadCalls = 0;
	};

	// Mandatory preconditions: a normal timing profile and a clean owned candidate editor.
	// They are asserted, never skipped: a runtime skip would let a missing admission
	// precondition pass silently instead of failing this leaf. The checks mirror the exact
	// profile the lease consumes: no foreign provider, no fixed application/engine timing,
	// no explicit delta override and no live or queued PIE work.
	UEngineCustomTimeStep* const SavedProvider = GEngine->GetCustomTimeStep();
	const bool SavedFixedFrameRate = !!GEngine->bUseFixedFrameRate;
	const float SavedFixedFrameRateValue = GEngine->FixedFrameRate;
	const bool SavedUseFixedTimeStep = FApp::UseFixedTimeStep();
	const double SavedFixedDeltaTime = FApp::GetFixedDeltaTime();
	const bool SavedBenchmarking = FApp::IsBenchmarking();
	TestNull(TEXT("Precondition: no foreign engine custom time step"), SavedProvider);
	TestFalse(TEXT("Precondition: engine fixed framerate is off"), SavedFixedFrameRate);
	TestFalse(TEXT("Precondition: application fixed-step mode is off"), SavedUseFixedTimeStep);
	TestFalse(TEXT("Precondition: application benchmarking is off"), SavedBenchmarking);
	const IConsoleVariable* const OverrideFps =
		IConsoleManager::Get().FindConsoleVariable(TEXT("t.OverrideFPS"));
	TestTrue(TEXT("Precondition: no explicit application delta override is active"),
		OverrideFps == nullptr || OverrideFps->GetFloat() < 0.001f);
	int32 PIEContextCount = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			++PIEContextCount;
		}
	}
	TestEqual(TEXT("Precondition: no PIE world context exists"), PIEContextCount, 0);
	TestFalse(TEXT("Precondition: no PIE request is queued"), GEditor->IsPlaySessionRequestQueued());
	TestFalse(TEXT("Precondition: no PIE session is running"), GEditor->IsPlayingSessionInEditor());
	TestFalse(TEXT("Precondition: no queued PIE end"), GEditor->ShouldEndPlayMap());

	const TSharedPtr<FUnsupportedAdmissionProbe> Probe = MakeShared<FUnsupportedAdmissionProbe>();
	const TWeakPtr<FUnsupportedAdmissionProbe> WeakProbe = Probe;
	const FCortexCommandResult Admission = Probe->Clock->Acquire(*GEngine, 1,
		[WeakProbe](FCortexEditorAppClockStep& Step)
		{
			const TSharedPtr<FUnsupportedAdmissionProbe> Pinned = WeakProbe.Pin();
			FCortexCommandResult Result;
			if (!Pinned.IsValid())
			{
				Result.ErrorCode = CortexErrorCodes::InvalidOperation;
				Result.ErrorMessage = TEXT("Unsupported-engine admission probe owner expired");
				return Result;
			}
			++Pinned->ReadCalls;
			// A recorded step that stays coherent with real application/wall time: if a buggy
			// baseline unexpectedly accepts this acquisition, the attached clock can pace the
			// engine without inventing time and the bounded cleanup below unwinds it safely.
			Step.LastSeconds = FApp::GetCurrentTime();
			Step.CurrentSeconds = Step.LastSeconds + 1.0 / 30.0;
			Step.DeltaSeconds = 1.0 / 30.0;
			while (FPlatformTime::Seconds() < Step.CurrentSeconds)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Result.bSuccess = true;
			return Result;
		});

	// Unwanted real acceptance/attachment is the Red this leaf exists to observe: on a stock
	// engine the current implementation has no capability preflight, so it attaches its own
	// provider and leaves the lease in Preparing here.
	TestFalse(TEXT("Unsupported engine admission is refused"), Admission.bSuccess);
	TestEqual(TEXT("Unsupported engine admission uses the shared invalid-operation code"),
		Admission.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("The refusal leaves no live recorded-frame lease"),
		Probe->Clock->GetState() == ECortexEditorClockState::Released);
	TestTrue(TEXT("Engine custom time step is the original provider after the refusal"),
		GEngine->GetCustomTimeStep() == SavedProvider);
	TestEqual(TEXT("Engine fixed-framerate setting is unchanged by the refusal"),
		!!GEngine->bUseFixedFrameRate, SavedFixedFrameRate);
	TestTrue(FString::Printf(
		TEXT("Engine fixed framerate is bit-identical after the refusal (%.9g vs %.9g)"),
		GEngine->FixedFrameRate, SavedFixedFrameRateValue),
		GEngine->FixedFrameRate == SavedFixedFrameRateValue);
	TestEqual(TEXT("Application fixed-step mode is unchanged by the refusal"),
		FApp::UseFixedTimeStep(), SavedUseFixedTimeStep);
	TestTrue(FString::Printf(
		TEXT("Application fixed delta is bit-identical after the refusal (%.17g vs %.17g)"),
		FApp::GetFixedDeltaTime(), SavedFixedDeltaTime),
		FApp::GetFixedDeltaTime() == SavedFixedDeltaTime);
	TestEqual(TEXT("Application benchmarking mode is unchanged by the refusal"),
		FApp::IsBenchmarking(), SavedBenchmarking);
	TestFalse(TEXT("The refusal queues no PIE request"), GEditor->IsPlaySessionRequestQueued());
	TestFalse(TEXT("The refusal starts no PIE session"), GEditor->IsPlayingSessionInEditor());
	TestEqual(TEXT("The refusal never invokes the recorded-step producer"), Probe->ReadCalls, 0);

	// Bounded latent cleanup. Destroying the probe's lease runs the owner's real native safety
	// cleanup: it removes the lease's own engine observers and detaches only a provider it actually
	// owns (never a fabricated Released state). The cleanup is bounded to two latent frames.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunAfterFrames(this, 2,
		[Probe, SavedProvider, SavedFixedFrameRate, SavedFixedFrameRateValue, SavedUseFixedTimeStep, SavedFixedDeltaTime](FAutomationTestBase& Test)
		{
			Probe->Clock.Reset();
			// Only an actual later producer call can show the consumer ran; after a real refusal
			// nothing was ever attached, so the count must still be zero across the cleanup frames.
			Test.TestEqual(TEXT("No recorded-step producer call occurred through this leaf"),
				Probe->ReadCalls, 0);
			Test.TestTrue(TEXT("Engine custom time step is the original provider after cleanup"),
				GEngine != nullptr && GEngine->GetCustomTimeStep() == SavedProvider);
			Test.TestEqual(TEXT("Engine fixed-framerate setting is unchanged after cleanup"),
				GEngine != nullptr && !!GEngine->bUseFixedFrameRate, SavedFixedFrameRate);
			Test.TestTrue(FString::Printf(
				TEXT("Engine fixed framerate is bit-identical after cleanup (%.9g vs %.9g)"),
				GEngine != nullptr ? GEngine->FixedFrameRate : 0.0f, SavedFixedFrameRateValue),
				GEngine != nullptr && GEngine->FixedFrameRate == SavedFixedFrameRateValue);
			Test.TestEqual(TEXT("Application fixed-step mode is unchanged after cleanup"),
				FApp::UseFixedTimeStep(), SavedUseFixedTimeStep);
			Test.TestTrue(FString::Printf(
				TEXT("Application fixed delta is bit-identical after cleanup (%.17g vs %.17g)"),
				FApp::GetFixedDeltaTime(), SavedFixedDeltaTime),
				FApp::GetFixedDeltaTime() == SavedFixedDeltaTime);
		}));
	return true;
}

#endif // Scoped-PIE session capability declared as version 1 on this engine
