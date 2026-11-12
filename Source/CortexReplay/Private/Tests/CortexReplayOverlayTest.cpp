#include "Misc/AutomationTest.h"

#include "CortexReplayLibrary.h"
#include "CortexReplayLivePrerequisites.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Widgets/SCortexReplayRunOverlay.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Layout/WidgetPath.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"

namespace
{
constexpr double OverlayWatchdogSeconds = 30.0;
constexpr double OverlayCountdownSeconds = 1.5;
constexpr TCHAR OverlayRecordingName[] = TEXT("Overlay Walk");

FString OverlayMapAssetPath()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	return Current == TEXT("/Game/Maps/TestMap") ? TEXT("/Game/Maps/TestRoomMap")
		: TEXT("/Game/Maps/TestMap");
}

/** True when Widget is Root or one of its descendants. */
bool IsWidgetUnder(const TSharedPtr<SWidget>& Widget, const SWidget* Root)
{
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid(); Current = Current->GetParentWidget())
	{
		if (Current.Get() == Root)
		{
			return true;
		}
	}
	return false;
}

FString OverlayRunState(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("state")) : FString();
}

/**
 * Observes the run's overlay once per engine frame: shown during the countdown, replaced by the
 * banner on Replaying, never hit-testable, gone when the run is terminal.
 */
class FCortexReplayObserveOverlay : public IAutomationLatentCommand
{
public:
	FCortexReplayObserveOverlay(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, TSharedPtr<FCortexReplayTestFixture> InKeepAlive)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0)
		{
			StartTime = FPlatformTime::Seconds();
		}
		if (FPlatformTime::Seconds() - StartTime > OverlayWatchdogSeconds)
		{
			Test->AddError(TEXT("The overlay observation timed out"));
			return true;
		}

		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while observing the overlay"));
			return true;
		}
		const FString State = OverlayRunState(Run);
		const bool bTerminal = State == TEXT("Completed") || State == TEXT("Cancelled")
			|| State == TEXT("Interrupted") || State == TEXT("Error");
		const bool bGone = bTerminal || State == TEXT("Finalizing");
		const TSharedPtr<SWidget> Overlay = Service->GetRunOverlayForTesting();

		if (bGone)
		{
			if (!Seen.IsValid())
			{
				Test->AddError(TEXT("The run ended without ever showing an overlay"));
				return true;
			}
			if (!bCheckedGone)
			{
				bCheckedGone = true;
				Test->TestFalse(TEXT("The service holds no overlay once the run is finalizing"), Overlay.IsValid());
				Test->TestFalse(TEXT("The game viewport no longer holds the overlay"),
					Seen->GetParentWidget().IsValid());
				Test->TestTrue(TEXT("The countdown was observed before playback"), bSawCountdown);
				Test->TestTrue(TEXT("The playing banner was observed"), bSawPlaying);
			}
			return bTerminal;
		}

		if (!Overlay.IsValid())
		{
			return false;
		}
		Seen = Overlay;
		const FString Headline =
			StaticCastSharedPtr<SCortexReplayRunOverlay>(Overlay)->GetHeadline().ToString();

		if (State == TEXT("Preparing") && Headline.Contains(TEXT("don't touch the mouse or keyboard")))
		{
			if (!bSawCountdown)
			{
				bSawCountdown = CheckOverlay(Overlay, TEXT("countdown"));
				if (bSawCountdown)
				{
					Test->TestTrue(TEXT("The countdown headline names the hands-off request in full"),
						Headline.Contains(TEXT("Replay starting")));
				}
			}
		}
		else if (State == TEXT("Replaying") && Headline.StartsWith(TEXT("Replaying '")))
		{
			if (!bSawPlaying)
			{
				bSawPlaying = CheckOverlay(Overlay, TEXT("playing"));
				if (bSawPlaying)
				{
					Test->TestTrue(TEXT("The banner names the recording"),
						Headline.Contains(FString::Printf(TEXT("'%s'"), OverlayRecordingName)));
					Test->TestTrue(TEXT("The banner says touching input stops the replay"),
						Headline.Contains(TEXT("touching the mouse or keyboard stops it")));
				}
			}
		}
		return false;
	}

