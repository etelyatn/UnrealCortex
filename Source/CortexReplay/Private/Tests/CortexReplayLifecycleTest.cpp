#include "Misc/AutomationTest.h"

#include "Application/ThrottleManager.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayLivePrerequisites.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
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
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeCounter.h"
#include "Input/Events.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
/** Harness-only failure reporting; the product deadline itself is asserted separately. */
constexpr double ReplayReadyWatchdogSeconds = 15.0;
constexpr double ReplayProductDeadlineWatchdogSeconds = 45.0;
constexpr double ReplayTeardownWatchdogSeconds = 15.0;

/** The recorded pawn class must load on the owned PIE target; the sandbox maps spawn a DefaultPawn. */
constexpr TCHAR ReplayPawnClassPath[] = TEXT("/Script/Engine.DefaultPawn");

/** Restores the human's fixed-step settings when the last reference dies, whatever a test did. */
struct FCortexReplayFixedStepBaseline
{
	bool bUseFixedTimeStep = FApp::UseFixedTimeStep();
	double FixedDeltaSeconds = FApp::GetFixedDeltaTime();

	~FCortexReplayFixedStepBaseline()
	{
		FApp::SetUseFixedTimeStep(bUseFixedTimeStep);
		FApp::SetFixedDeltaTime(FixedDeltaSeconds);
	}
};

/** A distinctive human fixed delta, so restoration is proven exact rather than a reset to a default. */
constexpr double ReplayHumanFixedDeltaSeconds = 1.0 / 7.0;

/** Saves the human's clock and installs the distinctive human fixed delta with fixed stepping off. */
TSharedPtr<FCortexReplayFixedStepBaseline> MakeDistinctHumanClock()
{
	const TSharedPtr<FCortexReplayFixedStepBaseline> Baseline = MakeShared<FCortexReplayFixedStepBaseline>();
	FApp::SetUseFixedTimeStep(false);
	FApp::SetFixedDeltaTime(ReplayHumanFixedDeltaSeconds);
	return Baseline;
}

/** Asserts that a finished run handed the engine clock back exactly as the human left it. */
void TestHumanClockRestored(FAutomationTestBase& Test)
{
	Test.TestFalse(TEXT("Fixed stepping is off after the run"), FApp::UseFixedTimeStep());
	Test.TestEqual(TEXT("The human fixed delta is restored exactly"),
		FApp::GetFixedDeltaTime(), ReplayHumanFixedDeltaSeconds);
}

/**
 * The map the recording names, chosen as the alternative to the currently open editor map so the
 * owned PIE load cannot disturb the editor world (same convention as the shared session tests).
 */
FString MakeReplayMapAssetPath()
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

FCortexReplayTimedEvent MakeKeyPress(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayTimedEvent Timed;
	Timed.TimeSeconds = TimeSeconds;
	Timed.Event.Sequence = Sequence;
	Timed.Event.Input.Kind = Kind;
	Timed.Event.Input.Key = Key;
	return Timed;
}

/**
 * Resolves the owned PIE world's selected viewport route and drives real pointer edges through
 * normal Slate routing, so capture admission does not depend on editor keyboard focus. Mirrors the
 * route resolution used by the shared physical-input tests.
 */
class FCortexReplayOwnedPointerRoute
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
		Viewport = ViewportWidget;
		return true;
	}

	/** Focuses the route's viewport and presses the editor's stop-play key without releasing it. */
	void PressStopKey(bool bRepeat = false)
	{
		const FModifierKeysState Modifiers;
		FSlateApplication::Get().SetUserFocus(UserIndex, Viewport, EFocusCause::SetDirectly);
		FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::Escape, Modifiers, UserIndex, bRepeat, 27, 0));
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

	void PressUp()
	{
		const FModifierKeysState Modifiers;
		const TSet<FKey> Pressed;
		FSlateApplication::Get().ProcessMouseButtonUpEvent(
			FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
				Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
	}

private:
	FVector2D Center = FVector2D::ZeroVector;
	TSharedPtr<SViewport> Viewport;
	FInputDeviceId Device = INPUTDEVICEID_NONE;
	int32 UserIndex = 0;
};

/** Publishes one valid AI-eligible recording whose map is a real loadable PIE map. */
bool PublishReplayRecording(FAutomationTestBase& Test, FCortexReplayTestFixture& Fixture,
	int32 Id, const FString& MapPath, const TArray<FCortexReplayTimedEvent>& Events, bool bAIEnabled,
	double DurationOverrideSeconds = 0.0)
{
	FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, bAIEnabled, Events);
	Snapshot.Metadata.MapAssetPath = MapPath;
	Snapshot.InitialState.PawnClassPath = ReplayPawnClassPath;
	if (DurationOverrideSeconds > 0.0)
	{
		Snapshot.Metadata.DurationSeconds = FMath::Max(Snapshot.Metadata.DurationSeconds,
			DurationOverrideSeconds);
		// The recorded frames must cover the overridden duration, so the idle tail is real frames.
		TArray<double> EventTimes;
		EventTimes.Reserve(Events.Num());
		for (const FCortexReplayTimedEvent& Timed : Events)
		{
			EventTimes.Add(Timed.TimeSeconds);
		}
		FCortexReplayTestFixture::ApplyCadenceFrames(Snapshot, EventTimes);
	}
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	return Test.TestTrue(TEXT("Recording published"), Library.Publish(Snapshot).bSuccess);
}

FString ReplayMetadataPath(const FCortexReplayTestFixture& Fixture, int32 Id)
{
	return FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(Fixture.GetProjectRoot(), TEXT(".cortex/replay/recordings")),
			FString::FromInt(Id)),
		TEXT("metadata.json"));
}

FString ReplayRunRecordPath(const FCortexReplayTestFixture& Fixture, const FGuid& Id)
{
	return FPaths::Combine(
		FPaths::Combine(Fixture.GetProjectRoot(), TEXT("Saved/CortexReplay/Runs")),
		Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower() + TEXT(".json"));
}

bool SaveJsonObjectFile(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text, *Path);
}

/** Builds a retained terminal run record; the coverage counts are caller-supplied deliberately. */
TSharedRef<FJsonObject> MakeRetainedRunRecordJson(const FGuid& Id, int32 RecordingId,
	const FString& State, int32 Dispatched, int32 Total, int32 PosePresses, int32 SupportedPresses,
	int32 UnavailablePresses, int32 NotApplicablePresses)
{
	const FString Stamp = FDateTime::UtcNow().ToIso8601();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplayRun"));
	Object->SetNumberField(TEXT("schema_version"), 3);
	Object->SetStringField(TEXT("run_id"), Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
	Object->SetNumberField(TEXT("recording_id"), RecordingId);
	Object->SetStringField(TEXT("origin"), TEXT("ai"));
	Object->SetStringField(TEXT("state"), State);
	Object->SetStringField(TEXT("editor_instance_id"), TEXT("test-editor"));
	Object->SetStringField(TEXT("started_at_utc"), Stamp);
	Object->SetStringField(TEXT("finalized_at_utc"), Stamp);
	Object->SetNumberField(TEXT("dispatched_events"), Dispatched);
	Object->SetNumberField(TEXT("total_events"), Total);
	Object->SetNumberField(TEXT("authorized_wait_seconds"), 0.0);
	Object->SetNumberField(TEXT("completed_frames"), 2);
	Object->SetNumberField(TEXT("frame_count"), 2);
	Object->SetStringField(TEXT("frames_sha256"), FString::ChrN(64, TEXT('d')));
	Object->SetNumberField(TEXT("behind_seconds"), 0.0);
	Object->SetStringField(TEXT("recording_snapshot_sha256"), FString::ChrN(64, TEXT('a')));
	Object->SetStringField(TEXT("initial_state_sha256"), FString::ChrN(64, TEXT('b')));
	Object->SetStringField(TEXT("inputs_sha256"), FString::ChrN(64, TEXT('c')));

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("pose_presses"), PosePresses);
	Coverage->SetNumberField(TEXT("ui_supported_presses"), SupportedPresses);
	Coverage->SetNumberField(TEXT("ui_unavailable_presses"), UnavailablePresses);
	Coverage->SetNumberField(TEXT("ui_not_applicable_presses"), NotApplicablePresses);
	Object->SetObjectField(TEXT("guard_coverage"), Coverage);
	return Object;
}

/** Rewrites only the live permission bit of a published recording's metadata. */
bool SetMetadataAIEnabled(const FString& MetadataPath, bool bEnabled)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *MetadataPath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	Object->SetBoolField(TEXT("ai_enabled"), bEnabled);
	return SaveJsonObjectFile(MetadataPath, Object.ToSharedRef());
}

FString RunState(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("state")) : FString();
}

FGuid ParseRunId(const FCortexCommandResult& Result)
{
	FGuid Id;
	if (Result.Data.IsValid())
	{
		FGuid::Parse(Result.Data->GetStringField(TEXT("run_id")), Id);
	}
	return Id;
}

bool IsTerminalRunState(const FString& State)
{
	return State == TEXT("Completed") || State == TEXT("Cancelled")
		|| State == TEXT("Interrupted") || State == TEXT("Error");
}

FString RunExecutionErrorCode(const FCortexCommandResult& Result)
{
	const TSharedPtr<FJsonObject>* Error = nullptr;
	if (Result.Data.IsValid()
		&& Result.Data->TryGetObjectField(TEXT("execution_error"), Error)
		&& Error != nullptr && Error->IsValid())
	{
		return (*Error)->GetStringField(TEXT("code"));
	}
	return FString();
}

int32 RunDispatchedEvents(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid()
		? static_cast<int32>(Result.Data->GetNumberField(TEXT("dispatched_events"))) : INDEX_NONE;
}

bool IsPIEWorldPresentForMap(const FString& MapAssetPath)
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

/** The owned PIE world's game viewport widget; this is the session's selected route root. */
TSharedPtr<SWidget> ResolveOwnedRouteRootWidget()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE || Context.GameViewport == nullptr)
		{
			continue;
		}
		if (const TSharedPtr<SViewport> Widget = Context.GameViewport->GetGameViewportWidget())
		{
			return Widget;
		}
	}
	return nullptr;
}

/** True when Widget is the route root or one of its descendants. */
bool IsWidgetOnRouteRoot(const TSharedPtr<SWidget>& Widget, const TSharedPtr<SWidget>& RouteRoot)
{
	if (!RouteRoot.IsValid())
	{
		return false;
	}
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid(); Current = Current->GetParentWidget())
	{
		if (Current.Get() == RouteRoot.Get())
		{
			return true;
		}
	}
	return false;
}

/**
 * Stand-in for the human's pre-existing focus owner (the replay recorder): a focusable widget in a
 * separate Slate window. Keyboard events route only to a focused widget, so a nonzero key count
 * proves replay keys reached this foreign consumer instead of the owned route.
 */
class SCortexReplayForeignKeyboardSink : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayForeignKeyboardSink) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		ChildSlot
		[
			SNew(STextBlock).Text(FText::FromString(TEXT("foreign keyboard sink")))
		];
	}

	virtual bool SupportsKeyboardFocus() const override { return true; }

	virtual FReply OnKeyDown(const FGeometry& /*MyGeometry*/, const FKeyEvent& /*InKeyEvent*/) override
	{
		++KeyDownCount;
		return FReply::Handled();
	}

	int32 KeyDownCount = 0;
};

TSharedRef<FCortexReplayService> MakeService(const FCortexReplayTestFixture& Fixture)
{
	return MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
}

// ---------------------------------------------------------------------------
// Latent commands
// ---------------------------------------------------------------------------

/** Runs one synchronous check once. */
class FCortexReplayRunOnce : public IAutomationLatentCommand
{
public:
	FCortexReplayRunOnce(FAutomationTestBase* InTest, TFunction<void(FAutomationTestBase&)> InAction,
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
	/**
	 * Keeps the owning fixture (and therefore its temporary project root) alive for as long as
	 * this latent command exists: latent commands run after RunTest returns, so a local fixture
	 * reference would otherwise delete the recorded library while the service still needs it.
	 */
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	bool bRan = false;
};

/** Polls get_run until the run reports one exact state. */
class FCortexReplayAwaitRunState : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRunState(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, FString InExpected, TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr,
		double InWatchdog = ReplayReadyWatchdogSeconds * 2)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId)
		, Expected(MoveTemp(InExpected)), Watchdog(InWatchdog), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a run state"));
			return true;
		}
		LastState = RunState(Run);
		if (LastState == Expected) { return true; }
		if (IsTerminalRunState(LastState) && LastState != Expected)
		{
			Test->AddError(FString::Printf(TEXT("Run reached %s instead of %s"), *LastState, *Expected));
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(TEXT("Run never reached %s (last %s)"), *Expected, *LastState));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	FString Expected;
	double Watchdog;
	double StartTime = 0.0;
	FString LastState;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * Polls get_run with an explicit AI-only scope until the run reports one exact state, then runs
 * one synchronous check in the same frame so an observed epoch arming mode is not lost to a later
 * transition.
 */
class FCortexReplayAwaitStateThenCheck : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitStateThenCheck(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, TSharedPtr<FGuid> InRunId, FString InExpected,
		bool bInAIOnly, TFunction<void(FAutomationTestBase&)> InCheck,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr,
		double InWatchdog = ReplayReadyWatchdogSeconds * 2)
		: Test(InTest), Service(MoveTemp(InService)), RunId(MoveTemp(InRunId))
		, Expected(MoveTemp(InExpected)), bAIOnly(bInAIOnly), Check(MoveTemp(InCheck))
		, Watchdog(InWatchdog), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(*RunId, bAIOnly);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a run state"));
			return true;
		}
		const FString State = RunState(Run);
		if (State == Expected)
		{
			if (Check) { Check(*Test); }
			return true;
		}
		if (IsTerminalRunState(State) && State != Expected)
		{
			Test->AddError(FString::Printf(TEXT("Run reached %s instead of %s"), *State, *Expected));
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(TEXT("Run never reached %s (last %s)"), *Expected, *State));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	TSharedPtr<FGuid> RunId;
	FString Expected;
	bool bAIOnly = true;
	TFunction<void(FAutomationTestBase&)> Check;
	double Watchdog = 0.0;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Polls get_run until the run reports any terminal state and stores that state. */
class FCortexReplayAwaitRunTerminal : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRunTerminal(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr,
		double InWatchdog = ReplayReadyWatchdogSeconds * 2)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), Watchdog(InWatchdog)
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a terminal run"));
			return true;
		}
		FinalState = RunState(Run);
		if (IsTerminalRunState(FinalState)) { return true; }
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
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
	double Watchdog;
	double StartTime = 0.0;
	FString FinalState;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until no PIE world context remains. */
class FCortexReplayAwaitNoPieWorlds : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayAwaitNoPieWorlds(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), KeepAlive(MoveTemp(InKeepAlive)) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (!HasAnyPIEWorldContext()) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayTeardownWatchdogSeconds)
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

/** Waits for an externally started PIE session to begin playing. */
class FCortexReplayAwaitExternalPiePlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayAwaitExternalPiePlaying(FAutomationTestBase* InTest,
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
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("Timed out waiting for the external PIE session"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/** Polls get_run until the dispatched-event count is reached. */
class FCortexReplayAwaitDispatchedEvents : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitDispatchedEvents(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, int32 InCount,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), Count(InCount)
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting dispatched events"));
			return true;
		}
		if (RunDispatchedEvents(Run) >= Count) { return true; }
		if (IsTerminalRunState(RunState(Run))
			|| FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Run never dispatched %d events (last %s)"),
				Count, *RunState(Run)));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	int32 Count = 0;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Polls a freshly constructed service until the retained terminal record is queryable. */
