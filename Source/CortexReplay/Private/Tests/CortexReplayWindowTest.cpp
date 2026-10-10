// Task 6 window regression suite: the independent human recording-library window.
//
// These tests drive the actual Slate widgets (metadata dialog drafts + Save/Cancel, delete
// confirmation, target choice) and read the real library/run-store back from disk. Widget callback
// bodies are not bypassed: Save/Cancel go through the dialog's own actions and the window's
// service-backed commit path.

#include "Misc/AutomationTest.h"

#include "CortexEditorPhysicalInput.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationEditorCommon.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SCortexReplayMetadataDialog.h"
#include "Widgets/SCortexReplayRecordingRow.h"
#include "Widgets/SCortexReplayWindow.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
/** Harness watchdogs mirror the lifecycle suite; product deadlines stay in the service. */
constexpr double WindowReadyWatchdogSeconds = 15.0;
constexpr double WindowTeardownWatchdogSeconds = 15.0;

FString HashFile(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return FString();
	}

	uint8 Digest[20];
	FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num()), Digest);
	return FString::FromHexBlob(Digest, 20u);
}

FCortexReplayMetadata ReadMetadata(FCortexReplayTestFixture& Fixture, int32 Id)
{
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	if (Library.Load(Id, false, Snapshot).bSuccess && Snapshot.IsValid())
	{
		return Snapshot->Metadata;
	}
	return FCortexReplayMetadata();
}

bool RecordingExists(FCortexReplayTestFixture& Fixture, int32 Id)
{
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	return Library.Load(Id, false, Snapshot).bSuccess;
}

FCortexReplayEvent WindowMakeKeyEvent(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event;
	Event.Sequence = Sequence;
	Event.TimeSeconds = TimeSeconds;
	Event.Input.Kind = Kind;
	Event.Input.Key = Key;
	return Event;
}

/** A press captured without an identifiable UI target guard: partial press-only coverage. */
FCortexReplayEvent MakeUnavailablePress(int32 Sequence, double TimeSeconds, const FKey& Key)
{
	FCortexReplayEvent Event = WindowMakeKeyEvent(Sequence, TimeSeconds,
		ECortexEditorPhysicalInputKind::KeyDown, Key);
	FCortexReplayInteractionGuard Guard;
	Guard.ExpectedPose = FCortexEditorPhysicalInputPlayerPose();
	Guard.UICoverage = ECortexEditorUICoverage::Unavailable;
	Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::AnonymousSlate;
	Event.Guard = Guard;
	return Event;
}

bool PublishRecording(FAutomationTestBase& Test, FCortexReplayTestFixture& Fixture, int32 Id,
	bool bAIEnabled, const FString& MapAssetPath, const TArray<FCortexReplayEvent>& Events)
{
	FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, bAIEnabled, Events);
	if (!MapAssetPath.IsEmpty())
	{
		Snapshot.Metadata.MapAssetPath = MapAssetPath;
	}
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	return Test.TestTrue(TEXT("Recording published"), Library.Publish(Snapshot).bSuccess);
}

FString RunRecordPath(const FCortexReplayTestFixture& Fixture, const FGuid& RunId)
{
	return FPaths::Combine(
		FPaths::Combine(Fixture.GetProjectRoot(), TEXT("Saved/CortexReplay/Runs")),
		RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower() + TEXT(".json"));
}

/** Writes a retained terminal run in the product's persisted local-result format. */
bool WriteRetainedRun(const FCortexReplayTestFixture& Fixture, const FGuid& RunId,
	int32 RecordingId, const FString& State)
{
	const FString RunsDir = FPaths::Combine(Fixture.GetProjectRoot(),
		TEXT("Saved/CortexReplay/Runs"));
	IFileManager::Get().MakeDirectory(*RunsDir, true);

	const FString Stamp = FDateTime::UtcNow().ToIso8601();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplayRun"));
	Object->SetNumberField(TEXT("schema_version"), 1);
	Object->SetStringField(TEXT("run_id"), RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
	Object->SetNumberField(TEXT("recording_id"), RecordingId);
	Object->SetStringField(TEXT("origin"), TEXT("ai"));
	Object->SetStringField(TEXT("state"), State);
	Object->SetStringField(TEXT("editor_instance_id"), TEXT("window-test-editor"));
	Object->SetStringField(TEXT("started_at_utc"), Stamp);
	Object->SetStringField(TEXT("finalized_at_utc"), Stamp);
	Object->SetNumberField(TEXT("dispatched_events"), 2);
	Object->SetNumberField(TEXT("total_events"), 2);
	Object->SetNumberField(TEXT("authorized_wait_seconds"), 0.0);
	Object->SetStringField(TEXT("recording_snapshot_sha256"), FString::ChrN(64, TEXT('a')));
	Object->SetStringField(TEXT("initial_state_sha256"), FString::ChrN(64, TEXT('b')));
	Object->SetStringField(TEXT("inputs_sha256"), FString::ChrN(64, TEXT('c')));

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("pose_presses"), 1);
	Coverage->SetNumberField(TEXT("ui_supported_presses"), 0);
	Coverage->SetNumberField(TEXT("ui_unavailable_presses"), 0);
	Coverage->SetNumberField(TEXT("ui_not_applicable_presses"), 1);
	Object->SetObjectField(TEXT("guard_coverage"), Coverage);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text, *RunRecordPath(Fixture, RunId));
}

/** Save through the metadata popup's own action; the window's commit path runs. */
TSharedPtr<SCortexReplayMetadataDialog> SaveMetadataThroughDialog(FAutomationTestBase& Test,
	SCortexReplayWindow& Window, int32 RecordingId, const TCHAR* Name, const TCHAR* Description,
	bool bAIEnabled)
{
	TSharedPtr<SCortexReplayMetadataDialog> Dialog = Window.OpenMetadataDialog(RecordingId);
	Test.TestTrue(TEXT("Metadata dialog opened"), Dialog.IsValid());
	if (!Dialog.IsValid())
	{
		return Dialog;
	}
	Dialog->EditDraftName(Name);
	Dialog->EditDraftDescription(Description);
	Dialog->SetDraftAIEnabled(bAIEnabled);
	Dialog->OnSaveClicked();
	// The window closes the popup only on a successful commit.
	return Dialog;
}