private:
	/** Returns false while the overlay has no cached geometry yet, so the check is retried next frame. */
	bool CheckOverlay(const TSharedPtr<SWidget>& Overlay, const TCHAR* Label)
	{
		UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
		UGameViewportClient* ViewportClient = PlayWorld ? PlayWorld->GetGameViewport() : nullptr;
		FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
		const TSharedPtr<SViewport> ViewportWidget =
			SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
		if (!ViewportWidget.IsValid() || !FSlateApplication::IsInitialized())
		{
			Test->AddError(FString::Printf(TEXT("%s: the owned PIE viewport is missing"), Label));
			return true;
		}
		const FGeometry Geometry = Overlay->GetCachedGeometry();
		if (Geometry.GetLocalSize().X <= 0.0 || Geometry.GetLocalSize().Y <= 0.0)
		{
			return false;
		}

		Test->TestEqual(FString::Printf(TEXT("%s: the overlay is hit-test invisible"), Label),
			Overlay->GetVisibility(), EVisibility::HitTestInvisible);
		Test->TestFalse(FString::Printf(TEXT("%s: the overlay never takes keyboard focus"), Label),
			Overlay->SupportsKeyboardFocus());
		bool bInViewportTree = false;
		for (TSharedPtr<SWidget> Current = Overlay; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (Current.Get() == ViewportWidget.Get())
			{
				bInViewportTree = true;
				break;
			}
		}
		Test->TestTrue(FString::Printf(TEXT("%s: the overlay is in the PIE viewport tree"), Label),
			bInViewportTree);

		int32 UserIndex = 0;
		ULocalPlayer* LocalPlayer = PlayWorld->GetGameInstance()
			? PlayWorld->GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
		if (const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr)
		{
			UserIndex = SlateUser->GetUserIndex();
		}
		const FVector2D Center = Geometry.GetAbsolutePositionAtCoordinates(FVector2D(0.5, 0.5));
		const FWidgetPath Path = FSlateApplication::Get().LocateWindowUnderMouse(
			Center, FSlateApplication::Get().GetInteractiveTopLevelWindows(), false, UserIndex);
		bool bPathHasViewport = false;
		bool bPathHasOverlay = false;
		for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
		{
			const TSharedPtr<SWidget> PathWidget = Path.Widgets[Index].Widget;
			bPathHasViewport |= PathWidget.Get() == ViewportWidget.Get();
			bPathHasOverlay |= IsWidgetUnder(PathWidget, Overlay.Get());
		}
		Test->TestTrue(FString::Printf(TEXT("%s: the hit test reaches the game viewport (control)"), Label),
			bPathHasViewport);
		Test->TestFalse(FString::Printf(TEXT("%s: the overlay is not in the widget path under the cursor"), Label),
			bPathHasOverlay);
		return true;
	}

	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	TSharedPtr<SWidget> Seen;
	double StartTime = 0.0;
	bool bSawCountdown = false;
	bool bSawPlaying = false;
	bool bCheckedGone = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayOverlayLifecycleTest,
	"Cortex.Replay.Overlay.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayOverlayLifecycleTest::RunTest(const FString& Parameters)
{
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping the overlay lifecycle test"));
		return true;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture->GetProjectRoot());
	Service->SetCountdownSecondsForTesting(OverlayCountdownSeconds);

	TArray<FCortexReplayTimedEvent> Events;
	FCortexReplayTimedEvent Down;
	Down.TimeSeconds = 0.0;
	Down.Event.Sequence = 0;
	Down.Event.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Down.Event.Input.Key = EKeys::W;
	Events.Add(Down);
	FCortexReplayTimedEvent Up = Down;
	Up.TimeSeconds = 1.0;
	Up.Event.Sequence = 1;
	Up.Event.Input.Kind = ECortexEditorPhysicalInputKind::KeyUp;
	Events.Add(Up);

	FCortexReplaySnapshot Snapshot = Fixture->MakeRecording(1, true, Events);
	Snapshot.Metadata.Name = OverlayRecordingName;
	Snapshot.Metadata.MapAssetPath = OverlayMapAssetPath();
	Snapshot.InitialState.PawnClassPath = TEXT("/Script/Engine.DefaultPawn");
	FCortexReplayLibrary Library(Fixture->GetProjectRoot());
	if (!TestTrue(TEXT("Recording published"), Library.Publish(Snapshot).bSuccess))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("AI replay admitted"), Started.bSuccess))
	{
		return false;
	}
	FGuid RunId;
	if (Started.Data.IsValid())
	{
		FGuid::Parse(Started.Data->GetStringField(TEXT("run_id")), RunId);
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayObserveOverlay(this, Service, RunId, Fixture));
	return true;
}