class FCortexReplayAwaitRetainedRun : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRetainedRun(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InFixture, FGuid InRunId, FString InExpected,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Fixture(MoveTemp(InFixture)), RunId(InRunId)
		, Expected(MoveTemp(InExpected)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		// A new service re-loads the retained history from disk on construction.
		FCortexReplayService Probe(Fixture->GetProjectRoot());
		const FCortexCommandResult Run = Probe.GetRun(RunId, true);
		if (Run.bSuccess && RunState(Run) == Expected) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Retained run never became %s"), *Expected));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> Fixture;
	FGuid RunId;
	FString Expected;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the native service reports no active capture operation. */
class FCortexReplayAwaitCaptureIdle : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCaptureIdle(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && !Current.Data.IsValid()) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("Capture never released its retained publication state"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the native service reports the expected capture state. */
class FCortexReplayAwaitCapturePhase : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCapturePhase(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FString InExpected,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), Expected(MoveTemp(InExpected))
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && Current.Data.IsValid()
			&& Current.Data->GetStringField(TEXT("state")) == Expected)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Capture never reached %s"), *Expected));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FString Expected;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the capture status reports a retained publication failure. */
class FCortexReplayAwaitCapturePublicationFailure : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCapturePublicationFailure(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && Current.Data.IsValid()
			&& Current.Data->GetBoolField(TEXT("publication_failed")))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("Capture publication failure was never reported"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * While the matching owned PIE context exists after cancellation, get_run must stay Finalizing,
 * no further input may be dispatched and competing starts must be busy. The stable terminal
 * result becomes queryable only after the product's own teardown removed the context.
 */
class FCortexReplayObserveFinalizing : public IAutomationLatentCommand
{
public:
	FCortexReplayObserveFinalizing(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, FString InMapPath,
		int32 InAdmittedRecordingId, TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), MapPath(MoveTemp(InMapPath))
		, AdmittedRecordingId(InAdmittedRecordingId), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed during finalization"));
			return true;
		}

		if (IsPIEWorldPresentForMap(MapPath))
		{
			if (!bObservedContext)
			{
				bObservedContext = true;
				DispatchedAtFinalizing = RunDispatchedEvents(Run);
				Test->TestTrue(TEXT("get_run remains Finalizing while the owned context exists"),
					RunState(Run) == TEXT("Finalizing"));
				const FCortexCommandResult Competing = Service->StartReplay(AdmittedRecordingId,
					ECortexReplayOrigin::AI);
				Test->TestFalse(TEXT("Competing start is busy during finalization"), Competing.bSuccess);
			}
			else
			{
				Test->TestEqual(TEXT("No input dispatched during finalization"),
					RunDispatchedEvents(Run), DispatchedAtFinalizing);
			}
			if (FPlatformTime::Seconds() - StartTime > ReplayTeardownWatchdogSeconds * 2)
			{
				Test->AddError(TEXT("Owned context never disappeared after cancellation"));
				return true;
			}
			return false;
		}

		Test->TestTrue(TEXT("Owned context really existed during finalization"), bObservedContext);

		// The product retains its finalization ticker until IsOwnedPIEEnded() confirms teardown and
		// only then publishes the stable terminal result, so allow that documented bounded window
		// instead of requiring the publication in the same frame the context disappeared.
		if (!IsTerminalRunState(RunState(Run)))
		{
			if (FPlatformTime::Seconds() - StartTime <= ReplayTeardownWatchdogSeconds * 2)
			{
				return false;
			}
			Test->AddError(TEXT("Owned PIE teardown never reached a stable terminal state"));
			return true;
		}
		Test->TestTrue(TEXT("Stable terminal state after product-owned teardown"),
			IsTerminalRunState(RunState(Run)));
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	FString MapPath;
	int32 AdmittedRecordingId = 0;
	double StartTime = 0.0;
	bool bObservedContext = false;
	int32 DispatchedAtFinalizing = INDEX_NONE;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * Test-only prerequisite gate: keeps the owned PIE world created after the baseline from
 * possessing a pawn or from having its owned viewport registered, exposing exactly the
 * unavailability the product validates. It never touches an existing/external PIE session.
 */
enum class EReplayPreparationGateMode : uint8 { Possession, Viewport };

class FReplayPreparationGate
{
public:
	FReplayPreparationGate(EReplayPreparationGateMode InMode, FString InMapPath)
		: Mode(InMode), MapPath(MoveTemp(InMapPath)) {}

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

	void Enforce()
	{
		if (!bClosed)
		{
			return;
		}
		UWorld* World = FindOwnedPIEWorld();
		if (World == nullptr)
		{
			return;
		}
		GatedWorld = World;
		if (Mode == EReplayPreparationGateMode::Possession)
		{
			bPrerequisiteWithheld |= EnforceMissingPawn(World);
		}
		else
		{
			bPrerequisiteWithheld |= EnforceUnregisteredViewport(World);
		}
	}

	bool WasPrerequisiteWithheld() const { return bPrerequisiteWithheld; }
	const FString& GetMapPath() const { return MapPath; }

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
		if (Mode == EReplayPreparationGateMode::Viewport
			&& WithheldViewportClient.IsValid() && WithheldSceneViewport != nullptr)
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
		const FString RequestedPackage = FPackageName::ObjectPathToPackageName(MapPath);
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Context.WorldType == EWorldType::PIE && Candidate != nullptr
				&& !BaselineWorlds.Contains(Candidate)
				&& UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) == RequestedPackage)
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
			WithheldSceneViewport = Client->GetGameViewport();
		}
		if (WithheldSceneViewport != nullptr && Client->GetGameViewport() == WithheldSceneViewport)
		{
			Client->RemoveAssociation(*WithheldSceneViewport);
		}
		return WithheldSceneViewport != nullptr;
	}

	EReplayPreparationGateMode Mode;
	FString MapPath;
	bool bClosed = true;
	bool bPrerequisiteWithheld = false;
	TSubclassOf<APawn> SavedDefaultPawnClass;
	bool bSavedDefaultPawnClass = false;
	TArray<TWeakObjectPtr<UWorld>> BaselineWorlds;
	TWeakObjectPtr<UWorld> GatedWorld;
	TWeakObjectPtr<UGameViewportClient> WithheldViewportClient;
	FSceneViewport* WithheldSceneViewport = nullptr;
};

/**
 * Holds the real missing-pawn/missing-viewport prerequisite until the product's own preparation
 * deadline fails, then asserts the retained Error/INPUT_PREPARATION_TIMEOUT with zero dispatched
 * events, that a late readiness signal cannot mutate the expired run, and that a successor can
 * still be admitted.
 */
class FCortexReplayGatePreparationTimeout : public IAutomationLatentCommand
{
public:
	FCortexReplayGatePreparationTimeout(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, int32 InRecordingId,
		TSharedRef<FReplayPreparationGate> InGate,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId)
		, RecordingId(InRecordingId), Gate(MoveTemp(InGate)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (!bAdmitted)
		{
			bAdmitted = true;
			Deadline = FPlatformTime::Seconds() + ReplayProductDeadlineWatchdogSeconds;
		}

		Gate->Enforce();

		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting the preparation timeout"));
			return true;
		}

		if (!bTimeoutSeen)
		{
			if (!IsTerminalRunState(RunState(Run)))
			{
				if (FPlatformTime::Seconds() > Deadline)
				{
					Test->AddError(TEXT("Replay preparation never timed out"));
					return true;
				}
				return false;
			}

			bTimeoutSeen = true;
			Test->TestTrue(TEXT("Preparation failed as a terminal Error"), RunState(Run) == TEXT("Error"));
			Test->TestEqual(TEXT("Retained primary preparation code"),
				RunExecutionErrorCode(Run), FString(TEXT("INPUT_PREPARATION_TIMEOUT")));
			Test->TestEqual(TEXT("Preparation failure dispatched nothing"),
				RunDispatchedEvents(Run), 0);
			Test->TestTrue(TEXT("Withheld prerequisite was really observed"), Gate->WasPrerequisiteWithheld());
			FrozenState = RunState(Run);
			FrozenDispatched = RunDispatchedEvents(Run);
			FrozenErrorCode = RunExecutionErrorCode(Run);
			// A late readiness signal appears only now, after the product already failed.
			Gate->Release();
			ReleaseTime = FPlatformTime::Seconds();
			return false;
		}

		// The released prerequisite must not resurrect or mutate the expired run.
		if (FPlatformTime::Seconds() - ReleaseTime <= 2.0)
		{
			return false;
		}

		if (!bLateChecked)
		{
			bLateChecked = true;
			Test->TestEqual(TEXT("Late readiness cannot change the expired state"),
				RunState(Run), FrozenState);
			Test->TestEqual(TEXT("Late readiness cannot dispatch into the expired run"),
				RunDispatchedEvents(Run), FrozenDispatched);
			Test->TestEqual(TEXT("Retained error code survives late readiness"),
				RunExecutionErrorCode(Run), FrozenErrorCode);
			Test->TestTrue(TEXT("No PIE context survives the preparation failure"),
				!IsPIEWorldPresentForMap(Gate->GetMapPath()));
		}

		if (!bSuccessorChecked)
		{
			// The expired run must have fully finalized (no Finalizing) before a successor.
			if (!IsTerminalRunState(RunState(Run)))
			{
				if (FPlatformTime::Seconds() - ReleaseTime <= ReplayProductDeadlineWatchdogSeconds)
				{
					return false;
				}
				Test->AddError(TEXT("Expired run never reached a stable terminal state"));
				return true;
			}

			bSuccessorChecked = true;
			// A successor run may be admitted after the expired run's owned context is gone.
			const FCortexCommandResult Successor = Service->StartReplay(RecordingId,
				ECortexReplayOrigin::AI);
			Test->TestTrue(TEXT("Successor replay admitted after the preparation timeout"),
				Successor.bSuccess);
			if (Successor.bSuccess)
			{
				SuccessorId = ParseRunId(Successor);
			}
		}

		if (SuccessorId.IsValid())
		{
			const FCortexCommandResult SuccessorRun = Service->GetRun(SuccessorId, true);
			if (SuccessorRun.bSuccess && IsTerminalRunState(RunState(SuccessorRun)))
			{
				return true;
			}
			if (FPlatformTime::Seconds() - ReleaseTime > ReplayProductDeadlineWatchdogSeconds)
			{
				Test->AddError(TEXT("Successor replay did not finalize"));
				return true;
			}
			return false;
		}

		return true;
	}

	FGuid GetSuccessorId() const { return SuccessorId; }
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	int32 RecordingId = 0;
	TSharedRef<FReplayPreparationGate> Gate;
	double Deadline = 0.0;
	double ReleaseTime = 0.0;
	bool bAdmitted = false;
	bool bTimeoutSeen = false;
	bool bLateChecked = false;
	bool bSuccessorChecked = false;
	FString FrozenState;
	FString FrozenErrorCode;
	int32 FrozenDispatched = INDEX_NONE;
	FGuid SuccessorId;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};


}

