#include "CortexReplayTestUtils.h"

#include "CortexEditorPhysicalInputSession.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr int32 FixtureFormatSchemaVersion = 2;

/**
 * Brings the level editor's active viewport window to the front so the selected owned-PIE route
 * window is the actually-active top-level window. Replay ownership requires the route to be the
 * actually-active route (CR-03); automation may leave a different window active, which is a test
 * environment condition, not a product relaxation.
 */
void EnsureEditorRouteWindowActive()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	const TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport();
	if (!Viewport.IsValid())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedRef<SWidget> Widget = Viewport->AsWidget();
	if (const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Widget))
	{
		Window->BringToFront();
	}
}

bool IsPressEvent(const FCortexReplayEvent& Event)
{
	if (Event.Input.bRepeat)
	{
		return false;
	}

	return Event.Input.Kind == ECortexEditorPhysicalInputKind::KeyDown
		|| Event.Input.Kind == ECortexEditorPhysicalInputKind::PointerDown
		|| Event.Input.Kind == ECortexEditorPhysicalInputKind::DoubleClick;
}

FString MakeRecordingDirectory(const FString& ProjectRoot, int32 Id)
{
	const FString RecordingsRoot = FPaths::Combine(ProjectRoot, TEXT(".cortex/replay/recordings"));
	return FPaths::Combine(RecordingsRoot, FString::FromInt(Id));
}
}

FCortexReplayTestFixture::FCortexReplayTestFixture()
	: ProjectRoot(FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexReplayTests")),
		FGuid::NewGuid().ToString(EGuidFormats::Digits))))
{
	// Deterministic automation: override the physical-key snapshot to neutral so admission never
	// depends on ambient host keyboard input. The real GetAsyncKeyState snapshot remains the
	// production default; the pre-installation regression drives the snapshot through this
	// resolver explicitly.
#if WITH_DEV_AUTOMATION_TESTS
	FCortexEditorPhysicalInputSession::SetPhysicalKeySnapshotResolver(
		[](const FKey&) { return false; });
#endif
	// Replay ownership requires the actually-active route; make the level editor viewport window
	// the active top-level window before any replay starts.
	EnsureEditorRouteWindowActive();
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ProjectRoot, TEXT(".cortex/replay")), true);
}

FCortexReplayTestFixture::~FCortexReplayTestFixture()
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexEditorPhysicalInputSession::ClearPhysicalKeySnapshotResolver();
#endif
	if (!ProjectRoot.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true);
	}
}

const FString& FCortexReplayTestFixture::GetProjectRoot() const
{
	return ProjectRoot;
}

FString FCortexReplayTestFixture::GetInputsPath(int32 Id) const
{
	return FPaths::Combine(MakeRecordingDirectory(ProjectRoot, Id), TEXT("inputs.jsonl"));
}

FString FCortexReplayTestFixture::GetInitialStatePath(int32 Id) const
{
	return FPaths::Combine(MakeRecordingDirectory(ProjectRoot, Id), TEXT("initial_state.json"));
}

FCortexReplaySnapshot FCortexReplayTestFixture::MakeRecording(
	int32 Id,
	bool bAIEnabled,
	const TArray<FCortexReplayEvent>& Events)
{
	FCortexReplaySnapshot Snapshot;

	FCortexReplayInitialState& InitialState = Snapshot.InitialState;
	InitialState.SchemaVersion = FixtureFormatSchemaVersion;
	InitialState.RecordingId = Id;
	InitialState.PawnClassPath = TEXT("/Game/Blueprints/BP_ReplayPawn.BP_ReplayPawn_C");
	InitialState.Pose.PawnTransform = FTransform::Identity;
	InitialState.Pose.ControlRotation = FRotator::ZeroRotator;

	Snapshot.Events = Events;

	double FinalTime = 0.0;
	for (FCortexReplayEvent& Event : Snapshot.Events)
	{
		FinalTime = FMath::Max(FinalTime, Event.TimeSeconds);

		if (IsPressEvent(Event) && !Event.Guard.IsSet())
		{
			FCortexReplayInteractionGuard Guard;
			Guard.ExpectedPose = InitialState.Pose;
			Guard.UICoverage = ECortexEditorUICoverage::NotApplicable;
			Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
			Event.Guard = Guard;
		}
	}

	FCortexReplayMetadata& Metadata = Snapshot.Metadata;
	Metadata.SchemaVersion = FixtureFormatSchemaVersion;
	Metadata.RecordingId = Id;
	Metadata.Name = FString::Printf(TEXT("Recording %d"), Id);
	Metadata.MapAssetPath = TEXT("/Game/Maps/ReplayTestMap");
	Metadata.EngineVersion = TEXT("5.8.0");
	Metadata.PluginVersion = TEXT("0.3.3");
	Metadata.CreatedAtUtc = FDateTime::UtcNow();
	Metadata.DurationSeconds = FinalTime;
	Metadata.bAIEnabled = bAIEnabled;
	Metadata.bComplete = true;
	Metadata.Prerequisites.LocalPlayerIndex = 0;
	Metadata.Prerequisites.ViewportSize = FIntPoint(1920, 1080);
	Metadata.Prerequisites.DpiScale = 1.0;
	Metadata.Prerequisites.PawnClassPath = InitialState.PawnClassPath;

	for (const FCortexReplayEvent& Event : Snapshot.Events)
	{
		if (!IsPressEvent(Event) || !Event.Guard.IsSet())
		{
			continue;
		}

		++Metadata.GuardCoverage.PosePresses;
		switch (Event.Guard->UICoverage)
		{
		case ECortexEditorUICoverage::Supported:
			++Metadata.GuardCoverage.UISupportedPresses;
			break;
		case ECortexEditorUICoverage::Unavailable:
			++Metadata.GuardCoverage.UIUnavailablePresses;
			break;
		case ECortexEditorUICoverage::NotApplicable:
			++Metadata.GuardCoverage.UINotApplicablePresses;
			break;
		default:
			break;
		}
	}

	// Format-2 fixture frames express the recorded input cadence: an empty warmup frame that admits
	// no input (so the input epoch is a valid frame ordinal), one frame per distinct recorded input
	// time, and a trailing empty frame holding the idle tail open to the recorded duration. A frame
	// whose event could not be consumed by its own deadline is a malformed recording, so the fixture
	// never parks input behind an unrelated frame boundary.
	ApplyCadenceFrames(Snapshot);

	return Snapshot;
}

