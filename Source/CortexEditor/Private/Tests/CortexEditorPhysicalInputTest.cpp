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
#include "GameFramework/Pawn.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Misc/PackageName.h"
#include "PlayInEditorDataTypes.h"
#include "Slate/SceneViewport.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"

#include <limits>

namespace
{
/** Watchdogs are harness-only failure reporting; product deadlines are asserted separately. */
constexpr double CortexPhysicalInputReadyWatchdogSeconds = 15.0;
constexpr double CortexPhysicalInputProductDeadlineWatchdogSeconds = 45.0;
constexpr double CortexPhysicalInputTeardownWatchdogSeconds = 15.0;

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

	FCortexEditorPhysicalInputTestFixture()
	{
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