// ---------------------------------------------------------------------------
// Admission, busy rejection and repeated cancellation of an admitted run.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleAdmissionTest,
	"Cortex.Replay.Lifecycle.StartAdmissionBusyAndRepeatedCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// No prior result for a recording that was never replayed.
	const FCortexCommandResult NoPrior = Service->GetLastRunForRecording(1);
	TestTrue(TEXT("get_last_run_for_recording succeeds without a prior run"), NoPrior.bSuccess);
	TestFalse(TEXT("No prior run summary is null"), NoPrior.Data.IsValid());
	const FCortexCommandResult Idle = Service->GetCurrentOperation();
	TestTrue(TEXT("get_current_operation succeeds while idle"), Idle.bSuccess);
	TestFalse(TEXT("Idle current operation is null"), Idle.Data.IsValid());

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	TestTrue(TEXT("Accepted run has a canonical run id"), RunId.IsValid());
	TestEqual(TEXT("Acceptance reports Preparing"), RunState(Started), FString(TEXT("Preparing")));
	TestTrue(TEXT("Admitted record is in use"), Service->IsRecordInUse(1));

	const FCortexCommandResult Busy = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestFalse(TEXT("Competing start is busy while a run is active"), Busy.bSuccess);
	TestEqual(TEXT("Busy rejection uses EDITOR_BUSY"), Busy.ErrorCode,
		FString(CortexErrorCodes::EditorBusy));

	const FCortexCommandResult Cancelled = Service->CancelReplay(RunId, true);
	TestTrue(TEXT("Admitted run can be cancelled"), Cancelled.bSuccess);
	const FString FirstCancelState = RunState(Cancelled);
	const FCortexCommandResult CancelledAgain = Service->CancelReplay(RunId, true);
	TestTrue(TEXT("Repeated cancellation of the same run is stable"), CancelledAgain.bSuccess);
	TestEqual(TEXT("Repeated cancellation does not change the terminal state"),
		RunState(CancelledAgain), FirstCancelState);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult First = Service->GetRun(RunId, true);
			const FCortexCommandResult Second = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Terminal run is queryable"), First.bSuccess && Second.bSuccess);
			T.TestEqual(TEXT("Terminal state is Cancelled"), RunState(First), FString(TEXT("Cancelled")));
			T.TestEqual(TEXT("Repeated status reads are identical"),
				RunState(Second), RunState(First));
			T.TestEqual(TEXT("Dispatched count is stable across reads"),
				RunDispatchedEvents(Second), RunDispatchedEvents(First));
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained terminal summary exists"), Last.bSuccess && Last.Data.IsValid());
			if (Last.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained summary is the admitted run"),
					Last.Data->GetStringField(TEXT("run_id")),
					RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
				T.TestEqual(TEXT("Retained summary state"), Last.Data->GetStringField(TEXT("state")),
					FString(TEXT("Cancelled")));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("No record remains in use after finalization"),
				Service->IsRecordInUse(1));
			const FCortexCommandResult IdleAfter = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after finalization"), IdleAfter.bSuccess);
			T.TestFalse(TEXT("No active operation remains"), IdleAfter.Data.IsValid());
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// While the owned context exists, finalization is observable and stable.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleFinalizationTest,
	"Cortex.Replay.Lifecycle.FinalizationObservesOwnedTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleFinalizationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	// A replay window wide enough that the cancel command reliably observes Replaying
	// before it cancels (the pre-fix 0.05s recording could complete between frames).
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	// The run must actually reach playback before we cancel, so finalization has an owned
	// context to observe.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Cancel = Service->CancelReplay(RunId, true);
			T.TestTrue(TEXT("Active run cancelled"), Cancel.bSuccess);
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayObserveFinalizing(this, Service, RunId, MapPath, 1, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId, Clock](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Terminal result queryable after teardown"), Run.bSuccess);
			T.TestEqual(TEXT("Cancelled run stays Cancelled"), RunState(Run),
				FString(TEXT("Cancelled")));
			TestHumanClockRestored(T);
			// A successor may now be admitted.
			const FCortexCommandResult Successor = Service->StartReplay(1, ECortexReplayOrigin::AI);
			T.TestTrue(TEXT("Successor may start after teardown"), Successor.bSuccess);
			if (Successor.bSuccess)
			{
				const FGuid SuccessorId = ParseRunId(Successor);
				Service->CancelReplay(SuccessorId, true);
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An owned replay launch must acquire the initial keyboard focus on the newly created route,
// mirroring UEditorEngine::GiveFocusToLastClientPIEViewport (which the owned launch skips while
// the 'Game Gets Mouse Control' play setting is off). A valid foreign off-route keyboard focus
// present at launch must not interrupt the run once the owned route has been focused; the
// post-epoch control then proves a foreign focus acquired AFTER arming still interrupts.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleOwnedLaunchFocusTest,
	"Cortex.Replay.Lifecycle.OwnedLaunchAcquiresRouteFocusBeforeEpoch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleOwnedLaunchFocusTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping owned launch focus test"));
		return true;
	}

	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// Recording 1 completes under the acquired route focus; recording 2 is long enough to observe
	// the armed epoch and then lose it to a foreign focus. Both are Human origin (no AI grant).
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.4, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, false))
	{
		return false;
	}
	if (!PublishReplayRecording(*this, *Fixture, 2, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.5, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, false))
	{
		return false;
	}

	const uint32 User = static_cast<uint32>(FSlateApplication::Get().GetUserIndexForKeyboard());
	const TSharedPtr<SCortexReplayForeignKeyboardSink> Foreign = SNew(SCortexReplayForeignKeyboardSink);
	const TSharedRef<SWindow> HostWindow =
		SNew(SWindow).ClientSize(FVector2D(240.0f, 120.0f))[Foreign.ToSharedRef()];

	// Force the engine's own PIE auto-focus path off so only the product's owned-launch acquisition
	// can put keyboard focus on the route; the saved value is restored at the end of the chain. The
	// owned request leaves EditorPlaySettings unset, so the engine duplicates this CDO into the run.
	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	const bool bSavedGameGetsMouseControl = PlaySettings->GameGetsMouseControl;
	PlaySettings->GameGetsMouseControl = false;

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[User, Foreign, HostWindow](FAutomationTestBase& T)
		{
			auto& Slate = FSlateApplication::Get();
			// The editor window that will host the owned PIE route is the active top-level window
			// before the foreign sink is introduced.
			const TSharedPtr<SWindow> EditorWindow = Slate.GetActiveTopLevelWindow();
			Slate.AddWindow(HostWindow);
			if (EditorWindow.IsValid() && EditorWindow.Get() != &HostWindow.Get())
			{
				// Keep the route's window the active top-level so any interruption is attributed to
				// the foreign keyboard focus, not to a stray window activation by the sink's window.
				EditorWindow->BringToFront();
			}
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			T.TestTrue(TEXT("Foreign off-route sink owns keyboard focus before the owned launch"),
				Slate.GetUserFocusedWidget(User).Get() == Foreign.Get());
		}, Fixture));

	// --- Owned launch: a valid foreign focus is present when the run starts. ---
	const TSharedPtr<FGuid> RunId1 = MakeShared<FGuid>();
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId1](FAutomationTestBase& T)
		{
			const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::Human);
			T.TestTrue(TEXT("Human owned replay admitted with a foreign keyboard focus present"),
				Started.bSuccess);
			if (Started.bSuccess) { *RunId1 = ParseRunId(Started); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitStateThenCheck(this, Service, RunId1,
		TEXT("Replaying"), /*bAIOnly=*/false,
		[User](FAutomationTestBase& T)
		{
			auto& Slate = FSlateApplication::Get();
			const TSharedPtr<SWidget> Route = ResolveOwnedRouteRootWidget();
			T.TestTrue(TEXT("Owned PIE route widget exists once Replaying"), Route.IsValid());
			T.TestTrue(TEXT("Owned launch acquired keyboard focus on the route"),
				Route.IsValid() && IsWidgetOnRouteRoot(Slate.GetUserFocusedWidget(User), Route));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitStateThenCheck(this, Service, RunId1,
		TEXT("Completed"), /*bAIOnly=*/false,
		[Service, RunId1, Foreign](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(*RunId1, false);
			T.TestEqual(TEXT("Every recorded event dispatched under the acquired focus"),
				RunDispatchedEvents(Run), 2);
			T.TestEqual(TEXT("The foreign keyboard sink received no replay key"),
				Foreign->KeyDownCount, 0);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	// --- Post-epoch control: focus lost AFTER arming is still genuine interference. ---
	const TSharedPtr<FGuid> RunId2 = MakeShared<FGuid>();
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId2](FAutomationTestBase& T)
		{
			const FCortexCommandResult Started = Service->StartReplay(2, ECortexReplayOrigin::Human);
			T.TestTrue(TEXT("Second Human owned replay admitted"), Started.bSuccess);
			if (Started.bSuccess) { *RunId2 = ParseRunId(Started); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitStateThenCheck(this, Service, RunId2,
		TEXT("Replaying"), /*bAIOnly=*/false, TFunction<void(FAutomationTestBase&)>(), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[User, Foreign](FAutomationTestBase& T)
		{
			auto& Slate = FSlateApplication::Get();
			Slate.SetUserFocus(User, Foreign, EFocusCause::SetDirectly);
			T.TestTrue(TEXT("Foreign focus established after the epoch"),
				Slate.GetUserFocusedWidget(User).Get() == Foreign.Get());
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitStateThenCheck(this, Service, RunId2,
		TEXT("Interrupted"), /*bAIOnly=*/false,
		[Service, RunId2, Foreign](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(*RunId2, false);
			T.TestTrue(TEXT("Post-epoch foreign focus interrupts before all events are dispatched"),
				RunDispatchedEvents(Run) < 2);
			T.TestEqual(TEXT("Post-epoch foreign focus received no synthetic replay key"),
				Foreign->KeyDownCount, 0);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[HostWindow, PlaySettings, bSavedGameGetsMouseControl](FAutomationTestBase&)
		{
			FSlateApplication::Get().RequestDestroyWindow(HostWindow);
			PlaySettings->GameGetsMouseControl = bSavedGameGetsMouseControl;
		}));

	return true;
}

// ---------------------------------------------------------------------------
// Admission digests are immutable across external library edits and service restart.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleImmutabilityTest,
	"Cortex.Replay.Lifecycle.SnapshotImmutabilityAfterExternalEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleImmutabilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	const FCortexCommandResult Admitted = Service->GetRun(RunId, true);
	TestTrue(TEXT("Admitted run is queryable"), Admitted.bSuccess);
	const FString AdmittedSnapshot = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("recording_snapshot_sha256")) : FString();
	const FString AdmittedInitial = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("initial_state_sha256")) : FString();
	const FString AdmittedInputs = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("inputs_sha256")) : FString();
	TestFalse(TEXT("Admitted snapshot digest is exposed"), AdmittedSnapshot.IsEmpty());
	TestFalse(TEXT("Admitted initial digest is exposed"), AdmittedInitial.IsEmpty());
	TestFalse(TEXT("Admitted inputs digest is exposed"), AdmittedInputs.IsEmpty());

	// The payload is replaced with a different *valid* payload (fewer events) while the run is
	// still active, so the regression proves dispatch uses the admitted snapshot rather than
	// re-reading the library mid-flight.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture](FAutomationTestBase& T)
		{
			const FString InputsPath = Fixture->GetInputsPath(1);
			TArray<FString> Rows;
			T.TestTrue(TEXT("Admitted inputs payload is readable"),
				FCortexReplayTestFixture::ReadPayloadRows(InputsPath, Rows));
			T.TestTrue(TEXT("Admitted payload has a first event row"), Rows.Num() > 1);
			if (Rows.Num() > 1)
			{
				Rows.SetNum(1);
				T.TestTrue(TEXT("Active-run payload replaced with a different valid payload"),
					FCortexReplayTestFixture::WritePayloadRows(InputsPath, Rows));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			// The admitted two-event snapshot must still have driven the whole run.
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Completed run is queryable"), Run.bSuccess && Run.Data.IsValid());
			T.TestEqual(TEXT("Playback completed from the admitted snapshot"),
				RunState(Run), FString(TEXT("Completed")));
			T.TestEqual(TEXT("Every admitted event was dispatched"), RunDispatchedEvents(Run), 2);
			if (Run.Data.IsValid())
			{
				T.TestEqual(TEXT("Admitted event count retained, not the tampered payload"),
					static_cast<int32>(Run.Data->GetNumberField(TEXT("total_events"))), 2);
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture, RunId, AdmittedSnapshot, AdmittedInitial, AdmittedInputs](FAutomationTestBase& T)
		{
			FCortexReplayService Reloaded(Fixture->GetProjectRoot());
			const FCortexCommandResult Retained = Reloaded.GetRun(RunId, true);
			T.TestTrue(TEXT("Retained run reloads after restart"), Retained.bSuccess);
			if (Retained.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained snapshot digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("recording_snapshot_sha256")),
					AdmittedSnapshot);
				T.TestEqual(TEXT("Retained initial digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("initial_state_sha256")), AdmittedInitial);
				T.TestEqual(TEXT("Retained inputs digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("inputs_sha256")), AdmittedInputs);
			}
			const FCortexCommandResult Last = Reloaded.GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained terminal summary reloads"), Last.bSuccess && Last.Data.IsValid());
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// External PIE loss interrupts the run and yields one stable terminal result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecyclePieLossTest,
	"Cortex.Replay.Lifecycle.PieLossInterruptionStableTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecyclePieLossTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	// Target/PIE loss is an interruption, not an error.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId, Clock](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Interrupted run is queryable"), Run.bSuccess);
			T.TestEqual(TEXT("Uncontrolled PIE loss interrupts rather than errors"),
				RunState(Run), FString(TEXT("Interrupted")));
			TestHumanClockRestored(T);
			const FCortexCommandResult Again = Service->CancelReplay(RunId, true);
			T.TestTrue(TEXT("Cancelling an already-terminal run succeeds"), Again.bSuccess);
			T.TestEqual(TEXT("Repeated terminal cancellation preserves the result"),
				RunState(Again), FString(TEXT("Interrupted")));
			T.TestFalse(TEXT("Interrupted run no longer owns the record"),
				Service->IsRecordInUse(1));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Revocation after a prior interruption preserves the first terminal result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRevocationTest,
	"Cortex.Replay.Lifecycle.RevocationAfterInterruptionPreservesTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleRevocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Before = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("First terminal result is Interrupted"), RunState(Before),
				FString(TEXT("Interrupted")));

			// Revoking permission after the run already ended must not overwrite its result.
			const FCortexCommandResult Revoked = Service->SaveMetadata(1, TEXT("Revoked"),
				TEXT("ai disabled"), false);
			T.TestTrue(TEXT("Human metadata save revokes permission"), Revoked.bSuccess);

			const FCortexCommandResult After = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("Revocation preserves the first terminal result"),
				RunState(After), FString(TEXT("Interrupted")));
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained summary still exists"), Last.bSuccess && Last.Data.IsValid());
			if (Last.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained summary remains Interrupted"),
					Last.Data->GetStringField(TEXT("state")), FString(TEXT("Interrupted")));
			}

			const FCortexCommandResult Denied = Service->StartReplay(1, ECortexReplayOrigin::AI);
			T.TestFalse(TEXT("Revoked recording denies a new AI start"), Denied.bSuccess);
			T.TestEqual(TEXT("Denial uses PERMISSION_DENIED"), Denied.ErrorCode,
				FString(CortexReplayErrorCodes::PermissionDenied));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Destroying the backend owner ends the owned run and neutralizes its PIE.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleShutdownTest,
	"Cortex.Replay.Lifecycle.ModuleShutdownNeutralizesOwnedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleShutdownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	// A recording whose replay stays in flight long enough to observe Replaying and hold a key
	// down while the backend owner is shut down.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 1.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase&)
		{
			// Destroying the actual backend owner must invoke shutdown cleanup; dropping a
			// client reference must not.
			Service->Shutdown();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Clock](FAutomationTestBase& T)
		{
			// The reference is still held, so lifetime alone cannot have manufactured teardown.
			T.TestNotNull(TEXT("Backend owner reference still valid"), &Service.Get());
			T.TestTrue(TEXT("No owned PIE remains after backend shutdown"),
				!IsPIEWorldPresentForMap(TEXT("/Game/Maps/TestMap"))
				&& !IsPIEWorldPresentForMap(TEXT("/Game/Maps/TestRoomMap")));
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Module-unload barrier (review P0): an explicit unload or hot reload frees this module's code right
// after ShutdownModule. ShutdownForModuleUnload must return with no ticker, frame delegate, session or
// observer of the service installed, the terminal run already persisted, and the owned PIE still torn
// down afterwards (CortexEditor keeps the ending session alive). Catches the asynchronous shutdown that
// left the ticker and frame hooks registered until a later engine frame.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleModuleUnloadRunTest,
	"Cortex.Replay.Lifecycle.ModuleUnloadCompletesActiveRunSynchronously",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleModuleUnloadRunTest::RunTest(const FString& Parameters)
{
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();
	if (!PublishReplayRecording(*this, *Fixture, 1, MakeReplayMapAssetPath(),
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 1.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("AI replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId, TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId, Fixture](FAutomationTestBase& T)
		{
			Service->ShutdownForModuleUnload();
			T.TestTrue(TEXT("Nothing of the service remains installed when unload returns"), Service->IsQuiescent());
			// The terminal record is durable before the code is freed.
			const TSharedRef<FCortexReplayService> Fresh = MakeService(*Fixture);
			const FCortexCommandResult Retained = Fresh->GetRun(RunId, true);
			T.TestTrue(TEXT("The terminal run is persisted at unload"), Retained.bSuccess);
			T.TestEqual(TEXT("The unloaded run is Cancelled"), RunState(Retained), FString(TEXT("Cancelled")));
			Fresh->Shutdown();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("The owned PIE was torn down after unload"), GEditor->PlayWorld != nullptr);
			TestHumanClockRestored(T);
		}, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Module-unload barrier, capture side: an owned capture in Recording is discarded and nothing of the
// service stays installed when unload returns; the owned PIE still ends afterwards.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleModuleUnloadCaptureTest,
	"Cortex.Replay.Lifecycle.ModuleUnloadCompletesCaptureSynchronously",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleModuleUnloadCaptureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	if (!TestTrue(TEXT("Owned capture admitted"), Service->StartCapture(MakeReplayMapAssetPath()).bSuccess))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			Service->ShutdownForModuleUnload();
			T.TestTrue(TEXT("Nothing of the capture remains installed when unload returns"), Service->IsQuiescent());
			T.TestFalse(TEXT("The discarded capture is not published"), Service->GetRecording(1, false).bSuccess);
			T.TestFalse(TEXT("The discarded capture no longer owns the record"), Service->IsRecordInUse(1));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// The production preparation deadline fails an owned replay with a missing pawn.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTimeoutPossessionTest,
	"Cortex.Replay.Lifecycle.PreparationTimeoutPossessionGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTimeoutPossessionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, true))
	{
		return false;
	}

	// The baseline must be captured before admission, so only the owned PIE world is gated.
	const TSharedRef<FReplayPreparationGate> Gate =
		MakeShared<FReplayPreparationGate>(EReplayPreparationGateMode::Possession, MapPath);
	Gate->CaptureBaselineWorlds();

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayGatePreparationTimeout(this, Service, RunId, 1, Gate, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// The production preparation deadline fails an owned replay with an unregistered viewport.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTimeoutViewportTest,
	"Cortex.Replay.Lifecycle.PreparationTimeoutViewportGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTimeoutViewportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, true))
	{
		return false;
	}

	// The baseline must be captured before admission, so only the owned PIE world is gated.
	const TSharedRef<FReplayPreparationGate> Gate =
		MakeShared<FReplayPreparationGate>(EReplayPreparationGateMode::Viewport, MapPath);
	Gate->CaptureBaselineWorlds();

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayGatePreparationTimeout(this, Service, RunId, 1, Gate, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A failed metadata write preserves the committed permission.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleMetadataWriteFailureTest,
	"Cortex.Replay.Lifecycle.MetadataWriteFailurePreservesPermission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleMetadataWriteFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const FString MapPath = TEXT("/Game/Maps/TestMap");

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, false))
	{
		return false;
	}

	const FString MetadataPath = FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(Fixture->GetProjectRoot(), TEXT(".cortex/replay/recordings")),
			FString::FromInt(1)),
		TEXT("metadata.json"));
	TestTrue(TEXT("Metadata file exists"), IFileManager::Get().FileExists(*MetadataPath));

	FString Before;
	TestTrue(TEXT("Original metadata readable"), FFileHelper::LoadFileToString(Before, *MetadataPath));
	TestTrue(TEXT("Metadata made read-only for the failure case"),
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MetadataPath, true));

	FCortexReplayService Service(Fixture->GetProjectRoot());
	const FCortexCommandResult Denied = Service.SaveMetadata(1, TEXT("Renamed"),
		TEXT("should not commit"), true);
	TestFalse(TEXT("Metadata write failure is rejected"), Denied.bSuccess);

	// Restore write access and verify the committed permission is untouched.
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MetadataPath, false);
	FString After;
	TestTrue(TEXT("Metadata readable after the failed write"),
		FFileHelper::LoadFileToString(After, *MetadataPath));
	TestEqual(TEXT("Failed write left the metadata bytes unchanged"), After, Before);

	FCortexReplayLibrary Library(Fixture->GetProjectRoot());
	TArray<FCortexReplayMetadata> All;
	TestTrue(TEXT("Library list succeeds"), Library.List(false, All).bSuccess);
	const FCortexReplayMetadata* Found = nullptr;
	for (const FCortexReplayMetadata& Metadata : All)
	{
		if (Metadata.RecordingId == 1)
		{
			Found = &Metadata;
			break;
		}
	}
	TestNotNull(TEXT("Recording remains listed"), Found);
	if (Found != nullptr)
	{
		TestFalse(TEXT("Permission stayed revoked/denied after the failed write"),
			Found->bAIEnabled);
		TestEqual(TEXT("Name unchanged after the failed write"),
			Found->Name, FString(TEXT("Recording 1")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// An external revocation during trailing idle must cancel instead of completing.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTrailingRevocationTest,
	"Cortex.Replay.Lifecycle.RevocationDuringTrailingIdleCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTrailingRevocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// A long recorded duration leaves the run idle after its last event, which is exactly where an
	// external revocation must still be observed before completion is claimed.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true,
		1.0 /* duration override: trailing idle */))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 2, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture](FAutomationTestBase& T)
		{
			// An external (out-of-process style) revocation while the run is only trailing idle.
			T.TestTrue(TEXT("Live permission revoked externally"),
				SetMetadataAIEnabled(ReplayMetadataPath(*Fixture, 1), false));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Cancelled"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Revoked run is queryable"), Run.bSuccess);
			T.TestEqual(TEXT("Revocation during trailing idle cancels instead of completing"),
				RunState(Run), FString(TEXT("Cancelled")));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Malformed retained terminal records are never indexed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleMalformedRecordTest,
	"Cortex.Replay.Lifecycle.MalformedRetainedRecordRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleMalformedRecordTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const FString RunsRoot = FPaths::Combine(Fixture->GetProjectRoot(), TEXT("Saved/CortexReplay/Runs"));
	TestTrue(TEXT("Run store directory created"), IFileManager::Get().MakeDirectory(*RunsRoot, true));

	// Control: a consistent terminal record must be indexed.
	const FGuid ValidId = FGuid::NewGuid();
	TestTrue(TEXT("Valid retained record written"),
		SaveJsonObjectFile(ReplayRunRecordPath(*Fixture, ValidId),
			MakeRetainedRunRecordJson(ValidId, 1, TEXT("Cancelled"), 2, 2,
				1, 1, 0, 0)));

	// Malformed: the three UI counts sum far beyond int32, wrapping to the declared pose count in a
	// 32-bit sum (1500000000 * 3 = 4500000000 -> 205032704 in int32). An int64 partition rejects it.
	const FGuid MalformedId = FGuid::NewGuid();
	TestTrue(TEXT("Malformed retained record written"),
		SaveJsonObjectFile(ReplayRunRecordPath(*Fixture, MalformedId),
			MakeRetainedRunRecordJson(MalformedId, 1, TEXT("Cancelled"), 0, MAX_int32,
				205032704, 1500000000, 1500000000, 1500000000)));

	FCortexReplayService Service(Fixture->GetProjectRoot());
	const FCortexCommandResult Valid = Service.GetRun(ValidId, true);
	TestTrue(TEXT("Consistent retained record is indexed"), Valid.bSuccess);
	TestEqual(TEXT("Consistent record state"), RunState(Valid), FString(TEXT("Cancelled")));

	const FCortexCommandResult Malformed = Service.GetRun(MalformedId, true);
	TestFalse(TEXT("Malformed retained record is rejected"), Malformed.bSuccess);
	TestEqual(TEXT("Malformed record reports RUN_NOT_FOUND"), Malformed.ErrorCode,
		FString(CortexReplayErrorCodes::RunNotFound));

	return true;
}

// ---------------------------------------------------------------------------
// The lifetime-owned finalization backend outlives the destroyed service object.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleDestructionTest,
	"Cortex.Replay.Lifecycle.FinalizationSurvivesServiceDestruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleDestructionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	// The holder lets one command drop the last client reference while the run is only Finalizing.
	const TSharedRef<TSharedPtr<FCortexReplayService>> Holder =
		MakeShared<TSharedPtr<FCortexReplayService>>();
	*Holder = MakeShared<FCortexReplayService>(Fixture->GetProjectRoot());
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = (*Holder)->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Holder->ToSharedRef(), RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Holder, RunId](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Cancellation accepted"), (*Holder)->CancelReplay(RunId, true).bSuccess);
			// Destroy the owning service while the owned teardown is still asynchronous.
			Holder->Reset();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRetainedRun(this, Fixture, RunId,
		TEXT("Cancelled"), Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An owned capture whose asynchronous publication fails keeps the failure observable and retries.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleOwnedCapturePublishFailureTest,
	"Cortex.Replay.Lifecycle.OwnedCapturePublicationFailureRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleOwnedCapturePublishFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// Occupy the canonical publication path of the first reserved recording id with a file, so the
	// owned capture's teardown-time publication must fail.
	const FString RecordingsRoot = FPaths::Combine(Fixture->GetProjectRoot(),
		TEXT(".cortex/replay/recordings"));
	TestTrue(TEXT("Recordings root created"),
		IFileManager::Get().MakeDirectory(*RecordingsRoot, true));
	const FString BlockedPath = FPaths::Combine(RecordingsRoot, TEXT("1"));
	TestTrue(TEXT("Publication path blocked"),
		FFileHelper::SaveStringToFile(TEXT("blocked"), *BlockedPath));

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	// The owned capture becomes Recording only after readiness, pose read and neutral-state arming.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service,
		TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			// The stop is accepted and the owned teardown/publication is still pending.
			T.TestTrue(TEXT("Owned capture stop accepted"), Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePublicationFailure(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture, BlockedPath](FAutomationTestBase& T)
		{
			// While retries are pending the retained failure is observable through the status.
			FString RetainedCode;
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Failed owned capture remains queryable"),
				Active.bSuccess && Active.Data.IsValid());
			if (Active.Data.IsValid())
			{
				T.TestEqual(TEXT("Owned capture keeps its reserved recording id"),
					static_cast<int32>(Active.Data->GetNumberField(TEXT("recording_id"))), 1);
				T.TestEqual(TEXT("Owned capture stays Finalizing"),
					Active.Data->GetStringField(TEXT("state")), FString(TEXT("Finalizing")));
				T.TestTrue(TEXT("Publication failure is reported"),
					Active.Data->GetBoolField(TEXT("publication_failed")));
				const TSharedPtr<FJsonObject>* PublicationError = nullptr;
				T.TestTrue(TEXT("Retained publication error exposed"),
					Active.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
					&& PublicationError != nullptr && PublicationError->IsValid());
				if (PublicationError != nullptr && PublicationError->IsValid())
				{
					RetainedCode = (*PublicationError)->GetStringField(TEXT("code"));
					T.TestFalse(TEXT("Publication error carries a code"), RetainedCode.IsEmpty());
				}
			}
			T.TestTrue(TEXT("Failed owned capture still owns the record"), Service->IsRecordInUse(1));

			// A subsequent stop returns the retained asynchronous publication failure instead of
			// the "No capture is active" Finalizing rejection.
			const FCortexCommandResult RetainedStop = Service->StopCapture(false);
			T.TestFalse(TEXT("Subsequent stop reports the retained publication failure"),
				RetainedStop.bSuccess);
			T.TestEqual(TEXT("Retained stop failure carries the published error code"),
				RetainedStop.ErrorCode, RetainedCode);

			// Let the retained publication retry succeed.
			T.TestTrue(TEXT("Publication path unblocked"),
				IFileManager::Get().Delete(*BlockedPath, false, true, true));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			const FCortexCommandResult Idle = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after the retry"), Idle.bSuccess);
			T.TestFalse(TEXT("Owned capture released once its publication succeeded"), Idle.Data.IsValid());
			T.TestFalse(TEXT("Retried capture no longer owns the record"), Service->IsRecordInUse(1));

			// The retried recording is now a normal loadable, listed recording.
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TArray<FCortexReplayMetadata> All;
			T.TestTrue(TEXT("Library lists the retried capture"), Library.List(false, All).bSuccess);
			bool bListed = false;
			for (const FCortexReplayMetadata& Metadata : All)
			{
				if (Metadata.RecordingId == 1) { bListed = true; break; }
			}
			T.TestTrue(TEXT("Retried capture is listed"), bListed);
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Retried capture loads"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Retried capture is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// CR-06: a required pre-press pose that cannot be read at the press is a capture fault. Even
// though the pose recovers before the next service tick, the recording must not be published as
// complete with an invented (initial-pose) guard.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureRequiredPoseFaultTest,
	"Cortex.Replay.Lifecycle.CaptureRequiredPoseFaultWithheld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureRequiredPoseFaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			APlayerController* Controller = PlayWorld->GetFirstPlayerController();
			T.TestNotNull(TEXT("Owned capture controller exists"), Controller);
			if (!Controller) { return; }
			APawn* OriginalPawn = Controller->GetPawn();
			T.TestNotNull(TEXT("Owned capture pawn exists"), OriginalPawn);
			if (!OriginalPawn) { return; }

			ULocalPlayer* LocalPlayer = PlayWorld->GetGameInstance()
				? PlayWorld->GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
			const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
			UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
			FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
			const TSharedPtr<SViewport> ViewportWidget =
				SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
			T.TestTrue(TEXT("Owned capture viewport widget exists"), ViewportWidget.IsValid());
			if (!SlateUser.IsValid() || !ViewportWidget.IsValid()) { return; }
			const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
			const FVector2D ViewportSize = Geometry.GetLocalSize();
			if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0) { return; }
			const FVector2D Center =
				Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.5, ViewportSize.Y * 0.5));
			const FInputDeviceId Device = IPlatformInputDeviceMapper::Get()
				.GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
			const int32 UserIndex = SlateUser->GetUserIndex();

			// The required pre-press pose is unreadable exactly at the press: the selected target's
			// pawn is transiently unbound (a state that does not destroy the target or trigger an
			// engine fault) and restored before the next service tick. A real pointer press at the
			// selected viewport centre is used so capture admission does not depend on editor
			// keyboard focus.
			Controller->UnPossess();

			// Slate routes device input only while the application is active unless inactive-input
			// handling is enabled (the session's dispatch does the same); automation drives input
			// while the editor window may not hold OS focus.
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);

			const FModifierKeysState Modifiers;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr,
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
			Pressed.Reset();
			FSlateApplication::Get().ProcessMouseButtonUpEvent(
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));

			Controller->Possess(OriginalPawn);
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			T.TestTrue(TEXT("Controller re-possessed the original pawn after the press"),
				Controller->GetPawn() == OriginalPawn);
		}, Fixture));

	// Stop is accepted or already auto-finalized; either way the fault withholds publication.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase&)
		{
			Service->StopCapture(false);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			// Later recovery must not legitimize the guard that was never observed: no complete
			// recording is published for the reserved id.
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestFalse(TEXT("Faulted capture is not published as complete"), Recording.bSuccess);
			T.TestFalse(TEXT("Faulted capture no longer owns the record"), Service->IsRecordInUse(1));
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TArray<FCortexReplayMetadata> All;
			T.TestTrue(TEXT("Library listing still succeeds"), Library.List(false, All).bSuccess);
			for (const FCortexReplayMetadata& Metadata : All)
			{
				T.TestFalse(TEXT("Library has no recording for the faulted id"), Metadata.RecordingId == 1);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// CR-07: the persisted immutable provenance version must come from the loaded UnrealCortex
// plugin descriptor, not a second hard-coded release string.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureProvenanceVersionTest,
	"Cortex.Replay.Lifecycle.CaptureProvenanceMatchesPluginDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureProvenanceVersionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
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
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Owned capture stop accepted"), Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const TSharedPtr<IPlugin> Plugin =
				IPluginManager::Get().FindPlugin(FString(TEXT("UnrealCortex")));
			T.TestTrue(TEXT("UnrealCortex plugin descriptor is available"), Plugin.IsValid());
			const FString DescriptorVersion = Plugin.IsValid()
				? Plugin->GetDescriptor().VersionName : FString();
			T.TestFalse(TEXT("Plugin descriptor declares a version"), DescriptorVersion.IsEmpty());

			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Captured recording loads"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Persisted provenance equals the plugin descriptor VersionName"),
					Recording.Data->GetStringField(TEXT("plugin_version")), DescriptorVersion);
			}
		}, Fixture));

	return true;
}

