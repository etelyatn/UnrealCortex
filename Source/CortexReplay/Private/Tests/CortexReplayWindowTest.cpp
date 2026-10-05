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
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SCortexReplayMetadataDialog.h"
#include "Widgets/SCortexReplayRecordingRow.h"
#include "Widgets/SCortexReplayWindow.h"
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
	Window.CloseMetadataDialog();
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
	Window.CloseMetadataDialog();
	return Dialog;
}

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
		Window->RefreshLibrary();
		Window->SelectRecording(RecordingId);
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

	Window->SelectRecording(1);
	TestTrue(TEXT("Retained terminal result is shown"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::PreviousTerminal);
	TestTrue(TEXT("Retained summary names Completed"),
		Window->GetSelectedPlaybackSummaryText().ToString().Contains(TEXT("Completed")));
	TestFalse(TEXT("Retained summary is not 'Not replayed yet'"),
		Window->GetSelectedPlaybackSummaryText().ToString().Contains(TEXT("Not replayed yet")));

	Window->SelectRecording(2);
	TestTrue(TEXT("Recording without a run shows none"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::None);
	TestEqual(TEXT("Recording without a run says Not replayed yet"),
		Window->GetSelectedPlaybackSummaryText().ToString(), FString(TEXT("Not replayed yet")));

	Window->SelectRecording(1);
	TestTrue(TEXT("Switching back restores the retained result"),
		Window->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::PreviousTerminal);

	// Restart/reopen reads the retained local results again.
	const TSharedRef<FCortexReplayService> RestartedService = MakeWindowService(Fixture);
	const TSharedRef<SCortexReplayWindow> ReopenedWindow =
		SNew(SCortexReplayWindow).Service(RestartedService);
	ReopenedWindow->RefreshLibrary();
	ReopenedWindow->SelectRecording(1);
	TestTrue(TEXT("Reopen reads the retained result"),
		ReopenedWindow->GetSelectedPlaybackSummaryKind()
			== ECortexReplayPlaybackSummary::PreviousTerminal);
	ReopenedWindow->SelectRecording(2);
	TestTrue(TEXT("Reopen still isolates the other recording"),
		ReopenedWindow->GetSelectedPlaybackSummaryKind() == ECortexReplayPlaybackSummary::None);

	return true;
}

// ---------------------------------------------------------------------------
// Desktop/narrow rows keep every required field; targets are chosen explicitly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayWindowCompactAndTargetChoiceTest,
	"Cortex.Replay.Window.CompactRowsAndExplicitTargetSelection",
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

	for (const bool bCompact : { false, true })
	{
		Window->SetCompactLayout(bCompact);
		Window->RefreshLibrary();
		TestEqual(TEXT("Window reports the requested layout"), Window->IsCompactLayout(), bCompact);

		for (const int32 Id : { 1, 2 })
		{
			const TSharedPtr<SCortexReplayRecordingRow> Row = Window->GetRow(Id);
			if (!TestTrue(TEXT("Compact/narrow row exists"), Row.IsValid()))
			{
				continue;
			}
			TestEqual(TEXT("Row follows compact layout"), Row->IsCompact(), bCompact);
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

	// Target choices: no PIE -> owned path; one candidate -> that exact choice; several -> explicit.
	Window->RefreshLibrary();
	Window->SelectRecording(1);
	TestFalse(TEXT("No target is chosen before Record"), Window->HasChosenCaptureTarget());

	const TArray<FCortexReplayCaptureTargetChoice> NoCandidates;
	TestNull(TEXT("Zero candidates leaves the owned saved-map path"),
		Window->PromptForCaptureTarget(NoCandidates).Get());
	TestFalse(TEXT("Zero candidates selects nothing"), Window->HasChosenCaptureTarget());

	TArray<FCortexReplayCaptureTargetChoice> Single;
	FCortexReplayCaptureTargetChoice Only;
	Only.LocalPlayerIndex = 0;
	Only.MapAssetPath = TEXT("/Game/Maps/TestMap");
	Only.ViewportLabel = TEXT("Viewport 0");
	Single.Add(Only);
	TestNull(TEXT("One ready candidate auto-selects without a popup"),
		Window->PromptForCaptureTarget(Single).Get());
	TestTrue(TEXT("One ready candidate is chosen explicitly"), Window->HasChosenCaptureTarget());
	TestEqual(TEXT("Auto-selected candidate keeps its exact map"),
		Window->GetChosenCaptureTarget().MapAssetPath, Single[0].MapAssetPath);

	TArray<FCortexReplayCaptureTargetChoice> Several;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FCortexReplayCaptureTargetChoice Candidate;
		Candidate.LocalPlayerIndex = Index;
		Candidate.MapAssetPath = FString::Printf(TEXT("/Game/Maps/Candidate%d"), Index);
		Candidate.ViewportLabel = FString::Printf(TEXT("Viewport %d"), Index);
		Several.Add(Candidate);
	}

	const TSharedPtr<SCortexReplayTargetChoiceDialog> Choice =
		Window->PromptForCaptureTarget(Several);
	if (!TestTrue(TEXT("Several candidates require the choice popup"), Choice.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Every candidate is displayed"), Choice->GetCandidateCount(), 3);
	TestEqual(TEXT("No candidate defaults to the first"), Choice->GetSelectedIndex(), INDEX_NONE);

	Choice->SelectCandidate(2);
	TestEqual(TEXT("Chosen index is the explicit selection"), Choice->GetSelectedIndex(), 2);
	TestTrue(TEXT("Explicit selection is retained"), Window->HasChosenCaptureTarget());
	TestEqual(TEXT("Explicit selection keeps its exact local player"),
		Window->GetChosenCaptureTarget().LocalPlayerIndex, 2);
	TestEqual(TEXT("Explicit selection keeps its exact map"),
		Window->GetChosenCaptureTarget().MapAssetPath, FString(TEXT("/Game/Maps/Candidate2")));
	TestEqual(TEXT("Explicit selection keeps its exact viewport label"),
		Window->GetChosenCaptureTarget().ViewportLabel, FString(TEXT("Viewport 2")));

	// Cancelling a fresh choice selects none rather than attaching to the first world.
	const TSharedPtr<SCortexReplayTargetChoiceDialog> Cancelled =
		Window->PromptForCaptureTarget(Several);
	if (!TestTrue(TEXT("Choice popup reopens"), Cancelled.IsValid()))
	{
		return false;
	}
	Cancelled->Cancel();
	TestFalse(TEXT("Cancel selects no target"), Window->HasChosenCaptureTarget());

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
	const TSharedRef<SCortexReplayWindow> Window = SNew(SCortexReplayWindow).Service(Service);
	const FString MapPath = WindowMapAssetPath();

	if (!WindowPublishPlaybackRecording(*this, *Fixture, 1, MapPath,
		{WindowMakeKeyEvent(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 WindowMakeKeyEvent(1, 2.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

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
			Window->RefreshLibrary();
			Window->SelectRecording(1);
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
			Window->RefreshLibrary();
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
			Window->RefreshLibrary();
			Window->SelectRecording(1);

			// The second run keeps its single terminal interruption result after revocation.
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained result is queryable"), Last.bSuccess && Last.Data.IsValid());
			const FString FirstTerminal = Last.Data.IsValid()
				? Last.Data->GetStringField(TEXT("state")) : FString();

			SaveMetadataThroughDialog(T, *Window, 1, TEXT("Revoked"), TEXT("ai off"), false);
			Window->RefreshLibrary();

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