/** Edits drafts then Cancels, leaving the committed metadata untouched. */
TSharedPtr<SCortexReplayMetadataDialog> CancelMetadataDraft(FAutomationTestBase& Test,
	SCortexReplayWindow& Window, int32 RecordingId, const TCHAR* Name, bool bAIEnabled)
{
	TSharedPtr<SCortexReplayMetadataDialog> Dialog = Window.OpenMetadataDialog(RecordingId);
	Test.TestTrue(TEXT("Metadata dialog reopened"), Dialog.IsValid());
	if (!Dialog.IsValid())
	{
		return Dialog;
	}
	Dialog->EditDraftName(Name);
	Dialog->SetDraftAIEnabled(bAIEnabled);
	Dialog->OnCancelClicked();
	return Dialog;
}

/** A left-button row click through the row's own mouse handler (real selection input). */
void ClickRowSelection(SCortexReplayRecordingRow& Row)
{
	const TSet<FKey> PressedButtons;
	const FModifierKeysState ModifierKeys;
	const FPointerEvent Event(0u, FVector2D::ZeroVector, FVector2D::ZeroVector, PressedButtons,
		EKeys::LeftMouseButton, 0.0f, ModifierKeys);
	Row.OnMouseButtonDown(FGeometry(), Event);
}

/** Drives one real window tick with a geometry wide enough to stay in desktop layout. */
void TickWindow(SCortexReplayWindow& Window, double CurrentTime = 0.0, float DeltaSeconds = 0.05f)
{
	Window.Tick(FGeometry::MakeRoot(FVector2D(1000.0f, 800.0f), FSlateLayoutTransform()),
		CurrentTime, DeltaSeconds);
}

/**
 * Resolves the owned PIE world's selected viewport route and drives real pointer edges through
 * normal Slate routing, so capture admission does not depend on editor keyboard focus.
 */
class FWindowOwnedPointerRoute
{
public:
	bool Resolve(FAutomationTestBase& Test, UWorld& PlayWorld)
	{
		APlayerController* Controller = PlayWorld.GetFirstPlayerController();
		Test.TestNotNull(TEXT("Owned capture controller exists"), Controller);
		if (!Controller)
		{
			return false;
		}
		ULocalPlayer* LocalPlayer = PlayWorld.GetGameInstance()
			? PlayWorld.GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
		const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
		UGameViewportClient* ViewportClient = PlayWorld.GetGameViewport();
		FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
		const TSharedPtr<SViewport> ViewportWidget =
			SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
		Test.TestTrue(TEXT("Owned capture viewport widget exists"), ViewportWidget.IsValid());
		if (!SlateUser.IsValid() || !ViewportWidget.IsValid())
		{
			return false;
		}
		const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
		const FVector2D ViewportSize = Geometry.GetLocalSize();
		if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0)
		{
			return false;
		}
		Center = Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.5, ViewportSize.Y * 0.5));
		Device = IPlatformInputDeviceMapper::Get()
			.GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
		UserIndex = SlateUser->GetUserIndex();
		return true;
	}

	void PressDown()
	{
		const FModifierKeysState Modifiers;
		TSet<FKey> Pressed;
		Pressed.Add(EKeys::LeftMouseButton);
		FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr,
			FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
				Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
	}

private:
	FVector2D Center = FVector2D::ZeroVector;
	FInputDeviceId Device = INPUTDEVICEID_NONE;
	int32 UserIndex = 0;
};

/** Counts editable text/checkbox controls anywhere in a widget subtree. */
int32 CountEditableControls(const TSharedRef<SWidget>& Widget)
{
	// SWidget::GetType() returns the widget class name registered by SNew (DeclarativeSyntaxSupport.h
	// passes #WidgetType to SWidget::SetDebugInfo), which is the runtime API UE 5.8 exposes for this.
	const FName Type = Widget->GetType();
	int32 Count =
		(Type == FName(TEXT("SEditableText"))
			|| Type == FName(TEXT("SEditableTextBox"))
			|| Type == FName(TEXT("SMultiLineEditableTextBox"))
			|| Type == FName(TEXT("SCheckBox"))) ? 1 : 0;

	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			Count += CountEditableControls(Children->GetChildAt(Index));
		}
	}
	return Count;
}

/** Picks a PIE map distinct from the currently open editor map. */
FString WindowMapAssetPath()
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

constexpr TCHAR WindowPawnClassPath[] = TEXT("/Script/Engine.DefaultPawn");

bool WindowPublishPlaybackRecording(FAutomationTestBase& Test, FCortexReplayTestFixture& Fixture,
	int32 Id, const FString& MapPath, const TArray<FCortexReplayEvent>& Events, bool bAIEnabled)
{
	FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, bAIEnabled, Events);
	Snapshot.Metadata.MapAssetPath = MapPath;
	Snapshot.InitialState.PawnClassPath = WindowPawnClassPath;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	return Test.TestTrue(TEXT("Replay recording published"), Library.Publish(Snapshot).bSuccess);
}

FGuid WindowParseRunGuid(const FCortexCommandResult& Result)
{
	FGuid Id;
	if (Result.Data.IsValid())
	{
		FGuid::Parse(Result.Data->GetStringField(TEXT("run_id")), Id);
	}
	return Id;
}

FString WindowRunStateValue(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("state")) : FString();
}

bool WindowIsTerminalState(const FString& State)
{
	return State == TEXT("Completed") || State == TEXT("Cancelled")
		|| State == TEXT("Interrupted") || State == TEXT("Error");
}

TSharedRef<FCortexReplayService> MakeWindowService(const FCortexReplayTestFixture& Fixture)
{
	return MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
}

// --- Latent helpers (active-run window lifecycle) ---------------------------------------------