namespace
{
/** Asserts no recording was published for a reserved id and that the record is no longer owned. */
void AssertCaptureNotPublished(FAutomationTestBase& T, FCortexReplayService& Service,
	FCortexReplayTestFixture& Fixture, int32 Id)
{
	T.TestFalse(TEXT("Discarded capture is not loadable"),
		Service.GetRecording(Id, false).bSuccess);
	T.TestFalse(TEXT("Discarded capture no longer owns the record"), Service.IsRecordInUse(Id));
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	TArray<FCortexReplayMetadata> All;
	T.TestTrue(TEXT("Library listing still succeeds"), Library.List(false, All).bSuccess);
	for (const FCortexReplayMetadata& Metadata : All)
	{
		T.TestFalse(TEXT("Library has no recording for the discarded id"), Metadata.RecordingId == Id);
	}
}
}

// ---------------------------------------------------------------------------
// A human Stop before an owned capture reaches Recording is a discard, and it must say so
// explicitly (the previous behaviour silently returned success and saved nothing).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopBeforeRecordingTest,
	"Cortex.Replay.Lifecycle.CaptureStopBeforeRecordingReportsDiscard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopBeforeRecordingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	// Owned readiness is asynchronous, so the capture is still Preparing: the stop discards it.
	const FCortexCommandResult Stopped = Service->StopCapture(false);
	TestFalse(TEXT("Stopping a preparing capture reports a non-publishing outcome"), Stopped.bSuccess);
	TestEqual(TEXT("Discard reuses the module's invalid-operation code"),
		Stopped.ErrorCode, FString(CortexReplayErrorCodes::InvalidOperation));
	TestTrue(TEXT("Discard names the reason it was discarded"),
		Stopped.ErrorMessage.Contains(TEXT("never reached Recording")));
	TestTrue(TEXT("Discard names the Preparing phase"),
		Stopped.ErrorMessage.Contains(TEXT("Preparing")));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A human Stop of a faulted capture reports the fault reason explicitly instead of a silent
