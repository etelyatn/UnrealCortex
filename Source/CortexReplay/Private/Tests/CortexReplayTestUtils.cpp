#include "CortexReplayTestUtils.h"

#include "CortexEditorPhysicalInputSession.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
constexpr int32 FixtureFormatSchemaVersion = 1;

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

	return Snapshot;
}
