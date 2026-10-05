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