// success, and the capture status exposes the same reason while it finalizes.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopFaultedTest,
	"Cortex.Replay.Lifecycle.CaptureStopFaultedReportsReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopFaultedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			APlayerController* Controller = PlayWorld->GetFirstPlayerController();
			T.TestNotNull(TEXT("Owned capture controller exists"), Controller);
			if (!Controller) { return; }
			APawn* OriginalPawn = Controller->GetPawn();
			T.TestNotNull(TEXT("Owned capture pawn exists"), OriginalPawn);
			if (!OriginalPawn) { return; }

			ULocalPlayer* LocalPlayer = PlayWorld->GetGameInstance()
				? PlayWorld->GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
			const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
			UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
			FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
			const TSharedPtr<SViewport> ViewportWidget =
				SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
			T.TestTrue(TEXT("Owned capture viewport widget exists"), ViewportWidget.IsValid());
			if (!SlateUser.IsValid() || !ViewportWidget.IsValid()) { return; }
			const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
			const FVector2D ViewportSize = Geometry.GetLocalSize();
			if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0) { return; }
			const FVector2D Center =
				Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.5, ViewportSize.Y * 0.5));
			const FInputDeviceId Device = IPlatformInputDeviceMapper::Get()
				.GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
			const int32 UserIndex = SlateUser->GetUserIndex();

			// Fault the recording synchronously: the required pre-press pose is unreadable at the
			// press (the pawn is transiently unbound and restored before the next service tick).
			Controller->UnPossess();
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);

			const FModifierKeysState Modifiers;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr,
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
			Pressed.Reset();
			FSlateApplication::Get().ProcessMouseButtonUpEvent(
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));

			Controller->Possess(OriginalPawn);
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);

			// The status exposes the fault reason while the capture is still active.
			FString FaultCode;
			FString FaultMessage;
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			if (Active.bSuccess && Active.Data.IsValid())
			{
				const TSharedPtr<FJsonObject>* FaultError = nullptr;
				if (Active.Data->TryGetObjectField(TEXT("fault_error"), FaultError)
					&& FaultError != nullptr && FaultError->IsValid())
				{
					FaultCode = (*FaultError)->GetStringField(TEXT("code"));
					FaultMessage = (*FaultError)->GetStringField(TEXT("message"));
				}
			}
			T.TestFalse(TEXT("Fault reason is exposed in the capture status"), FaultMessage.IsEmpty());
			T.TestTrue(TEXT("Fault status is flagged"), Active.bSuccess && Active.Data.IsValid()
				&& Active.Data->GetBoolField(TEXT("faulted")));

			// The human stop reports the same reason the status exposes, not a silent success.
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestFalse(TEXT("Stopping a faulted capture reports a non-publishing outcome"),
				Stopped.bSuccess);
			T.TestEqual(TEXT("Faulted stop reports the exposed fault code"),
				Stopped.ErrorCode, FaultCode);
			T.TestEqual(TEXT("Faulted stop reports the exposed fault reason"),
				Stopped.ErrorMessage, FaultMessage);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

namespace
{
/**
 * A foreign second tick of the owned world inside one engine frame: the frame observer invalidates that
 * frame (it no longer matches one recorded world tick). Broadcast twice so the frame is invalid whether
 * or not the real world tick already ran this frame.
 */
void BroadcastForeignOwnedWorldTicks(UWorld& PlayWorld)
{
	FWorldDelegates::OnWorldTickStart.Broadcast(&PlayWorld, LEVELTICK_All, 1.0f / 60.0f);
	FWorldDelegates::OnWorldTickStart.Broadcast(&PlayWorld, LEVELTICK_All, 1.0f / 60.0f);
}
}

// ---------------------------------------------------------------------------
// Audit C5: a captured frame the engine frame observer invalidated would record a frame the target did
// not consume as recorded. The capture faults with REPLAY_TIMING_ERROR and is never published.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureInvalidatedFrameFaultsTest,
	"Cortex.Replay.Lifecycle.CaptureInvalidatedFrameFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureInvalidatedFrameFaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	if (!TestTrue(TEXT("Owned capture admitted"), Service->StartCapture(MakeReplayMapAssetPath()).bSuccess))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor->PlayWorld;
			if (T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld))
			{
				BroadcastForeignOwnedWorldTicks(*PlayWorld);
			}
		}, Fixture));
	// The invalid frame closes at this frame's end; the next frame observes the fault.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this, [](FAutomationTestBase&) {}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestFalse(TEXT("A capture with an invalidated frame does not publish"), Stopped.bSuccess);
			T.TestEqual(TEXT("The invalidated frame is REPLAY_TIMING_ERROR"), Stopped.ErrorCode,
				FString(CortexReplayErrorCodes::ReplayTimingError));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("Nothing was published"), Service->GetRecording(1, false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Audit C5, final frame: the frame still open when the human stops is sealed into the recording only if
// the observer still vouches for it. Invalidating it in the stop frame faults the capture instead of
// publishing a frame the target did not consume as recorded.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureInvalidatedFinalFrameFaultsTest,
	"Cortex.Replay.Lifecycle.CaptureInvalidatedFinalFrameFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureInvalidatedFinalFrameFaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	if (!TestTrue(TEXT("Owned capture admitted"), Service->StartCapture(MakeReplayMapAssetPath()).bSuccess))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	// Let a few valid frames be recorded first.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.2f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor->PlayWorld;
			if (!T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld))
			{
				return;
			}
			BroadcastForeignOwnedWorldTicks(*PlayWorld);
			// The stop seals this same, now invalid, open frame.
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestFalse(TEXT("An invalidated final frame does not publish"), Stopped.bSuccess);
			T.TestEqual(TEXT("The invalidated final frame is REPLAY_TIMING_ERROR"), Stopped.ErrorCode,
				FString(CortexReplayErrorCodes::ReplayTimingError));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("Nothing was published"), Service->GetRecording(1, false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	return true;
}

// ---------------------------------------------------------------------------
// Green control: a normal Stop while the owned capture is Recording still publishes and still
// returns success.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopDuringRecordingTest,
	"Cortex.Replay.Lifecycle.CaptureStopDuringRecordingPublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopDuringRecordingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
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
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Recording stop is accepted and publishes"),
				Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Recording stop published the capture"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Published capture is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
			T.TestFalse(TEXT("Published capture no longer owns the record"), Service->IsRecordInUse(1));

			// Format 3: the recording holds exactly the engine frames that consumed input after the
			// epoch (no warmup/epoch/trailing frames), each on its observed application delta, and
			// the duration is the sum of those deltas.
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TSharedPtr<const FCortexReplaySnapshot> Snapshot;
			T.TestTrue(TEXT("Published capture reloads"),
				Library.Load(1, false, Snapshot).bSuccess && Snapshot.IsValid());
			if (Snapshot.IsValid())
			{
				T.TestEqual(TEXT("The frame count matches the timing block"),
					Snapshot->Frames.Num(), Snapshot->Metadata.Timing.FrameCount);
				double DeltaSum = 0.0;
				int32 AttributedEvents = 0;
				for (int32 Index = 0; Index < Snapshot->Frames.Num(); ++Index)
				{
					const FCortexReplayFrame& Frame = Snapshot->Frames[Index];
					T.TestEqual(TEXT("Frames are indexed from zero"), Frame.FrameIndex, Index);
					T.TestTrue(TEXT("Every captured frame carries an observed application delta"),
						Frame.AppDeltaSeconds > 0.0 && Frame.AppDeltaSeconds <= CortexReplayMaxFrameDeltaSeconds);
					DeltaSum += Frame.AppDeltaSeconds;
					AttributedEvents += Frame.EventCount;
				}
				T.TestEqual(TEXT("The duration is the sum of the frame deltas"),
					Snapshot->Metadata.DurationSeconds, DeltaSum, 1e-9);
				T.TestEqual(TEXT("Every published event is attributed to exactly one frame"),
					AttributedEvents, Snapshot->Events.Num());
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An external PIE/session end during an active Recording capture (the human ended PIE with the
// editor's own stop, not the widget's Stop) is treated as a normal stop and publishes the captured
// stream, which becomes a normal loadable recording.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndPublishesTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndPublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndPublishesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			// A balanced press/release is recorded before the session is ended.
			Route.PressDown();
			Route.PressUp();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);

			// The human ends the session with the editor's own stop instead of the widget's Stop.
			GEditor->RequestEndPlayMap();
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("External session end published the capture"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Published capture is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
			T.TestFalse(TEXT("Published capture no longer owns the record"), Service->IsRecordInUse(1));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit C1: ending the session with the editor's stop-play key (stock Escape) leaves that press
// unreleased in the captured stream. It is the session-ending gesture, not an input to replay, so it
// must be dropped: the capture publishes, and no stop-chord press is in the published events.
// Catches removal of DropTrailingStopChordPress (the capture would be withheld as incomplete).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndStopKeyDroppedTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndStopKeyDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndStopKeyDroppedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			// A balanced click, then the stop-play key whose release never arrives.
			Route.PressDown();
			Route.PressUp();
			Route.PressStopKey();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			GEditor->RequestEndPlayMap();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("The Escape-ended capture published"), Service->GetRecording(1, false).bSuccess);
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TSharedPtr<const FCortexReplaySnapshot> Snapshot;
			if (!T.TestTrue(TEXT("The published capture loads"),
				Library.Load(1, false, Snapshot).bSuccess && Snapshot.IsValid()))
			{
				return;
			}
			for (const FCortexReplayEvent& Event : Snapshot->Events)
			{
				T.TestFalse(TEXT("The stop-play key press is not part of the recording"),
					Event.Input.Key == EKeys::Escape);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A hitch can deliver the stop-play press and its auto-repeat in the same input pump before the deferred
// end runs. The whole hold (press and repeat) is the ending gesture: the capture still publishes.
// Catches a hold start that advances to the repeat, leaving the original press held.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndStopKeyRepeatDroppedTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndStopKeyRepeatDropped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndStopKeyRepeatDroppedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);

	const FCortexCommandResult Started = Service->StartCapture(MakeReplayMapAssetPath());
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			Route.PressStopKey();
			Route.PressStopKey(true);
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			GEditor->RequestEndPlayMap();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("The repeated stop-key capture published"), Service->GetRecording(1, false).bSuccess);
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TSharedPtr<const FCortexReplaySnapshot> Snapshot;
			if (!T.TestTrue(TEXT("The published capture loads"),
				Library.Load(1, false, Snapshot).bSuccess && Snapshot.IsValid()))
			{
				return;
			}
			for (const FCortexReplayEvent& Event : Snapshot->Events)
			{
				T.TestFalse(TEXT("No event of the stop-key hold is recorded"), Event.Input.Key == EKeys::Escape);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A stop-key hold that began frames before the session ended was consumed as gameplay input (the session
// was then ended another way). It is not the ending gesture and must not be erased: the stream is kept
// unbalanced and withheld as INCOMPLETE_RECORDING instead of publishing a stream the runtime never saw.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndEarlierStopKeyHoldWithheldTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndEarlierStopKeyHoldWithheld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndEarlierStopKeyHoldWithheldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);

	const FCortexCommandResult Started = Service->StartCapture(MakeReplayMapAssetPath());
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			Route.PressStopKey();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
		}, Fixture));
	// Later frames are recorded while the key stays held; then the session is ended another way.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			T.TestNotNull(TEXT("The held stop key did not end the owned session"), GEditor->PlayWorld.Get());
			GEditor->RequestEndPlayMap();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
			const FCortexCommandResult Summary = Service->GetLastCaptureResult();
			const TSharedPtr<FJsonObject>* PublicationError = nullptr;
			if (T.TestTrue(TEXT("Terminal summary carries the publication error"),
				Summary.bSuccess && Summary.Data.IsValid()
				&& Summary.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
				&& PublicationError != nullptr && PublicationError->IsValid()))
			{
				T.TestEqual(TEXT("The kept earlier hold reports INCOMPLETE_RECORDING"),
					(*PublicationError)->GetStringField(TEXT("code")),
					FString(CortexReplayErrorCodes::IncompleteRecording));
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-01 (capture side): Slate responsive mode must not throttle an owned capture, and the
// override must be released on every end path. Catches a missing DisableThrottle at capture start or
// a missing balanced release in ResetCapture.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureThrottleTest,
	"Cortex.Replay.Lifecycle.CaptureDisablesSlateThrottle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureThrottleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// A responsive-mode request is what throttles Slate; it is held for the whole test.
	const TSharedRef<FThrottleRequest> Responsive = MakeShared<FThrottleRequest>(
		FSlateThrottleManager::Get().EnterResponsiveMode());
	TestFalse(TEXT("Responsive mode throttles expensive tasks before the capture"),
		FSlateThrottleManager::Get().IsAllowingExpensiveTasks());

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		FSlateThrottleManager::Get().LeaveResponsiveMode(*Responsive);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("The capture disables Slate throttling while it owns the session"),
				FSlateThrottleManager::Get().IsAllowingExpensiveTasks());
			Service->StopCapture(true);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Responsive](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("The throttle override is released when the capture ends"),
				FSlateThrottleManager::Get().IsAllowingExpensiveTasks());
			FSlateThrottleManager::Get().LeaveResponsiveMode(*Responsive);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An external PIE/session end with an unpublishable stream (a button press whose release was never
// recorded) publishes nothing and reports an explicit incomplete outcome to the human surface.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndIncompleteTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndIncompleteWithheld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndIncompleteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
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
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			// Nothing is published as a playable recording and the id is no longer owned.
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);

			// The released immutable failure is never reported as an active operation.
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Releasing the incomplete capture leaves a successful status"),
				Active.bSuccess);
			T.TestFalse(TEXT("The released incomplete capture is not reported as active"),
				Active.Data.IsValid());

			// The explicit terminal outcome is retained for the human window, with the held identity.
			const FCortexCommandResult Summary = Service->GetLastCaptureResult();
			T.TestTrue(TEXT("Terminal capture summary is queryable"),
				Summary.bSuccess && Summary.Data.IsValid());
			if (!Summary.bSuccess || !Summary.Data.IsValid())
			{
				return;
			}
			T.TestFalse(TEXT("Terminal summary is not published"),
				Summary.Data->GetBoolField(TEXT("published")));
			const TSharedPtr<FJsonObject>* PublicationError = nullptr;
			if (T.TestTrue(TEXT("Terminal summary carries the publication error"),
				Summary.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
				&& PublicationError != nullptr && PublicationError->IsValid()))
			{
				T.TestEqual(TEXT("Incomplete stream reports INCOMPLETE_RECORDING"),
					(*PublicationError)->GetStringField(TEXT("code")),
					FString(CortexReplayErrorCodes::IncompleteRecording));
				const TArray<TSharedPtr<FJsonValue>>* HeldInputs = nullptr;
				if (T.TestTrue(TEXT("Incomplete outcome names the held inputs"),
					(*PublicationError)->TryGetArrayField(TEXT("held_inputs"), HeldInputs)
					&& HeldInputs != nullptr))
				{
					bool bNamed = false;
					for (const TSharedPtr<FJsonValue>& Value : *HeldInputs)
					{
						bNamed |= Value.IsValid()
							&& Value->AsString() == FKey(EKeys::LeftMouseButton).GetFName().ToString();
					}
					T.TestTrue(TEXT("held_inputs names the held left mouse button"), bNamed);
				}
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Green control: a bound-target identity loss during Recording (the controller loses the bound pawn
// while its world keeps running) still faults and discards instead of publishing.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTargetIdentityLossFaultsTest,
	"Cortex.Replay.Lifecycle.TargetIdentityLossStillFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTargetIdentityLossFaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld) { return; }
			APlayerController* Controller = PlayWorld->GetFirstPlayerController();
			T.TestNotNull(TEXT("Owned capture controller exists"), Controller);
			if (!Controller) { return; }
			// The world keeps running; only the exact bound pawn identity is lost.
			Controller->UnPossess();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// The external-session-end stream is unbalanced (a real press whose release is never observed), so
// it is unpublishable. The failure is immutable for that frozen stream, so it must not be retried
// forever: the capture has to release its operation lock and admit the next capture, with nothing
// published. This is the live surface the retained-forever publication path missed.
// (Sibling of ExternalSessionEndIncompleteWithheld; never a child of that leaf test.)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleExternalSessionEndIncompleteReleasesTest,
	"Cortex.Replay.Lifecycle.ExternalSessionEndIncompleteReleases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleExternalSessionEndIncompleteReleasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			FCortexReplayOwnedPointerRoute Route;
			if (!Route.Resolve(T, *PlayWorld)) { return; }
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			// A real press on the route whose release is never recorded leaves the stream unbalanced.
			Route.PressDown();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			GEditor->RequestEndPlayMap();
		}, Fixture));

	// The immutable incomplete failure must release the operation instead of retaining Finalizing
	// and republishing once per second forever.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture, MapPath](FAutomationTestBase& T)
		{
			// Nothing of the unpublishable stream reached the library, and the id is no longer owned.
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);

			// The operation lock is released: a fresh capture is admitted instead of EDITOR_BUSY.
			const FCortexCommandResult Restarted = Service->StartCapture(MapPath);
			T.TestTrue(TEXT("A new capture is admitted after the incomplete capture released"),
				Restarted.bSuccess);
			if (!Restarted.bSuccess)
			{
				T.TestNotEqual(TEXT("Release was not blocked by EDITOR_BUSY"), Restarted.ErrorCode,
					FString(CortexReplayErrorCodes::EditorBusy));
				return;
			}
			// Discard the re-observation capture so no owned PIE session is leaked.
			Service->StopCapture(/*bAbnormal=*/true);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