void FCortexReplayTestFixture::ApplyCadenceFrames(FCortexReplaySnapshot& Recording)
{
	Recording.Frames.Reset();

	// Frame 0 admits no input, so the input epoch stays a valid frame ordinal. A zero-length warmup
	// never delays the first input.
	FCortexReplayFrame Warmup;
	Warmup.FrameIndex = 0;
	Warmup.FrameBeginSeconds = 0.0;
	Warmup.InputDeadlineSeconds = 0.0;
	Warmup.FirstSequence = 0;
	Warmup.EventCount = 0;
	Recording.Frames.Add(Warmup);

	// One frame per distinct recorded input time, carrying that time's contiguous event range.
	int32 Index = 0;
	while (Index < Recording.Events.Num())
	{
		const double Time = Recording.Events[Index].TimeSeconds;
		int32 Count = 0;
		while (Index + Count < Recording.Events.Num()
			&& Recording.Events[Index + Count].TimeSeconds == Time)
		{
			++Count;
		}

		FCortexReplayFrame Frame;
		Frame.FrameIndex = Recording.Frames.Num();
		Frame.FrameBeginSeconds = Time;
		Frame.InputDeadlineSeconds = Time;
		Frame.FirstSequence = Index;
		Frame.EventCount = Count;
		Recording.Frames.Add(Frame);
		Index += Count;
	}

	// A trailing frame with no input holds the recorded idle tail open to the recorded duration.
	FCortexReplayFrame Trailing;
	Trailing.FrameIndex = Recording.Frames.Num();
	Trailing.FrameBeginSeconds = Recording.Events.Num() > 0 ? Recording.Events.Last().TimeSeconds : 0.0;
	Trailing.InputDeadlineSeconds = Recording.Metadata.DurationSeconds;
	Trailing.FirstSequence = Recording.Events.Num();
	Trailing.EventCount = 0;
	Recording.Frames.Add(Trailing);

	Recording.Metadata.Timing.FrameCount = Recording.Frames.Num();
	Recording.Metadata.Timing.InputEpochFrame = 1;
}

FCortexCommandResult AdvanceFrame(FCortexReplayScheduler& Scheduler,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch)
{
	// Drain every frame whose whole range is prepared and whose deadline has passed, stopping exactly
	// at the first frame that is not yet due - the same frontier the scalar scheduler used to break on.
	for (;;)
	{
		if (Scheduler.IsComplete())
		{
			return FCortexCommandRouter::Success(nullptr);
		}

		ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
		const FCortexCommandResult Prepared = Scheduler.PrepareFrame(Scheduler.GetCompletedFrameCount(),
			ReadElapsedSeconds, EvaluateGuard, Dispatch, Preparation);
		if (!Prepared.bSuccess)
		{
			return Prepared;
		}
		if (Preparation != ECortexReplayFramePreparation::Ready)
		{
			return Prepared;
		}

		const FCortexCommandResult Committed = Scheduler.CommitFrame(Scheduler.GetCompletedFrameCount());
		if (!Committed.bSuccess)
		{
			return Committed;
		}
	}
}
