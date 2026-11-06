#include "CortexReplayTestUtils.h"

#include "CortexCommandRouter.h"
#include "CortexReplayFormat.h"
#include "CortexReplayLivePrerequisites.h"
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
	InitialState.SchemaVersion = CortexReplayFormat::RecordingFormatVersion;
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
	Metadata.SchemaVersion = CortexReplayFormat::RecordingFormatVersion;
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
	Metadata.Prerequisites.PlayerInputClass = TEXT("/Script/Engine.PlayerInput");
	Metadata.Prerequisites.InputConfigSha256 = FString::ChrN(64, TEXT('a'));
	if (CortexReplayLivePrerequisites::IsReady())
	{
		const FCortexEditorPhysicalInputTargetInfo& Live = CortexReplayLivePrerequisites::Target();
		Metadata.Prerequisites.ViewportSize = Live.ViewportSize;
		Metadata.Prerequisites.DpiScale = Live.DpiScale;
		Metadata.Prerequisites.PlayerInputClass = Live.PlayerInputClass;
		Metadata.Prerequisites.InputConfigSha256 = Live.InputConfigSha256;
		InitialState.Viewport = CortexReplayLivePrerequisites::Viewport();
	}

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

	// Fixture frames are laid out exactly as a capture lays them out, at 60 Hz, so a frame-locked
	// replay of a fixture spends the recorded duration on real engine frames.
	ApplyCadenceFrames(Snapshot);

	return Snapshot;
}

void FCortexReplayTestFixture::ApplyCadenceFrames(FCortexReplaySnapshot& Recording)
{
	constexpr double FixtureFrameDeltaSeconds = 1.0 / 60.0;
	double Span = FMath::Max(0.0, Recording.Metadata.DurationSeconds);
	for (const FCortexReplayEvent& Event : Recording.Events)
	{
		Span = FMath::Max(Span, Event.TimeSeconds);
	}

	// Frame k consumes the input recorded up to its boundary at k * delta; the stream covers the span
	// and the last event.
	const int32 Frames = FMath::CeilToInt32(Span / FixtureFrameDeltaSeconds - 1e-9) + 1;
	TArray<double> Deltas;
	Deltas.Init(FixtureFrameDeltaSeconds, Frames);
	TArray<int32> EventFrames;
	EventFrames.Reserve(Recording.Events.Num());
	for (const FCortexReplayEvent& Event : Recording.Events)
	{
		EventFrames.Add(FMath::Clamp(FMath::CeilToInt32(Event.TimeSeconds / FixtureFrameDeltaSeconds - 1e-9),
			0, Frames - 1));
	}
	ApplyFrameStream(Recording, Deltas, EventFrames);
}

void FCortexReplayTestFixture::ApplyFrameStream(FCortexReplaySnapshot& Recording,
	TConstArrayView<double> FrameDeltas, TConstArrayView<int32> EventFrames)
{
	checkf(FrameDeltas.Num() > 0, TEXT("A frame stream needs at least one frame"));
	checkf(EventFrames.Num() == Recording.Events.Num(), TEXT("Every event needs a frame"));
	Recording.Frames.Reset();

	int32 NextEvent = 0;
	double Duration = 0.0;
	for (int32 FrameIndex = 0; FrameIndex < FrameDeltas.Num(); ++FrameIndex)
	{
		FCortexReplayFrame Frame;
		Frame.FrameIndex = FrameIndex;
		Frame.AppDeltaSeconds = FrameDeltas[FrameIndex];
		FCortexReplayFrameTick Tick;
		Tick.RealDeltaSeconds = static_cast<float>(FrameDeltas[FrameIndex]);
		Frame.WorldTick = Tick;
		Frame.FirstSequence = NextEvent;
		while (NextEvent < EventFrames.Num() && EventFrames[NextEvent] == FrameIndex)
		{
			++NextEvent;
		}
		Frame.EventCount = NextEvent - Frame.FirstSequence;
		Recording.Frames.Add(Frame);
		Duration += FrameDeltas[FrameIndex];
	}
	checkf(NextEvent == Recording.Events.Num(), TEXT("Event frames must be non-decreasing and in range"));

	Recording.Metadata.DurationSeconds = Duration;
	Recording.Metadata.Timing.FrameCount = Recording.Frames.Num();
}

FCortexCommandResult PrepareAndCommit(FCortexReplayScheduler& Scheduler,
	TFunctionRef<double()> ReadElapsedSeconds,
	TFunctionRef<FCortexReplayGuardDecision(const FCortexReplayEvent&)> EvaluateGuard,
	TFunctionRef<FCortexCommandResult(const FCortexReplayEvent&)> Dispatch)
{
	if (Scheduler.IsComplete())
	{
		return FCortexCommandRouter::Success(nullptr);
	}

	ECortexReplayFramePreparation Preparation = ECortexReplayFramePreparation::Waiting;
	const FCortexCommandResult Prepared = Scheduler.PrepareFrame(Scheduler.GetCompletedFrameCount(),
		ReadElapsedSeconds, EvaluateGuard, Dispatch, Preparation);
	if (!Prepared.bSuccess || Preparation != ECortexReplayFramePreparation::Ready)
	{
		return Prepared;
	}

	return Scheduler.CommitFrame(Scheduler.GetCompletedFrameCount());
}