/**
 * Records every PIE world tick from installation on: the real delta the world received and whether
 * the first local player held W when the tick ended (the state that tick consumed).
 */
struct FCortexReplayWorldTickProbe
{
	FDelegateHandle StartHandle;
	FDelegateHandle EndHandle;
	TArray<float> RealDeltas;
	TArray<bool> WDownAtEnd;

	void Install()
	{
		StartHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
			[this](UWorld* World, ELevelTick, float DeltaSeconds)
			{
				if (World != nullptr && World->WorldType == EWorldType::PIE)
				{
					RealDeltas.Add(DeltaSeconds);
				}
			});
		EndHandle = FWorldDelegates::OnWorldTickEnd.AddLambda(
			[this](UWorld* World, ELevelTick, float)
			{
				if (World != nullptr && World->WorldType == EWorldType::PIE)
				{
					const APlayerController* Controller = World->GetFirstPlayerController();
					WDownAtEnd.Add(Controller != nullptr && Controller->IsInputKeyDown(EKeys::W));
				}
			});
	}

	~FCortexReplayWorldTickProbe()
	{
		FWorldDelegates::OnWorldTickStart.Remove(StartHandle);
		FWorldDelegates::OnWorldTickEnd.Remove(EndHandle);
	}
};

/** True when Needle occurs in Haystack as one contiguous, bit-identical run. */
bool ContainsContiguousDeltas(const TArray<float>& Haystack, const TArray<float>& Needle)
{
	for (int32 Start = 0; Start + Needle.Num() <= Haystack.Num(); ++Start)
	{
		if (FMemory::Memcmp(&Haystack[Start], Needle.GetData(), Needle.Num() * sizeof(float)) == 0)
		{
			return true;
		}
	}
	return false;
}

/**
 * Publishes an AI-eligible recording laid out frame by frame, with W pressed in recorded frame
 * DownFrame and released in UpFrame. Event times are the recorded input times of their frames (the
 * application time elapsed since the epoch when the frame's input was processed). TicklessFrame, when
 * set, removes that frame's recorded world tick: the capture saw an engine frame that consumed input
 * while its world did not tick.
 */
FCortexCommandResult PublishFrameStreamRecording(const TSharedRef<FCortexReplayTestFixture>& Fixture,
	const FString& MapPath, const TArray<double>& ReplayedFrameDeltas, int32 DownFrame, int32 UpFrame,
	int32 TicklessFrame = INDEX_NONE)
{
	auto InputTime = [&ReplayedFrameDeltas](int32 Frame)
	{
		double Seconds = 0.0;
		for (int32 Index = 0; Index < Frame; ++Index)
		{
			Seconds += ReplayedFrameDeltas[Index];
		}
		return Seconds;
	};
	FCortexReplaySnapshot Recording = Fixture->MakeRecording(1, true,
		{ MakeKeyPress(0, InputTime(DownFrame), ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, InputTime(UpFrame), ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) });
	Recording.Metadata.MapAssetPath = MapPath;
	Recording.InitialState.PawnClassPath = ReplayPawnClassPath;
	const int32 EventFrames[] = { DownFrame, UpFrame };
	FCortexReplayTestFixture::ApplyFrameStream(Recording, ReplayedFrameDeltas, EventFrames);
	if (TicklessFrame != INDEX_NONE)
	{
		Recording.Frames[TicklessFrame].WorldTick.Reset();
	}
	FCortexReplayLibrary Library(Fixture->GetProjectRoot());
	return Library.Publish(Recording);
}

// ---------------------------------------------------------------------------
// Replay reproduces the RECORDED time step of every frame: the owned world receives exactly the
// recorded real delta, frame after frame, in recorded order - not a dominant rate and not the host's
// own frame time. The human's fixed-step settings are handed back exactly once the run ends.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRecordedFrameDeltasTest,
	"Cortex.Replay.Lifecycle.ReplayReproducesRecordedFrameDeltas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleRecordedFrameDeltasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	TArray<double> Deltas;
	for (int32 Repeat = 0; Repeat < 6; ++Repeat)
	{
		Deltas.Append({ 1.0 / 30.0, 1.0 / 60.0, 1.0 / 45.0, 1.0 / 90.0 });
	}
	if (!TestTrue(TEXT("Variable-delta recording published"),
		PublishFrameStreamRecording(Fixture, MapPath, Deltas, 0, 20).bSuccess))
	{
		return false;
	}
	TArray<float> Expected;
	for (const double Delta : Deltas)
	{
		Expected.Add(static_cast<float>(Delta));
	}

	const TSharedPtr<FCortexReplayWorldTickProbe> Probe = MakeShared<FCortexReplayWorldTickProbe>();
	Probe->Install();
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Variable-delta replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Completed"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Probe, Expected](FAutomationTestBase& T)
		{
			T.TestTrue(FString::Printf(
				TEXT("The owned world ticked with every recorded delta, in order (%d recorded, %d observed)"),
				Expected.Num(), Probe->RealDeltas.Num()),
				ContainsContiguousDeltas(Probe->RealDeltas, Expected));
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Replay is frame-locked: a key held for N recorded frames is down for exactly N world ticks, even
// when the recorded frames vary tenfold in length. Wall-clock dispatch would hold it for however
// many host frames fit into the recorded wall time instead.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleHeldKeySpansRecordedFramesTest,
	"Cortex.Replay.Lifecycle.ReplayHeldKeySpansRecordedFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleHeldKeySpansRecordedFramesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Baseline = MakeDistinctHumanClock();

	// W goes down in replayed frame 0 and up in frame 4: it is held through four recorded world ticks
	// spanning 0.22 s of recorded time, while 0.13 s of recorded wall time separates the two events.
	const TArray<double> Deltas = { 0.1, 0.1, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01 };
	constexpr int32 RecordedHeldTicks = 4;
	if (!TestTrue(TEXT("Mixed-delta recording published"),
		PublishFrameStreamRecording(Fixture, MapPath, Deltas, 0, RecordedHeldTicks).bSuccess))
	{
		return false;
	}

	const TSharedPtr<FCortexReplayWorldTickProbe> Probe = MakeShared<FCortexReplayWorldTickProbe>();
	Probe->Install();
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Mixed-delta replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Completed"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Baseline, Probe, RecordedHeldTicks](FAutomationTestBase& T)
		{
			int32 HeldTicks = 0;
			for (const bool bDown : Probe->WDownAtEnd)
			{
				HeldTicks += bDown ? 1 : 0;
			}
			T.TestEqual(TEXT("W is held for exactly the recorded number of world ticks"),
				HeldTicks, RecordedHeldTicks);
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Replay owns the engine clock only by switching to fixed application stepping, so it must refuse to
// start while the editor already runs on a fixed step: taking it over would silently replace the
// human's own timing, and handing it back could not tell the two owners apart.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRefusesFixedStepEditorTest,
	"Cortex.Replay.Lifecycle.ReplayRefusesWhileEditorFixedSteps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleRefusesFixedStepEditorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const FCortexReplayFixedStepBaseline Baseline;

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	FApp::SetFixedDeltaTime(1.0 / 30.0);
	FApp::SetUseFixedTimeStep(true);
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestFalse(TEXT("A replay is not admitted while the editor fixed-steps"), Started.bSuccess);
	TestEqual(TEXT("The refusal names the timing prerequisite"), Started.ErrorCode,
		FString(TEXT("INCOMPATIBLE_PREREQUISITES")));
	TestTrue(TEXT("The human's fixed step is left untouched"),
		FApp::UseFixedTimeStep() && FApp::GetFixedDeltaTime() == 1.0 / 30.0);
	TestFalse(TEXT("The refused start left no operation behind"), Service->GetCurrentOperation().Data.IsValid());
	return true;
}

/** A console variable a test mutates; the destructor hands the human's value back exactly. */
struct FCortexReplaySabotageGuard
{
	IConsoleVariable* Variable = nullptr;
	FString Value;
	uint32 SetBy = 0;

	~FCortexReplaySabotageGuard()
	{
		if (Variable == nullptr)
		{
			return;
		}
		if ((SetBy & ECVF_SetByMask) == ECVF_SetByConsole)
		{
			Variable->Set(*Value, ECVF_SetByConsole);
		}
		else
		{
			Variable->Unset(ECVF_SetByConsole);
		}
	}
};

/** Captures a console variable for mutation; null when it does not exist. */
TSharedPtr<FCortexReplaySabotageGuard> CaptureSabotageGuard(const TCHAR* Name)
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
	if (Variable == nullptr)
	{
		return nullptr;
	}
	const TSharedPtr<FCortexReplaySabotageGuard> Guard = MakeShared<FCortexReplaySabotageGuard>();
	Guard->Variable = Variable;
	Guard->Value = Variable->GetString();
	Guard->SetBy = Variable->GetFlags() & ECVF_SetByMask;
	return Guard;
}