class FWindowRunOnce : public IAutomationLatentCommand
{
public:
	FWindowRunOnce(FAutomationTestBase* InTest, TFunction<void(FAutomationTestBase&)> InAction,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Action(MoveTemp(InAction)), KeepAlive(MoveTemp(InKeepAlive)) {}

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
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	bool bRan = false;
};

class FWindowAwaitRunTerminal : public IAutomationLatentCommand
{
public:
	FWindowAwaitRunTerminal(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId)
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a terminal window run"));
			return true;
		}
		FinalState = WindowRunStateValue(Run);
		if (WindowIsTerminalState(FinalState)) { return true; }
		if (FPlatformTime::Seconds() - StartTime > WindowReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Run never finalized (last %s)"), *FinalState));
			return true;
		}
		return false;
	}

	const FString& GetFinalState() const { return FinalState; }

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	double StartTime = 0.0;
	FString FinalState;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * Polls the window itself until the selected record reports an active replay summary, which is the
 * state the window binds separately from the current-operation label.
 */
class FWindowAwaitActiveWindowReplay : public IAutomationLatentCommand
{
public:
	FWindowAwaitActiveWindowReplay(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, TSharedRef<SCortexReplayWindow> InWindow,
		int32 InRecordingId, TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), Window(MoveTemp(InWindow))
		, RecordingId(InRecordingId), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }

		// The window must reach the active state through its own tick/transition handling.
		TickWindow(*Window);
		if (Window->GetSelectedRecordingId() != RecordingId)
		{
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(RecordingId);
			if (Row.IsValid())
			{
				ClickRowSelection(*Row);
			}
		}
		if (Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::ActiveRun
			&& Service->GetCurrentOperation().bSuccess)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > WindowReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("Window never reported an active replay summary"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	TSharedRef<SCortexReplayWindow> Window;
	int32 RecordingId;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

bool WindowAnyPIEWorld()
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

class FWindowAwaitNoPieWorlds : public IAutomationLatentCommand
{
public:
	explicit FWindowAwaitNoPieWorlds(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), KeepAlive(MoveTemp(KeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (!WindowAnyPIEWorld()) { return true; }
		if (FPlatformTime::Seconds() - StartTime > WindowTeardownWatchdogSeconds)
		{
			Test->AddError(TEXT("PIE contexts did not disappear in time"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/** Waits for an externally started PIE session to begin playing with a controller. */
class FWindowAwaitPiePlaying : public IAutomationLatentCommand
{
public:
	explicit FWindowAwaitPiePlaying(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (GEditor && GEditor->PlayWorld != nullptr
			&& GEditor->PlayWorld->HasBegunPlay()
			&& GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > WindowReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("Timed out waiting for the PIE session"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/** Waits until the service can resolve at least one ready PIE capture target. */
class FWindowAwaitReadyTargets : public IAutomationLatentCommand
{
public:
	FWindowAwaitReadyTargets(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		// Captures are always owned now: readiness means an unrelated PIE session is up, which
		// Record must then refuse rather than borrow. There is no borrowable target any more.
		if (GEditor && GEditor->PlayWorld != nullptr)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > WindowReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("No unrelated PIE session became available"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/**
 * Waits for the current-operation kind to become active (optionally a specific kind) or for the
 * backend to go idle.
 */
class FWindowAwaitOperation : public IAutomationLatentCommand
{
public:
	FWindowAwaitOperation(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FString InKind, bool bInWantActive, double InWatchdog,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), Kind(MoveTemp(InKind))
		, bWantActive(bInWantActive), Watchdog(InWatchdog), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Operation = Service->GetCurrentOperation();
		const bool bActive = Operation.bSuccess && Operation.Data.IsValid();
		if (!bWantActive)
		{
			if (!bActive) { return true; }
		}
		else if (bActive && (Kind.IsEmpty()
			|| Operation.Data->GetStringField(TEXT("kind")) == Kind))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(TEXT("Backend never reached %s (%s)"),
				bWantActive ? TEXT("active") : TEXT("idle"), *Kind));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FString Kind;
	bool bWantActive;
	double Watchdog;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/**
 * Waits for the active capture to reach one exact phase. An owned capture only becomes Recording
 * (and therefore publishable) after asynchronous PIE readiness, so a test must not Stop earlier.
 */
class FWindowAwaitCapturePhase : public IAutomationLatentCommand
{
public:
	FWindowAwaitCapturePhase(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FString InExpectedPhase, double InWatchdog,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), ExpectedPhase(MoveTemp(InExpectedPhase))
		, Watchdog(InWatchdog), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Operation = Service->GetCurrentOperation();
		if (Operation.bSuccess && Operation.Data.IsValid()
			&& Operation.Data->GetStringField(TEXT("kind")) == TEXT("capture")
			&& Operation.Data->GetStringField(TEXT("state")) == ExpectedPhase)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(
				TEXT("Capture never reached phase %s"), *ExpectedPhase));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FString ExpectedPhase;
	double Watchdog;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};
} // namespace

// ---------------------------------------------------------------------------
// Metadata Save/Cancel preserve immutable identity; read-only map/AI surfaces.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowMetadataSaveCancelTest,
	"Cortex.Replay.Window.MetadataSaveCancelAndReadOnlySurfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowMetadataSaveCancelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const int32 Id = 1;
	if (!PublishRecording(*this, Fixture, Id, false, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 WindowMakeKeyEvent(1, 0.2, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	Window->RefreshLibrary();
	Window->SelectRecording(Id);

	const FString BeforeInputs = HashFile(Fixture.GetInputsPath(Id));
	const FString BeforeInitial = HashFile(Fixture.GetInitialStatePath(Id));
	const FCortexReplayMetadata Before = ReadMetadata(Fixture, Id);
	if (!TestTrue(TEXT("Recording readable before save"), Before.RecordingId == Id))
	{
		return false;
	}

	// Read-only row surfaces: short map + full-path tooltip; no editable control in the row.
	const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(Id);
	if (!TestTrue(TEXT("Row exists for the recording"), Row.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Short map label omits the asset root"),
		!Row->GetMapLabel().ToString().Contains(TEXT("/Game/")));
	TestTrue(TEXT("Map tooltip exposes the full canonical path"),
		Row->GetMapTooltipText().ToString().Contains(Before.MapAssetPath));
	TestEqual(TEXT("Library row exposes no editable control"),
		CountEditableControls(Row.ToSharedRef()), 0);

	SaveMetadataThroughDialog(*this, *Window, Id, TEXT("Renamed"), TEXT("Updated flow"), true);
	Window->RefreshLibrary();

	const FCortexReplayMetadata Reloaded = ReadMetadata(Fixture, Id);
	TestEqual(TEXT("Input bytes unchanged"), HashFile(Fixture.GetInputsPath(Id)), BeforeInputs);
	TestEqual(TEXT("Initial bytes unchanged"), HashFile(Fixture.GetInitialStatePath(Id)), BeforeInitial);
	TestEqual(TEXT("Creation preserved"), Reloaded.CreatedAtUtc.ToString(), Before.CreatedAtUtc.ToString());
	TestEqual(TEXT("Read-only map preserved"), Reloaded.MapAssetPath, Before.MapAssetPath);
	TestEqual(TEXT("Initial hash preserved"), Reloaded.InitialStateSha256, Before.InitialStateSha256);
	TestEqual(TEXT("Inputs hash preserved"), Reloaded.InputsSha256, Before.InputsSha256);
	TestEqual(TEXT("Coverage preserved"), Reloaded.GuardCoverage.PosePresses, Before.GuardCoverage.PosePresses);
	TestEqual(TEXT("Unavailable coverage preserved"),
		Reloaded.GuardCoverage.UIUnavailablePresses, Before.GuardCoverage.UIUnavailablePresses);
	TestEqual(TEXT("Saved name"), Reloaded.Name, FString(TEXT("Renamed")));
	TestEqual(TEXT("Saved description"), Reloaded.Description, FString(TEXT("Updated flow")));
	TestTrue(TEXT("Saved AI permission"), Reloaded.bAIEnabled);

	CancelMetadataDraft(*this, *Window, Id, TEXT("Discard me"), false);
	TestEqual(TEXT("Cancel preserves committed name"), ReadMetadata(Fixture, Id).Name,
		FString(TEXT("Renamed")));
	TestTrue(TEXT("Cancel preserves committed AI permission"), ReadMetadata(Fixture, Id).bAIEnabled);

	return true;
}

// ---------------------------------------------------------------------------
// New records default to denied; delete confirmation defaults to Cancel.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowDefaultDenialAndDeleteTest,
	"Cortex.Replay.Window.NewRecordDefaultDenialAndDeleteConfirmation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowDefaultDenialAndDeleteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, false, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	Window->RefreshLibrary();

	// A freshly published record opens with the default-denied permission untouched.
	const TSharedPtr<SCortexReplayMetadataDialog> NewDialog = Window->OpenMetadataDialog(1);
	TestTrue(TEXT("New record dialog opens"), NewDialog.IsValid());
	if (NewDialog.IsValid())
	{
		TestFalse(TEXT("New record defaults to AI denied"), NewDialog->IsDraftAIEnabled());
		TestTrue(TEXT("New record has empty description"), NewDialog->GetDraftDescription().IsEmpty());
	}
	Window->CloseMetadataDialog();
	SaveMetadataThroughDialog(*this, *Window, 1, TEXT("Recording 1"), TEXT(""), false);
	TestFalse(TEXT("Default denial survives Save"), ReadMetadata(Fixture, 1).bAIEnabled);

	// Delete confirmation identifiers, focus default and cancel/confirm behavior.
	TestTrue(TEXT("Inactive record can be deleted"), Window->CanDeleteRecording(1));
	const TSharedPtr<SCortexReplayDeleteDialog> DeleteDialog = Window->OpenDeleteConfirmation(1);
	TestTrue(TEXT("Delete confirmation opens"), DeleteDialog.IsValid());
	if (!DeleteDialog.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Delete confirmation names the ID"), DeleteDialog->GetRecordingId(), 1);
	TestTrue(TEXT("Delete title identifies the recording"),
		DeleteDialog->TitleText.IsValid()
		&& DeleteDialog->TitleText->GetText().ToString().Contains(TEXT("1")));
	TestTrue(TEXT("Delete body names the recording"),
		DeleteDialog->NameText.IsValid()
		&& DeleteDialog->NameText->GetText().ToString().Contains(TEXT("Recording 1")));
	TestTrue(TEXT("Cancel owns initial focus"),
		DeleteDialog->GetInitialFocusWidget().Get() == DeleteDialog->CancelButton.Get());
	TestFalse(TEXT("Delete is not blocked for an inactive record"), DeleteDialog->IsDeleteBlocked());

	DeleteDialog->OnCancelClicked();
	Window->RefreshLibrary();
	TestTrue(TEXT("Cancel keeps the recording"), RecordingExists(Fixture, 1));

	const TSharedPtr<SCortexReplayDeleteDialog> ConfirmDialog = Window->OpenDeleteConfirmation(1);
	if (!TestTrue(TEXT("Delete confirmation reopens"), ConfirmDialog.IsValid()))
	{
		return false;
	}
	ConfirmDialog->OnConfirmClicked();
	Window->RefreshLibrary();
	TestFalse(TEXT("Confirm removes the recording"), RecordingExists(Fixture, 1));
	TestNull(TEXT("Deleted recording has no row"), Window->GetRow(1).Get());

	return true;
}

// ---------------------------------------------------------------------------
// Partial/press-only coverage warnings survive reopening and an AI-permission Save.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowCoverageWarningTest,
	"Cortex.Replay.Window.CoverageWarningsSurviveReopenAndAISave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowCoverageWarningTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, false, FString(),
		{MakeUnavailablePress(0, 0.0, EKeys::W)})
		|| !PublishRecording(*this, Fixture, 2, false, FString(),
			{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	Window->RefreshLibrary();

	const TSharedPtr<SCortexReplayMetadataDialog> Partial = Window->OpenMetadataDialog(1);
	if (!TestTrue(TEXT("Partial-coverage dialog opens"), Partial.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Unavailable UI guard warns"), Partial->HasCoverageWarning());
	TestTrue(TEXT("Warning states the press-only limit"),
		Partial->GetCoverageWarningText().ToString().Contains(TEXT("press-only")));
	Window->CloseMetadataDialog();

	const TSharedPtr<SCortexReplayMetadataDialog> Full = Window->OpenMetadataDialog(2);
	if (!TestTrue(TEXT("Full-coverage dialog opens"), Full.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("No partial warning without unavailable guards"), Full->HasCoverageWarning());
	TestTrue(TEXT("Press-only limit is always stated"),
		Full->GetCoverageWarningText().ToString().Contains(TEXT("press-only")));
	Window->CloseMetadataDialog();

	const FCortexReplayMetadata Before = ReadMetadata(Fixture, 1);
	SaveMetadataThroughDialog(*this, *Window, 1, TEXT("Enabled"), TEXT("ai on"), true);
	Window->RefreshLibrary();

	const TSharedPtr<SCortexReplayMetadataDialog> Reopened = Window->OpenMetadataDialog(1);
	if (!TestTrue(TEXT("Dialog reopens after AI Save"), Reopened.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Partial warning survives AI Save"), Reopened->HasCoverageWarning());
	TestTrue(TEXT("Press-only warning survives AI Save"),
		Reopened->GetCoverageWarningText().ToString().Contains(TEXT("press-only")));
	Window->CloseMetadataDialog();

	const FCortexReplayMetadata After = ReadMetadata(Fixture, 1);
	TestTrue(TEXT("AI permission was saved"), After.bAIEnabled);
	TestEqual(TEXT("Coverage is unchanged by AI permission"),
		After.GuardCoverage.UIUnavailablePresses, Before.GuardCoverage.UIUnavailablePresses);
	TestEqual(TEXT("Coverage pose presses unchanged"),
		After.GuardCoverage.PosePresses, Before.GuardCoverage.PosePresses);

	// A fresh window over the same library still surfaces the warning.
	const TSharedRef<SCortexReplayWindow> ReopenedWindow =
		SNew(SCortexReplayWindow).Service(Service);
	ReopenedWindow->RefreshLibrary();
	const TSharedPtr<SCortexReplayMetadataDialog> AfterReopen =
		ReopenedWindow->OpenMetadataDialog(1);
	if (!TestTrue(TEXT("Dialog opens in a reopened window"), AfterReopen.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Partial warning survives window reopen"), AfterReopen->HasCoverageWarning());

	return true;
}

// ---------------------------------------------------------------------------
// Selected-record summary reads retained local results per recording.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowSelectedSummaryTest,
	"Cortex.Replay.Window.SelectedPlaybackSummaryReadsRetainedResults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowSelectedSummaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, true, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)})
		|| !PublishRecording(*this, Fixture, 2, true, FString(),
			{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::D)}))
	{
		return false;
	}

	// Retained terminal result for recording 1 only, in the product's local-result format.
	if (!TestTrue(TEXT("Retained run written"),
		WriteRetainedRun(Fixture, FGuid::NewGuid(), 1, TEXT("Completed"))))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	Window->RefreshLibrary();

	TestTrue(TEXT("Nothing selected shows no result"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::None);

	const TSharedPtr<SCortexReplayRecordingRow> Row1 = Window->GetRow(1);
	if (TestTrue(TEXT("Row 1 exists"), Row1.IsValid()))
	{
		ClickRowSelection(*Row1);
	}
	TestTrue(TEXT("Row click selects the recording"), Window->GetSelectedRecordingId() == 1);
	TestTrue(TEXT("Retained terminal result is shown"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::PreviousTerminal);
	TestTrue(TEXT("Retained summary names Completed"),
		Window->GetSelectedPlaybackSummaryText().ToString().Contains(TEXT("Completed")));
	TestFalse(TEXT("Retained summary is not 'Not replayed yet'"),
		Window->GetSelectedPlaybackSummaryText().ToString().Contains(TEXT("Not replayed yet")));

	const TSharedPtr<SCortexReplayRecordingRow> Row2 = Window->GetRow(2);
	if (TestTrue(TEXT("Row 2 exists"), Row2.IsValid()))
	{
		ClickRowSelection(*Row2);
	}
	TestTrue(TEXT("Row click switches selection"), Window->GetSelectedRecordingId() == 2);
	TestTrue(TEXT("Recording without a run shows none"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::None);
	TestEqual(TEXT("Recording without a run says Not replayed yet"),
		Window->GetSelectedPlaybackSummaryText().ToString(), FString(TEXT("Not replayed yet")));

	const TSharedPtr<SCortexReplayRecordingRow> Row1Again = Window->GetRow(1);
	if (TestTrue(TEXT("Row 1 still exists"), Row1Again.IsValid()))
	{
		ClickRowSelection(*Row1Again);
	}
	TestTrue(TEXT("Switching back restores the retained result"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::PreviousTerminal);

	// Restart/reopen reads the retained local results again.
	const TSharedRef<FCortexReplayService> RestartedService = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> ReopenedWindow =
		SNew(SCortexReplayWindow).Service(RestartedService);
	ReopenedWindow->RefreshLibrary();
	const TSharedPtr<SCortexReplayRecordingRow> ReopenRow1 = ReopenedWindow->GetRow(1);
	if (TestTrue(TEXT("Reopened row 1 exists"), ReopenRow1.IsValid()))
	{
		ClickRowSelection(*ReopenRow1);
	}
	TestTrue(TEXT("Reopen reads the retained result"),
		ReopenedWindow->GetSelectedPlaybackSummaryKind()
			== ECortexReplayPlaybackSummary::PreviousTerminal);
	const TSharedPtr<SCortexReplayRecordingRow> ReopenRow2 = ReopenedWindow->GetRow(2);
	if (TestTrue(TEXT("Reopened row 2 exists"), ReopenRow2.IsValid()))
	{
		ClickRowSelection(*ReopenRow2);
	}
	TestTrue(TEXT("Reopen still isolates the other recording"),
		ReopenedWindow->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::None);

	return true;
}

// ---------------------------------------------------------------------------
// Desktop/narrow rows keep every required field; targets are chosen explicitly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowCompactAndTargetChoiceTest,
	"Cortex.Replay.Window.CompactRowsFollowDockedWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowCompactAndTargetChoiceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, false, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)})
		|| !PublishRecording(*this, Fixture, 2, false, FString(),
			{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::D)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	// The compact layout follows the real docked width, not a manually set flag.
	struct FWidthCase
	{
		float Width;
		bool bCompact;
	};
	const FWidthCase WidthCases[] = { {1000.0f, false}, {500.0f, true} };
	for (const FWidthCase& Case : WidthCases)
	{
		Window->Tick(FGeometry::MakeRoot(FVector2D(Case.Width, 800.0f), FSlateLayoutTransform()),
			0.0, 0.05f);
		TestEqual(TEXT("Compact layout follows the docked width"), Window->IsCompactLayout(),
			Case.bCompact);

		for (const int32 Id : { 1, 2 })
		{
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(Id);
			if (!TestTrue(TEXT("Compact/narrow row exists"), Row.IsValid()))
			{
				continue;
			}
			TestEqual(TEXT("Row follows compact layout"), Row->IsCompact(), Case.bCompact);
			TestTrue(TEXT("Row keeps its ID"), Row->IDText.IsValid()
				&& Row->IDText->GetText().ToString().StartsWith(TEXT("#")));
			TestTrue(TEXT("Row keeps its title"), Row->TitleText.IsValid());
			TestTrue(TEXT("Row keeps its description"), Row->DescriptionText.IsValid());
			TestTrue(TEXT("Row keeps its read-only map"), Row->MapText.IsValid());
			TestTrue(TEXT("Row keeps its date"), Row->DateText.IsValid());
			TestTrue(TEXT("Row keeps its read-only AI indicator"), Row->AIIndicator.IsValid());
			TestTrue(TEXT("Row keeps Play"), Row->PlayButton.IsValid());
			TestTrue(TEXT("Row keeps Edit"), Row->EditButton.IsValid());
			TestTrue(TEXT("Row keeps Delete"), Row->DeleteButton.IsValid());
		}
	}


	return true;
}

// ---------------------------------------------------------------------------
// Active run: Edit stays available, Play/Delete stay blocked, the prior terminal result is
// explicitly previous, and revocation after a focus interruption preserves that first result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowActiveRunLifecycleTest,
	"Cortex.Replay.Window.ActiveEditRevocationAndPreviousSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowActiveRunLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const FString MapPath = WindowMapAssetPath();

	if (!WindowPublishPlaybackRecording(*this, *Fixture, 1, MapPath,
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 WindowMakeKeyEvent(1, 2.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	// The window is opened after the recording exists, so the completed-run assertions observe the
	// real start/completion transitions rather than a missing initial library read.
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	// First run completes and supplies the retained previous terminal result.
	const FCortexCommandResult FirstStart = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("First AI replay admitted"), FirstStart.bSuccess);
	if (!FirstStart.bSuccess)
	{
		return false;
	}
	const FGuid FirstRunId = WindowParseRunGuid(FirstStart);

	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitRunTerminal(this, Service, FirstRunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));

	// The completed first run is only ever shown as a previous result.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			TickWindow(*Window);
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(1);
			if (T.TestTrue(TEXT("Completed run row exists"), Row.IsValid()))
			{
				ClickRowSelection(*Row);
			}
			T.TestTrue(TEXT("Completed first run is shown as previous"),
				Window->GetSelectedPlaybackSummaryKind()
					== ECortexReplayPlaybackSummary::PreviousTerminal);
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult SecondStart = Service->StartReplay(1, ECortexReplayOrigin::AI);
			T.TestTrue(TEXT("Second AI replay admitted while a prior result is retained"),
				SecondStart.bSuccess);
		}, Fixture));

	// The window cannot know the new run ID, so wait for its active-run summary directly.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitActiveWindowReplay(this, Service, Window, 1, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			TickWindow(*Window);
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(1);
			if (!T.TestTrue(TEXT("Active record row exists"), Row.IsValid()))
			{
				return;
			}
			T.TestTrue(TEXT("Edit stays available for the active record"), Row->EditButton->IsEnabled());
			T.TestFalse(TEXT("Play is blocked for the active record"), Row->PlayButton->IsEnabled());
			T.TestFalse(TEXT("Delete is blocked for the active record"), Row->DeleteButton->IsEnabled());
			T.TestFalse(TEXT("Active record cannot be deleted"), Window->CanDeleteRecording(1));
			T.TestTrue(TEXT("Active run is shown as an active summary"),
				Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::ActiveRun);
			T.TestTrue(TEXT("Prior terminal result is explicitly previous"),
				Window->GetSelectedPlaybackSummaryText().ToString().Contains(TEXT("Previous")));
		}, Fixture));

	// Focus/PIE interruption during the active run, then revoke through the Edit popup Save.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window, Service, Fixture](FAutomationTestBase& T)
		{
			TickWindow(*Window);
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(1);
			if (T.TestTrue(TEXT("Interrupted run row exists"), Row.IsValid()))
			{
				ClickRowSelection(*Row);
			}

			// The second run keeps its single terminal interruption result after revocation.
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained result is queryable"), Last.bSuccess && Last.Data.IsValid());
			const FString FirstTerminal = Last.Data.IsValid()
				? Last.Data->GetStringField(TEXT("state")) : FString();

			SaveMetadataThroughDialog(T, *Window, 1, TEXT("Revoked"), TEXT("ai off"), false);
			TickWindow(*Window);

			const FCortexCommandResult After = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained result survives revocation"), After.bSuccess && After.Data.IsValid());
			T.TestEqual(TEXT("Revocation preserves the first terminal result"),
				After.Data.IsValid() ? After.Data->GetStringField(TEXT("state")) : FString(),
				FirstTerminal);
			T.TestFalse(TEXT("Revoked permission is committed"), ReadMetadata(*Fixture, 1).bAIEnabled);
			T.TestFalse(TEXT("Revoked recording denies a new AI start"),
				Service->StartReplay(1, ECortexReplayOrigin::AI).bSuccess);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Record with no PIE must start the owned saved-map capture and surface the asynchronously
// published recording through the window's own transition handling.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowOwnedRecordTest,
	"Cortex.Replay.Window.RecordWithoutPieUsesOwnedCaptureAndPublishesRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowOwnedRecordTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("No PIE is present before Record"), !WindowAnyPIEWorld());
			// Real Record action, not a fabricated candidate array.
			Window->OnRecordClicked();
		}, Fixture));
	// An owned capture becomes Recording only after asynchronous PIE readiness; a stop before that
	// point is a preparation abort that is never published (see StopCapture's bPublish rule), so the
	// test must wait for Recording and record one real input before stopping.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitCapturePhase(this, Service, TEXT("Recording"), 60.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			TickWindow(*Window);
			T.TestTrue(TEXT("Owned capture is reported as owned"),
				Window->GetOperationLabel().ToString().Contains(TEXT("owned")));

			// Record one real key edge through normal Slate routing so the capture is non-empty.
			if (FSlateApplication::IsInitialized())
			{
				const FModifierKeysState Modifiers;
				const uint32 UserIndex = FSlateApplication::Get().GetUserIndexForKeyboard();
				FSlateApplication::Get().ProcessKeyDownEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
				FSlateApplication::Get().ProcessKeyUpEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase&)
		{
			Window->OnStopClicked();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitOperation(this, Service, TEXT(""), false, 30.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			// No manual RefreshLibrary: the publication transition must surface the new row.
			TickWindow(*Window);
			T.TestEqual(TEXT("Published owned capture appears as a row"), Window->GetRows().Num(), 1);
			if (Window->GetRows().Num() == 1)
			{
				T.TestTrue(TEXT("Published row is playable"),
					Window->GetRows()[0]->PlayButton.IsValid()
					&& Window->GetRows()[0]->PlayButton->IsEnabled());
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An external PIE/session end during an owned Recording capture publishes the recording and the
// window surfaces the new row without a manual refresh (the human ended PIE, not pressed Stop).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowExternalSessionEndRecordTest,
	"Cortex.Replay.Window.ExternalSessionEndPublishesRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowExternalSessionEndRecordTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase&)
		{
			Window->OnRecordClicked();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitCapturePhase(this, Service, TEXT("Recording"), 60.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[](FAutomationTestBase&)
		{
			// The human ends the session with the editor's own stop instead of the widget's Stop.
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitOperation(this, Service, TEXT(""), false, 30.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			// No manual RefreshLibrary: the publication transition must surface the new row.
			TickWindow(*Window);
			T.TestEqual(TEXT("External session end published a row"), Window->GetRows().Num(), 1);
			if (Window->GetRows().Num() == 1)
			{
				T.TestTrue(TEXT("Published row is playable"),
					Window->GetRows()[0]->PlayButton.IsValid()
					&& Window->GetRows()[0]->PlayButton->IsEnabled());
			}
			T.TestFalse(TEXT("No save failure is reported for the published capture"),
				Window->GetOperationLabel().ToString().Contains(TEXT("save failed")));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An external PIE/session end with an unpublishable stream saves nothing and the window names the
// reason, so the human can see why the recording was not saved.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowExternalSessionEndIncompleteTest,
	"Cortex.Replay.Window.ExternalSessionEndIncompleteSurfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowExternalSessionEndIncompleteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase&)
		{
			Window->OnRecordClicked();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitCapturePhase(this, Service, TEXT("Recording"), 60.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FWindowOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			// A press whose release is never recorded leaves the captured stream unbalanced.
			Route.PressDown();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			GEditor->RequestEndPlayMap();
		}, Fixture));
	// The immutable failure releases the operation instead of retrying the frozen stream forever.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitOperation(this, Service, TEXT(""), false, 30.0, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase& T)
		{
			TickWindow(*Window);
			const FString Label = Window->GetOperationLabel().ToString();
			// No active operation, yet the retained outcome is still the human-visible reason.
			T.TestTrue(TEXT("Window names the reason nothing was saved"),
				Label.Contains(TEXT("save failed")));
			T.TestTrue(TEXT("Window reports the incomplete stream reason"),
				Label.Contains(TEXT("unreleased held key or button")));
			T.TestTrue(TEXT("Retained reason names the held left mouse button"),
				Label.Contains(FKey(EKeys::LeftMouseButton).GetFName().ToString()));
			T.TestTrue(TEXT("Release does not fall back to the ready label"), Label != TEXT("Ready"));
			T.TestEqual(TEXT("No recording row was published"), Window->GetRows().Num(), 0);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A rejected capture admission is exposed through the operation label instead of Ready.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowAdmissionRejectionTest,
	"Cortex.Replay.Window.CaptureAdmissionRejectionSurfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowAdmissionRejectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitPiePlaying(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitReadyTargets(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window, Service](FAutomationTestBase& T)
		{
			// An unrelated PIE session is running: the owned Record request must be refused as busy
			// through the real Record entry, and that refusal must be visible rather than Ready.
			Window->BeginRecord();

			T.TestFalse(TEXT("Rejected admission starts no backend operation"),
				Service->GetCurrentOperation().Data.IsValid());
			const FString Label = Window->GetOperationLabel().ToString();
			T.TestTrue(TEXT("Rejection is exposed in the operation label"),
				!Label.IsEmpty() && Label != TEXT("Ready"));
			T.TestTrue(TEXT("The unrelated PIE world is untouched"),
				GEditor && GEditor->PlayWorld != nullptr);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A failed metadata Save keeps the popup and drafts and shows the native error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowMetadataSaveFailureTest,
	"Cortex.Replay.Window.MetadataSaveFailureKeepsDrafts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowMetadataSaveFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, false, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	Window->RefreshLibrary();

	const TSharedPtr<SCortexReplayMetadataDialog> Dialog = Window->OpenMetadataDialog(1);
	if (!TestTrue(TEXT("Metadata dialog opens"), Dialog.IsValid()))
	{
		return false;
	}

	// An empty name is rejected by the library, so the commit fails.
	Dialog->EditDraftName(TEXT(""));
	Dialog->EditDraftDescription(TEXT("draft kept"));
	Dialog->SetDraftAIEnabled(true);
	Dialog->OnSaveClicked();

	TestTrue(TEXT("Failed Save keeps the popup open"), Window->GetOpenMetadataDialog().IsValid());
	if (Window->GetOpenMetadataDialog().IsValid())
	{
		TestTrue(TEXT("Failed Save keeps the draft name"),
			Window->GetOpenMetadataDialog()->GetDraftName().IsEmpty());
		TestEqual(TEXT("Failed Save keeps the draft description"),
			Window->GetOpenMetadataDialog()->GetDraftDescription(), FString(TEXT("draft kept")));
		TestTrue(TEXT("Failed Save keeps the draft permission"),
			Window->GetOpenMetadataDialog()->IsDraftAIEnabled());
		TestTrue(TEXT("Failed Save surfaces the native error"),
			Window->GetOpenMetadataDialog()->CommitError.IsValid()
			&& !Window->GetOpenMetadataDialog()->CommitError->GetText().IsEmpty());
	}

	const FCortexReplayMetadata Committed = ReadMetadata(Fixture, 1);
	TestEqual(TEXT("Failed Save leaves the committed name"), Committed.Name,
		FString(TEXT("Recording 1")));
	TestFalse(TEXT("Failed Save leaves the committed permission"), Committed.bAIEnabled);

	// A corrected draft commits and closes the popup.
	if (Window->GetOpenMetadataDialog().IsValid())
	{
		Window->GetOpenMetadataDialog()->EditDraftName(TEXT("Recovered"));
		Window->GetOpenMetadataDialog()->OnSaveClicked();
	}
	TestNull(TEXT("Successful Save closes the popup"), Window->GetOpenMetadataDialog().Get());
	TestEqual(TEXT("Successful Save commits the corrected name"), ReadMetadata(Fixture, 1).Name,
		FString(TEXT("Recovered")));

	return true;
}

// ---------------------------------------------------------------------------
// The delete confirmation gives its Cancel button the initial Slate keyboard focus.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowDeleteFocusTest,
	"Cortex.Replay.Window.DeleteConfirmationFocusesCancelButton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowDeleteFocusTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping delete focus test"));
		return true;
	}

	FCortexReplayTestFixture Fixture;
	if (!PublishRecording(*this, Fixture, 1, false, FString(),
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}))
	{
		return false;
	}

	const TSharedRef<FCortexReplayService> Service = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	// Host the window so the confirmation is inside a live widget path with real Slate focus.
	const TSharedRef<SWindow> HostWindow =
		SNew(SWindow).ClientSize(FVector2D(640.0f, 720.0f))[Window];
	FSlateApplication::Get().AddWindow(HostWindow);
	FSlateApplication::Get().Tick(ESlateTickType::All);

	const TSharedPtr<SCortexReplayDeleteDialog> Dialog = Window->OpenDeleteConfirmation(1);
	if (!TestTrue(TEXT("Delete confirmation opens"), Dialog.IsValid()))
	{
		FSlateApplication::Get().RequestDestroyWindow(HostWindow);
		return false;
	}
	FSlateApplication::Get().Tick(ESlateTickType::All);
	TestTrue(TEXT("Cancel owns the initial keyboard focus"),
		Dialog->CancelButton.IsValid() && Dialog->CancelButton->HasKeyboardFocus());

	Dialog->OnCancelClicked();
	FSlateApplication::Get().Tick(ESlateTickType::All);
	TestFalse(TEXT("Dismissal releases the Cancel focus"),
		Dialog->CancelButton.IsValid() && Dialog->CancelButton->HasKeyboardFocus());

	FSlateApplication::Get().RequestDestroyWindow(HostWindow);
	FSlateApplication::Get().Tick(ESlateTickType::All);

	return true;
}

// ---------------------------------------------------------------------------
// A window Stop of a capture that never reached Recording surfaces why nothing was saved instead
// of silently returning to Ready (the reported human-visible gap).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowCaptureStopDiscardTest,
	"Cortex.Replay.Window.CaptureStopDiscardSurfacesReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowCaptureStopDiscardTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window, Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Owned capture admitted"),
				Service->StartCapture(WindowMapAssetPath()).bSuccess);

			// Readiness is asynchronous, so the capture is still preparing: the window Stop discards
			// it and must tell the human why nothing was saved.
			Window->StopActiveOperation();
			const FString Label = Window->GetOperationLabel().ToString();
			T.TestTrue(TEXT("Window surfaces that the capture never reached Recording"),
				Label.Contains(TEXT("never reached Recording")));
			T.TestTrue(TEXT("Discard reason is shown instead of a bare Ready"),
				Label != TEXT("Ready"));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("Discarded capture was not saved"),
				Service->GetRecording(1, false).bSuccess);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// (Folded into Cortex.Replay.Window.ExternalSessionEndIncompleteSurfaces: the retained terminal
// capture reason now covers the idle display and the held identity in one place.)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// A fresh owned capture must never adopt (borrow) an unrelated running PIE session:
// clicking Record against unrelated PIE must be refused as busy, leave no capture
// ownership and no new row, and leave the unrelated session untouched.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowFreshCaptureRefusesActivePIETest,
	"Cortex.Replay.Window.FreshCaptureRefusesActivePIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayWindowFreshCaptureRefusesActivePIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor)
	{
		AddError(TEXT("GEditor missing"));
		return false;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeWindowService(*Fixture);
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitPiePlaying(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitReadyTargets(this, Service, Fixture));

	// Idle baseline: no capture operation is active before the click.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult Before = Service->GetCurrentOperation();
			T.TestTrue(TEXT("No capture operation is active before Record"),
				Before.bSuccess && !Before.Data.IsValid());
		}, Fixture));

	// The real Record entry, with an unrelated PIE already running.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window](FAutomationTestBase&)
		{
			Window->OnRecordClicked();
		}, Fixture));

	// Next engine frame: the click must have been refused, not turned into borrowed ownership.
	ADD_LATENT_AUTOMATION_COMMAND(FWindowRunOnce(this,
		[Window, Service](FAutomationTestBase& T)
		{
			TickWindow(*Window);

			const FCortexCommandResult Current = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Unrelated PIE is not borrowed"), Current.bSuccess && !Current.Data.IsValid());
			T.TestTrue(TEXT("Original unrelated world remains live"),
				GEditor && GEditor->PlayWorld != nullptr);
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWindowAwaitNoPieWorlds(this, Fixture));

	return true;
}