// ---------------------------------------------------------------------------
// t.OverrideFPS replaces the application delta after the engine's fixed step, without any mode flag
// or custom provider. The owned world then stops receiving the recorded delta, so the next commit has
// to fail with REPLAY_TIMEBASE_LOST instead of reporting a replay that ran on a foreign clock, and
// the run still hands fixed stepping back.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleClockTakeoverForcedDeltaTest,
	"Cortex.Replay.Lifecycle.ClockTakeover.ForcedDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleClockTakeoverForcedDeltaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();
	const TSharedPtr<FCortexReplaySabotageGuard> Override = CaptureSabotageGuard(TEXT("t.OverrideFPS"));
	if (!TestNotNull(TEXT("t.OverrideFPS exists"), Override.Get()))
	{
		return false;
	}

	// Half a second of 60 Hz frames keeps the run owning frames well after the takeover.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.5, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Forced delta replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 1, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Override](FAutomationTestBase& T)
		{
			Override->Variable->Set(10.0f, ECVF_SetByConsole);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Error"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Override, Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("Execution error is REPLAY_TIMEBASE_LOST"), RunExecutionErrorCode(Run),
				FString(TEXT("REPLAY_TIMEBASE_LOST")));
			TestHumanClockRestored(T);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Another owner switching the engine to its own fixed step mid-run (a movie capture, a profiling
// tool) takes application time away from the recording. The run must fail with
// REPLAY_TIMEBASE_LOST rather than silently overwrite that owner's step and complete, and it must
// leave the other owner's clock exactly as that owner set it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleFrameStepReplacedTest,
	"Cortex.Replay.Lifecycle.ReplayFailsWhenFrameStepIsReplaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleFrameStepReplacedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	// Half a second of 60 Hz frames keeps the run owning frames well after the takeover.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.5, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replaced-step replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	constexpr double ForeignFixedDeltaSeconds = 1.0 / 24.0;
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 1, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[ForeignFixedDeltaSeconds](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("The replay owns fixed stepping while it consumes recorded frames"),
				FApp::UseFixedTimeStep());
			FApp::SetFixedDeltaTime(ForeignFixedDeltaSeconds);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Error"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Service, RunId, ForeignFixedDeltaSeconds](FAutomationTestBase& T)
		{
			T.TestEqual(TEXT("Execution error is REPLAY_TIMEBASE_LOST"),
				RunExecutionErrorCode(Service->GetRun(RunId, true)), FString(TEXT("REPLAY_TIMEBASE_LOST")));
			T.TestTrue(TEXT("The other owner's fixed step is left exactly as it set it"),
				FApp::UseFixedTimeStep() && FApp::GetFixedDeltaTime() == ForeignFixedDeltaSeconds);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

namespace
{
/** Owns one Slate responsive-mode throttle request and always gives it back. */
struct FCortexReplayResponsiveModeGuard
{
	FThrottleRequest Request;

	void Enter()
	{
		if (!Request.IsValid())
		{
			Request = FSlateThrottleManager::Get().EnterResponsiveMode();
		}
	}

	void Leave()
	{
		if (Request.IsValid())
		{
			FSlateThrottleManager::Get().LeaveResponsiveMode(Request);
		}
	}

	~FCortexReplayResponsiveModeGuard()
	{
		Leave();
	}
};

/** Holds Slate responsive mode for a number of engine frames, then leaves it. */
class FCortexReplayHoldResponsiveMode : public IAutomationLatentCommand
{
public:
	FCortexReplayHoldResponsiveMode(TSharedRef<FCortexReplayResponsiveModeGuard> InGuard, uint64 InFrames,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Guard(MoveTemp(InGuard)), Frames(InFrames), KeepAlive(MoveTemp(InKeepAlive)) {}
	bool Update() override
	{
		if (!bEntered)
		{
			bEntered = true;
			Guard->Enter();
			StartFrame = GFrameCounter;
			return false;
		}
		if (GFrameCounter - StartFrame >= Frames)
		{
			Guard->Leave();
			return true;
		}
		return false;
	}
private:
	TSharedRef<FCortexReplayResponsiveModeGuard> Guard;
	uint64 Frames;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	bool bEntered = false;
	uint64 StartFrame = 0;
};

/** library.json's next_recording_id, or -1 when the file cannot be read. */
int64 ReadNextRecordingId(const FCortexReplayTestFixture& Fixture)
{
	FString Text;
	const FString Path = FPaths::Combine(Fixture.GetProjectRoot(), TEXT(".cortex/replay/library.json"));
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return -1;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	double Value = 0.0;
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid()
		|| !Object->TryGetNumberField(TEXT("next_recording_id"), Value))
	{
		return -1;
	}
	return static_cast<int64>(Value);
}

bool IsNullJsonField(const FCortexCommandResult& Result, const TCHAR* Field)
{
	if (!Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Value = Result.Data->TryGetField(Field);
	return Value.IsValid() && Value->Type == EJson::Null;
}
}

// ---------------------------------------------------------------------------
// Audit RUN-01: Slate responsive mode (a menu, a drag, a modal) throttles the editor and makes
// UEditorEngine skip PIE world ticks. The replay's frame ownership must survive it: the frame clock
// disables the Slate throttle while it owns the clock, so every recorded frame still gets exactly one
// owned world tick and the run completes instead of failing REPLAY_TIMEBASE_LOST.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleSlateResponsiveModeTest,
	"Cortex.Replay.Lifecycle.ReplaySurvivesSlateResponsiveMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleSlateResponsiveModeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();
	const TSharedRef<FCortexReplayResponsiveModeGuard> Throttle = MakeShared<FCortexReplayResponsiveModeGuard>();

	// About one second of 60 Hz frames: W down at 0, up at 0.9, run ends at 1.0.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.9, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true, 1.0))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 1, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayHoldResponsiveMode(Throttle, 6, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Throttle, Service, RunId](FAutomationTestBase& T)
		{
			Throttle->Leave();
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("A responsive-mode burst does not fail the run"), RunState(Run),
				FString(TEXT("Completed")));
			T.TestEqual(TEXT("No execution error is reported"), RunExecutionErrorCode(Run), FString());
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-09: t.OverrideFPS replaces the application delta after the fixed step, so replay cannot
// own the recorded clock. Frame-clock admission refuses the start synchronously as
// INCOMPATIBLE_PREREQUISITES while the override is active, alongside fixed-step, benchmarking,
// fixed-frame-rate and custom-time-step modes.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRefusesOverriddenFrameRateTest,
	"Cortex.Replay.Lifecycle.ReplayRefusesOverriddenFrameRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleRefusesOverriddenFrameRateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();
	const TSharedPtr<FCortexReplaySabotageGuard> Override = CaptureSabotageGuard(TEXT("t.OverrideFPS"));
	if (!TestNotNull(TEXT("t.OverrideFPS exists"), Override.Get()))
	{
		return false;
	}

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	Override->Variable->Set(30.0f, ECVF_SetByConsole);
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	const bool bPieStarted = HasAnyPIEWorldContext() || (GEditor && GEditor->PlayWorld != nullptr);
	const FCortexCommandResult Current = Service->GetCurrentOperation();
	if (Started.bSuccess)
	{
		// Do not leave an admitted run behind when the refusal is missing.
		Service->CancelReplay(ParseRunId(Started), true);
	}

	TestFalse(TEXT("A replay is not admitted while t.OverrideFPS replaces the frame delta"), Started.bSuccess);
	TestEqual(TEXT("The refusal names the timing prerequisite"), Started.ErrorCode,
		FString(CortexReplayErrorCodes::IncompatiblePrerequisites));
	TestFalse(TEXT("The refused start left no operation behind"), Current.Data.IsValid());
	TestFalse(TEXT("The refused start requested no PIE session"), bPieStarted);
	if (Started.bSuccess)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Audit S3: a terminal run that is not an Error still has a cause. An Interrupted run (PIE lost)
// exposes a non-empty execution_error.code, and a run cancelled by permission revocation exposes
// PERMISSION_DENIED: the run JSON emits execution_error for every non-Completed terminal state that
// retained a cause, not only for Error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleInterruptedRunRetainsCauseTest,
	"Cortex.Replay.Lifecycle.InterruptedRunRetainsCause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleInterruptedRunRetainsCauseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true)
		|| !PublishReplayRecording(*this, *Fixture, 2, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true, 1.0))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("First AI replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid InterruptedId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, InterruptedId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, InterruptedId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, InterruptedId, Fixture, Clock](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(InterruptedId, true);
			T.TestEqual(TEXT("PIE loss interrupts the run"), RunState(Run), FString(TEXT("Interrupted")));
			T.TestNotEqual(TEXT("The interrupted run exposes the cause of the interruption"),
				RunExecutionErrorCode(Run), FString());

			// Second run: revoke the live permission while it replays; the cancellation cause must
			// be retained as PERMISSION_DENIED.
			const FCortexCommandResult Second = Service->StartReplay(2, ECortexReplayOrigin::AI);
			if (!T.TestTrue(TEXT("Second AI replay admitted"), Second.bSuccess))
			{
				return;
			}
			const FGuid CancelledId = ParseRunId(Second);
			FAutomationTestBase* Test = &T;
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(Test, Service, CancelledId,
				TEXT("Replaying"), Fixture));
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(Test, Service, CancelledId, 2, Fixture));
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(Test,
				[Fixture](FAutomationTestBase& Inner)
				{
					Inner.TestTrue(TEXT("Live permission revoked during replay"),
						SetMetadataAIEnabled(ReplayMetadataPath(*Fixture, 2), false));
				}, Fixture));
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(Test, Service, CancelledId,
				TEXT("Cancelled"), Fixture));
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(Test, Fixture));
			ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(Test,
				[Service, CancelledId](FAutomationTestBase& Inner)
				{
					const FCortexCommandResult Cancelled = Service->GetRun(CancelledId, true);
					Inner.TestEqual(TEXT("Revocation cancels the run"), RunState(Cancelled),
						FString(TEXT("Cancelled")));
					Inner.TestEqual(TEXT("The cancelled run exposes the revocation cause"),
						RunExecutionErrorCode(Cancelled), FString(CortexReplayErrorCodes::PermissionDenied));
				}, Fixture));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit C6/S8: refusing a capture because an unrelated PIE is running does not consume a recording
// id and says EDITOR_BUSY. StartCapture checks the editor for unrelated PIE before reserving the id,
// so the refusal leaves the next recording id unchanged.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureRefusedKeepsIdTest,
	"Cortex.Replay.Lifecycle.CaptureRefusedWhilePIERunningKeepsRecordingId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleCaptureRefusedKeepsIdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// Materialise library.json so the counter has a concrete value to compare.
	{
		FCortexReplayLibrary Library(Fixture->GetProjectRoot());
		int32 Seeded = 0;
		if (!TestTrue(TEXT("Counter seeded"), Library.ReserveId(Seeded).bSuccess))
		{
			return false;
		}
	}
	const int64 CounterBefore = ReadNextRecordingId(*Fixture);
	if (!TestTrue(TEXT("The recording counter is readable"), CounterBefore > 0))
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitExternalPiePlaying(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture, MapPath, CounterBefore](FAutomationTestBase& T)
		{
			const FCortexCommandResult Started = Service->StartCapture(MapPath);
			T.TestFalse(TEXT("Capture is refused while an unrelated PIE runs"), Started.bSuccess);
			T.TestEqual(TEXT("The refusal is EDITOR_BUSY"), Started.ErrorCode,
				FString(CortexReplayErrorCodes::EditorBusy));
			T.TestEqual(TEXT("The refused capture consumed no recording id"),
				ReadNextRecordingId(*Fixture), CounterBefore);
			T.TestTrue(TEXT("The unrelated PIE world is untouched"),
				GEditor && GEditor->PlayWorld != nullptr);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-02/S7: cancel_replay reports the same run snapshot get_run reports right after. Once
// cancellation has moved the run to Finalizing, current_frame and waiting are null in both: one live
// predicate (not Finalizing, not terminal) builds the run JSON for every reader, so the two responses
// cannot disagree. This test pins that agreement.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCancelResponseMatchesGetRunTest,
	"Cortex.Replay.Lifecycle.CancelResponseMatchesGetRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleCancelResponseMatchesGetRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.5, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("AI replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 1, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId, Clock](FAutomationTestBase& T)
		{
			const FCortexCommandResult Cancel = Service->CancelReplay(RunId, true);
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Cancel succeeded"), Cancel.bSuccess);
			T.TestTrue(TEXT("get_run succeeded"), Run.bSuccess);
			T.TestEqual(TEXT("Both responses describe the same state"), RunState(Cancel), RunState(Run));
			T.TestTrue(TEXT("cancel_replay reports no current frame once finalizing"),
				IsNullJsonField(Cancel, TEXT("current_frame")));
			T.TestTrue(TEXT("get_run reports no current frame once finalizing"),
				IsNullJsonField(Run, TEXT("current_frame")));
			T.TestTrue(TEXT("cancel_replay reports no waiting wait once finalizing"),
				IsNullJsonField(Cancel, TEXT("waiting")));
			T.TestTrue(TEXT("get_run reports no waiting wait once finalizing"),
				IsNullJsonField(Run, TEXT("waiting")));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

namespace
{
constexpr TCHAR UiWaitRootTag[] = TEXT("CortexReplayWaitRoot");
constexpr TCHAR UiWaitControlTag[] = TEXT("CortexReplayWaitControl");

/** A numeric field of the run's execution_error.details, or Default when absent. */
double RunErrorDetailNumber(const FCortexCommandResult& Run, const TCHAR* Field, double Default)
{
	const TSharedPtr<FJsonObject>* Error = nullptr;
	const TSharedPtr<FJsonObject>* Details = nullptr;
	double Value = Default;
	if (Run.Data.IsValid() && Run.Data->TryGetObjectField(TEXT("execution_error"), Error)
		&& Error != nullptr && Error->IsValid()
		&& (*Error)->TryGetObjectField(TEXT("details"), Details) && Details != nullptr && Details->IsValid())
	{
		(*Details)->TryGetNumberField(Field, Value);
	}
	return Value;
}

/** A numeric top-level field of the run, or Default when absent. */
double RunNumber(const FCortexCommandResult& Run, const TCHAR* Field, double Default)
{
	double Value = Default;
	if (Run.Data.IsValid())
	{
		Run.Data->TryGetNumberField(Field, Value);
	}
	return Value;
}

/**
 * Publishes recording 1 for the UI-wait tests: a pointer press at the viewport's top-left corner whose
 * supported guard names a tagged control that only exists once a test adds it, then its release. Until
 * the control exists the live observation is pointer-pending, so the run blocks on a UI wait.
 */
bool PublishUiWaitRecording(FAutomationTestBase& Test, FCortexReplayTestFixture& Fixture, const FString& MapPath)
{
	const TSharedRef<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::Slate;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Identity->RootTag = UiWaitRootTag;
	Identity->TargetTag = UiWaitControlTag;
	Identity->IdentitySha256 = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(*Identity);

	FCortexReplayTimedEvent Down = MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::PointerDown,
		EKeys::LeftMouseButton);
	Down.Event.Input.ViewportPosition = FVector2D(1.0, 1.0);
	FCortexReplayInteractionGuard Guard;
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UITarget = Identity;
	Guard.ExpectedLocalPosition = FVector2D::ZeroVector;
	Down.Event.Guard = Guard;

	FCortexReplayTimedEvent Up = MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::PointerUp,
		EKeys::LeftMouseButton);
	Up.Event.Input.ViewportPosition = FVector2D(1.0, 1.0);
	return PublishReplayRecording(Test, Fixture, 1, MapPath, {Down, Up}, true);
}

/** Adds the tagged root and control the UI-wait recording names to the owned PIE game viewport. */
bool AddUiWaitWidgetToOwnedViewport(FAutomationTestBase& Test)
{
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE || Context.GameViewport == nullptr)
			{
				continue;
			}
			const TSharedRef<SBorder> Control = SNew(SBorder)
				.Visibility(EVisibility::Visible)
				.BorderBackgroundColor(FLinearColor::Transparent);
			Control->SetTag(FName(UiWaitControlTag));
			const TSharedRef<SBox> Root = SNew(SBox)[Control];
			Root->SetTag(FName(UiWaitRootTag));
			Context.GameViewport->AddViewportWidgetContent(Root);
			return true;
		}
	}
	Test.AddError(TEXT("No owned PIE game viewport to host the UI-wait control"));
	return false;
}

/** Waits until get_run reports a blocked UI wait, then runs one check in that same frame. */
class FCortexReplayAwaitUiWait : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitUiWait(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, TFunction<void(FAutomationTestBase&, const FCortexCommandResult&)> InOnWaiting,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), OnWaiting(MoveTemp(InOnWaiting))
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a UI wait"));
			return true;
		}
		const TSharedPtr<FJsonObject>* Waiting = nullptr;
		if (Run.Data.IsValid() && Run.Data->TryGetObjectField(TEXT("waiting"), Waiting)
			&& Waiting != nullptr && Waiting->IsValid())
		{
			Test->TestEqual(TEXT("The wait is on the pending pointer observation"),
				(*Waiting)->GetStringField(TEXT("reason")), FString(TEXT("pointer_pending")));
			OnWaiting(*Test, Run);
			return true;
		}
		if (IsTerminalRunState(RunState(Run)))
		{
			Test->AddError(FString::Printf(TEXT("The run ended (%s) without ever waiting"), *RunState(Run)));
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("The run never reported a UI wait"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	TFunction<void(FAutomationTestBase&, const FCortexCommandResult&)> OnWaiting;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Stalls every engine frame, so a host slower than the recording falls behind it. */
struct FCortexReplayEndFrameStall
{
	FDelegateHandle Handle;

	void Install(float StallSeconds)
	{
		Handle = FCoreDelegates::OnEndFrame.AddLambda([StallSeconds]()
		{
			FPlatformProcess::SleepNoStats(StallSeconds);
		});
	}

	void Remove()
	{
		if (Handle.IsValid())
		{
			FCoreDelegates::OnEndFrame.Remove(Handle);
			Handle.Reset();
		}
	}

	~FCortexReplayEndFrameStall()
	{
		Remove();
	}
};

/** The lag samples one run reported while it was live. */
struct FCortexReplayBehindSamples
{
	double MaxBehindSeconds = 0.0;
	int32 Samples = 0;
	FString FinalState;
	double FinalBehindSeconds = -1.0;
};

/** Samples get_run.behind_seconds every frame until the run is terminal. */
class FCortexReplaySampleBehind : public IAutomationLatentCommand
{
public:
	FCortexReplaySampleBehind(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, TSharedRef<FCortexReplayBehindSamples> InSamples,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), Samples(MoveTemp(InSamples))
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while sampling lag"));
			return true;
		}
		const double Behind = RunNumber(Run, TEXT("behind_seconds"), -1.0);
		Samples->MaxBehindSeconds = FMath::Max(Samples->MaxBehindSeconds, Behind);
		++Samples->Samples;
		if (IsTerminalRunState(RunState(Run)))
		{
			Samples->FinalState = RunState(Run);
			Samples->FinalBehindSeconds = Behind;
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 4)
		{
			Test->AddError(TEXT("The slow-host run never finalized"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	TSharedRef<FCortexReplayBehindSamples> Samples;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Counts the log lines of one category that contain a fragment. */
class FCortexReplayLogMatchCounter : public FOutputDevice
{
public:
	explicit FCortexReplayLogMatchCounter(FString InFragment)
		: Fragment(MoveTemp(InFragment))
	{
		GLog->AddOutputDevice(this);
	}

	~FCortexReplayLogMatchCounter() override
	{
		GLog->RemoveOutputDevice(this);
	}

	int32 GetCount() const { return Count.GetValue(); }

	void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName& Category) override
	{
		if (Category == FName(TEXT("LogCortexReplay")) && FCString::Strstr(Message, *Fragment) != nullptr)
		{
			Count.Increment();
		}
	}

	bool CanBeUsedOnAnyThread() const override { return true; }

private:
	FString Fragment;
	FThreadSafeCounter Count;
};

/**
 * With the run store unwritable, the finished run keeps retrying its terminal write - once per second,
 * not once per engine frame - holds its record while it does, and publishes as soon as the store
 * is writable again.
 */
class FCortexReplayAwaitPersistenceRetry : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitPersistenceRetry(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, FString InRunsPath, TSharedRef<FCortexReplayLogMatchCounter> InFailures,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), RunsPath(MoveTemp(InRunsPath))
		, Failures(MoveTemp(InFailures)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		if (PhaseStart == 0.0) { PhaseStart = Now; }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		switch (Phase)
		{
		case EPhase::AwaitFirstFailure:
			if (Failures->GetCount() >= 1)
			{
				BaselineFailures = Failures->GetCount();
				Phase = EPhase::Observe;
				PhaseStart = Now;
			}
			else if (Now - PhaseStart > ReplayTeardownWatchdogSeconds * 3)
			{
				Test->AddError(TEXT("The run never attempted its terminal write"));
				return true;
			}
			return false;
		case EPhase::Observe:
			if (Now - PhaseStart < ObserveSeconds)
			{
				return false;
			}
			Test->TestTrue(FString::Printf(
				TEXT("The terminal write retries at 1 Hz, not every engine frame (%d retries in %.1f s)"),
				Failures->GetCount() - BaselineFailures, ObserveSeconds),
				Failures->GetCount() - BaselineFailures <= 5);
			Test->TestEqual(TEXT("The unpublished run keeps its record while the write fails"),
				RunState(Run), FString(TEXT("Finalizing")));
			IFileManager::Get().Delete(*RunsPath, false, true, true);
			IFileManager::Get().MakeDirectory(*RunsPath, true);
			Phase = EPhase::AwaitPublished;
			PhaseStart = Now;
			return false;
		case EPhase::AwaitPublished:
			if (RunState(Run) == TEXT("Cancelled"))
			{
				Test->TestTrue(TEXT("The terminal record is on disk once the store is writable"),
					IFileManager::Get().FileExists(*FPaths::Combine(RunsPath,
						RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower() + TEXT(".json"))));
				return true;
			}
			if (Now - PhaseStart > 5.0)
			{
				Test->AddError(TEXT("The run never published after the store became writable"));
				return true;
			}
			return false;
		}
		return true;
	}
private:
	enum class EPhase : uint8 { AwaitFirstFailure, Observe, AwaitPublished };
	static constexpr double ObserveSeconds = 3.5;

	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	FString RunsPath;
	TSharedRef<FCortexReplayLogMatchCounter> Failures;
	EPhase Phase = EPhase::AwaitFirstFailure;
	double PhaseStart = 0.0;
	int32 BaselineFailures = 0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};
}

// ---------------------------------------------------------------------------
// Audit C9/RUN-01: a recorded frame whose world ticked must tick the owned world exactly once. When the
// owned world stops ticking mid-run, the next frame is not committed and the run fails with a precise
// REPLAY_TIMEBASE_LOST instead of completing on a silently stalled world.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleOwnedWorldStopsTickingTest,
	"Cortex.Replay.Lifecycle.ReplayFailsWhenOwnedWorldStopsTicking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleOwnedWorldStopsTickingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	TArray<double> Deltas;
	Deltas.Init(1.0 / 60.0, 40);
	if (!TestTrue(TEXT("Recording published"),
		PublishFrameStreamRecording(Fixture, MapPath, Deltas, 0, 30).bSuccess))
	{
		return false;
	}
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 1, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			if (T.TestNotNull(TEXT("The owned PIE world exists"), PlayWorld))
			{
				// The world stays alive but is never ticked again, like a throttled or suspended editor.
				PlayWorld->SetShouldTick(false);
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Error"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("The stalled world fails the run with REPLAY_TIMEBASE_LOST"),
				RunExecutionErrorCode(Run), FString(TEXT("REPLAY_TIMEBASE_LOST")));
			T.TestEqual(TEXT("The failure says the recorded tick never happened"),
				RunErrorDetailNumber(Run, TEXT("observed_world_ticks"), -1.0), 0.0);
			T.TestEqual(TEXT("The failure names the expected tick"),
				RunErrorDetailNumber(Run, TEXT("expected_world_ticks"), -1.0), 1.0);
			T.TestTrue(TEXT("The stalled frame was not committed"),
				RunNumber(Run, TEXT("completed_frames"), 0.0) < RunNumber(Run, TEXT("frame_count"), 0.0));
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-01/C9: a recorded frame that consumed input without a world tick cannot be replayed while the
// replay world does tick. The run never ticks the world silently: it fails with REPLAY_TIMEBASE_LOST,
// naming the recorded frame, the expected zero ticks and the one it observed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRecordedFrameWithoutTickTest,
	"Cortex.Replay.Lifecycle.ReplayFailsWhenRecordedFrameHasNoWorldTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleRecordedFrameWithoutTickTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	constexpr int32 TicklessFrame = 3;
	TArray<double> Deltas;
	Deltas.Init(1.0 / 60.0, 40);
	if (!TestTrue(TEXT("Recording with a tickless frame published"),
		PublishFrameStreamRecording(Fixture, MapPath, Deltas, 0, 30, TicklessFrame).bSuccess))
	{
		return false;
	}
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Error"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("A ticking world on a tickless recorded frame is REPLAY_TIMEBASE_LOST"),
				RunExecutionErrorCode(Run), FString(TEXT("REPLAY_TIMEBASE_LOST")));
			T.TestEqual(TEXT("The failure names the recorded frame"),
				RunErrorDetailNumber(Run, TEXT("frame_index"), -1.0), static_cast<double>(TicklessFrame));
			T.TestEqual(TEXT("The recording expected no world tick"),
				RunErrorDetailNumber(Run, TEXT("expected_world_ticks"), -1.0), 0.0);
			T.TestEqual(TEXT("The replay world ticked once"),
				RunErrorDetailNumber(Run, TEXT("observed_world_ticks"), -1.0), 1.0);
			T.TestEqual(TEXT("The frames before it were committed"),
				RunNumber(Run, TEXT("completed_frames"), -1.0), static_cast<double>(TicklessFrame));
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-06: a host slower than the recording does not fail the run; it falls behind. While the run is
// live get_run reports the lag, and the run still completes with every frame verified.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleSlowHostReportsBehindTest,
	"Cortex.Replay.Lifecycle.ReplayOnSlowHostReportsBehindAndCompletes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleSlowHostReportsBehindTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	// Half a second of 60 Hz frames, each followed by a 30 ms host stall.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.5, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}
	const TSharedRef<FCortexReplayEndFrameStall> Stall = MakeShared<FCortexReplayEndFrameStall>();
	Stall->Install(0.03f);
	const TSharedRef<FCortexReplayBehindSamples> Samples = MakeShared<FCortexReplayBehindSamples>();

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		Stall->Remove();
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplaySampleBehind(this, Service, RunId, Samples, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Stall, Samples](FAutomationTestBase& T)
		{
			Stall->Remove();
			T.TestEqual(TEXT("A slow host falls behind instead of failing the run"),
				Samples->FinalState, FString(TEXT("Completed")));
			T.TestTrue(FString::Printf(TEXT("The live run reported its lag (max %.4f s over %d samples)"),
				Samples->MaxBehindSeconds, Samples->Samples), Samples->MaxBehindSeconds > 0.0);
			T.TestTrue(TEXT("The terminal record keeps the last paced frame's lag"),
				Samples->FinalBehindSeconds >= 0.0);
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A live UI wait inside an owned run: the press waits on a tagged control, the control appears, the press
// is dispatched, the frame commits unverified (its wait frames ran on the editor's clock) and the run goes
// on to complete without REPLAY_TIMEBASE_LOST. The wait is reported as authorized wait time.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleUiWaitCommitsTest,
	"Cortex.Replay.Lifecycle.UIWaitCommitsWithoutTimebaseLoss",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleUiWaitCommitsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishUiWaitRecording(*this, *Fixture, MapPath))
	{
		return false;
	}
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitUiWait(this, Service, RunId,
		[](FAutomationTestBase& T, const FCortexCommandResult& Run)
		{
			T.TestEqual(TEXT("Nothing is dispatched while the press waits"), RunDispatchedEvents(Run), 0);
			AddUiWaitWidgetToOwnedViewport(T);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Completed"), Fixture, ReplayReadyWatchdogSeconds * 4));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("A live UI wait does not lose the time base"), RunExecutionErrorCode(Run), FString());
			T.TestEqual(TEXT("Both recorded events were dispatched"), RunDispatchedEvents(Run), 2);
			T.TestTrue(TEXT("The wait is reported as authorized wait time"),
				RunNumber(Run, TEXT("authorized_wait_seconds"), 0.0) > 0.0);
			T.TestEqual(TEXT("Every frame was committed"),
				RunNumber(Run, TEXT("completed_frames"), -1.0), RunNumber(Run, TEXT("frame_count"), -2.0));
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-02/S7: cancelling a run that is blocked on a UI wait reports the same snapshot get_run reports
// next - no stale waiting object, no current frame - and the pending press is never dispatched.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCancelDuringUiWaitTest,
	"Cortex.Replay.Lifecycle.CancelDuringUIWaitReportsNoWaiting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleCancelDuringUiWaitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishUiWaitRecording(*this, *Fixture, MapPath))
	{
		return false;
	}
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitUiWait(this, Service, RunId,
		[Service, RunId](FAutomationTestBase& T, const FCortexCommandResult&)
		{
			const FCortexCommandResult Cancel = Service->CancelReplay(RunId, true);
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Cancel succeeded"), Cancel.bSuccess);
			T.TestEqual(TEXT("Both responses describe the same state"), RunState(Cancel), RunState(Run));
			T.TestTrue(TEXT("cancel_replay reports no waiting object"), IsNullJsonField(Cancel, TEXT("waiting")));
			T.TestTrue(TEXT("get_run reports no waiting object"), IsNullJsonField(Run, TEXT("waiting")));
			T.TestTrue(TEXT("cancel_replay reports no current frame"), IsNullJsonField(Cancel, TEXT("current_frame")));
			T.TestTrue(TEXT("get_run reports no current frame"), IsNullJsonField(Run, TEXT("current_frame")));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("The cancelled run is Cancelled"), RunState(Run), FString(TEXT("Cancelled")));
			T.TestEqual(TEXT("The pending press was never dispatched"), RunDispatchedEvents(Run), 0);
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Audit RUN-07: a terminal record that cannot be written is retried once per second, not once per engine
// frame, and the run keeps its record until the write succeeds.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTerminalPersistenceRetryTest,
	"Cortex.Replay.Lifecycle.TerminalPersistenceRetriesAtOneHertz",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleTerminalPersistenceRetryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();
	const TSharedPtr<FCortexReplayFixedStepBaseline> Clock = MakeDistinctHumanClock();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) }, true))
	{
		return false;
	}

	// A file where the run store's directory belongs: every terminal write fails until it is removed.
	const FString RunsPath = FPaths::Combine(Fixture->GetProjectRoot(), TEXT("Saved/CortexReplay/Runs"));
	IFileManager::Get().DeleteDirectory(*RunsPath, false, true);
	if (!TestTrue(TEXT("The run store is blocked"), FFileHelper::SaveStringToFile(TEXT("blocked"), *RunsPath)))
	{
		return false;
	}
	const TSharedRef<FCortexReplayLogMatchCounter> Failures =
		MakeShared<FCortexReplayLogMatchCounter>(TEXT("terminal persistence failed"));

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	TestTrue(TEXT("The run is cancelled"), Service->CancelReplay(RunId, true).bSuccess);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitPersistenceRetry(this, Service, RunId, RunsPath, Failures, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Clock, Failures](FAutomationTestBase& T)
		{
			TestHumanClockRestored(T);
		}, Fixture));

	return true;
}

namespace
{
/**
 * Publishes one live-target recording altered by Mutate, replays it, and asserts the run ends as Error
 * INCOMPATIBLE_PREREQUISITES naming Field with nothing dispatched.
 */
bool RunPrerequisiteMismatchCase(FAutomationTestBase& Test, const TCHAR* Field,
	TFunctionRef<void(FCortexReplaySnapshot&)> Mutate)
{
	if (!GEditor) { Test.AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	FCortexReplaySnapshot Snapshot = Fixture->MakeRecording(1, true,
		{ MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		  MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W) });
	Snapshot.Metadata.MapAssetPath = MakeReplayMapAssetPath();
	Snapshot.InitialState.PawnClassPath = ReplayPawnClassPath;
	Mutate(Snapshot);
	FCortexReplayLibrary Library(Fixture->GetProjectRoot());
	if (!Test.TestTrue(TEXT("Recording published"), Library.Publish(Snapshot).bSuccess))
	{
		return false;
	}

	// Prerequisites are compared at readiness against the live owned target, so the start is admitted.
	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	if (!Test.TestTrue(TEXT("Replay admitted"), Started.bSuccess))
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	const FString FieldName(Field);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(&Test, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(&Test, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(&Test,
		[Service, RunId, FieldName](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("The mismatch ends the run as Error"), RunState(Run), FString(TEXT("Error")));
			T.TestEqual(TEXT("The mismatch is INCOMPATIBLE_PREREQUISITES"), RunExecutionErrorCode(Run),
				FString(CortexReplayErrorCodes::IncompatiblePrerequisites));
			T.TestEqual(TEXT("The mismatch dispatches nothing"), RunDispatchedEvents(Run), 0);
			const TSharedPtr<FJsonObject>* Error = nullptr;
			const TSharedPtr<FJsonObject>* Details = nullptr;
			if (T.TestTrue(TEXT("The mismatch carries details"),
				Run.Data.IsValid() && Run.Data->TryGetObjectField(TEXT("execution_error"), Error)
				&& Error != nullptr && Error->IsValid()
				&& (*Error)->TryGetObjectField(TEXT("details"), Details) && Details != nullptr && Details->IsValid()))
			{
				T.TestEqual(TEXT("The mismatch names the field"), (*Details)->GetStringField(TEXT("field")), FieldName);
			}
		}, Fixture));
	return true;
}
}

// ---------------------------------------------------------------------------
// Audit PIF-05: a recording captured under another input configuration does not replay: the run ends as
// Error INCOMPATIBLE_PREREQUISITES naming input_config_sha256 before any input is dispatched.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleInputConfigMismatchTest,
	"Cortex.Replay.Lifecycle.ReplayRefusesMismatchedInputConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleInputConfigMismatchTest::RunTest(const FString& Parameters)
{
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	return RunPrerequisiteMismatchCase(*this, TEXT("input_config_sha256"), [](FCortexReplaySnapshot& Snapshot)
	{
		Snapshot.Metadata.Prerequisites.InputConfigSha256 = FString::ChrN(64, TEXT('f'));
	});
}

// ---------------------------------------------------------------------------
// Audit PIF-01: a recording whose viewport began with a different mouse-capture state does not replay
// (mouse look would be dropped or an extra click injected): Error INCOMPATIBLE_PREREQUISITES naming
// viewport_mouse_capture before any input is dispatched.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleViewportCaptureMismatchTest,
	"Cortex.Replay.Lifecycle.ReplayRefusesMismatchedViewportCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayLifecycleViewportCaptureMismatchTest::RunTest(const FString& Parameters)
{
	if (CortexReplayLivePrerequisites::DeferUntilMeasured(*this, [this, Parameters]() { RunTest(Parameters); }))
	{
		return true;
	}
	return RunPrerequisiteMismatchCase(*this, TEXT("viewport_mouse_capture"), [](FCortexReplaySnapshot& Snapshot)
	{
		Snapshot.InitialState.Viewport.bMouseCapture = !Snapshot.InitialState.Viewport.bMouseCapture;
	});
}
