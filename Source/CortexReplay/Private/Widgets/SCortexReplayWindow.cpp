#include "Widgets/SCortexReplayWindow.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCortexReplayMetadataDialog.h"
#include "Widgets/SCortexReplayRecordingRow.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CortexReplayWindow"

namespace
{
/** Kind, recording and run of a backend operation; its state is deliberately not part of it. */
FString MakeOperationIdentity(const FJsonObject& Operation)
{
	FString RunId;
	Operation.TryGetStringField(TEXT("run_id"), RunId);
	return FString::Printf(TEXT("%s|%d|%s"), *Operation.GetStringField(TEXT("kind")),
		static_cast<int32>(Operation.GetNumberField(TEXT("recording_id"))), *RunId);
}

/** Lag below this is ordinary frame jitter and is not worth the human's attention. */
constexpr double VisibleBehindSeconds = 0.05;

/** " (CODE: message)" when the run retained a terminal cause, otherwise an empty string. */
FString DescribeRunCause(const FJsonObject& Run)
{
	const TSharedPtr<FJsonObject>* Error = nullptr;
	FString Code;
	if (!Run.TryGetObjectField(TEXT("execution_error"), Error) || Error == nullptr || !Error->IsValid()
		|| !(*Error)->TryGetStringField(TEXT("code"), Code) || Code.IsEmpty())
	{
		return FString();
	}
	FString Message;
	(*Error)->TryGetStringField(TEXT("message"), Message);
	return Message.IsEmpty() ? FString::Printf(TEXT(" (%s)"), *Code)
		: FString::Printf(TEXT(" (%s: %s)"), *Code, *Message);
}

/** " Behind schedule by N s." when the run's recorded timeline visibly trails the clock. */
FString DescribeBehind(const FJsonObject& Run)
{
	double Behind = 0.0;
	if (!Run.TryGetNumberField(TEXT("behind_seconds"), Behind) || Behind <= VisibleBehindSeconds)
	{
		return FString();
	}
	return FString::Printf(TEXT(" Behind schedule by %.2f s."), Behind);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// SCortexReplayDeleteDialog
// ---------------------------------------------------------------------------------------------

void SCortexReplayDeleteDialog::Construct(const FArguments& InArgs)
{
	SetCanTick(true);
	RecordingId = InArgs._RecordingId;
	RecordingName = InArgs._RecordingName;
	bDeleteBlocked = InArgs._bDeleteBlocked;
	OnConfirmed = InArgs._OnConfirmed;
	OnDismissed = InArgs._OnDismissed;

	ChildSlot
	[
		SNew(SBorder)
		.Padding(12.0f)
		[
			SNew(SBox)
			.WidthOverride(380.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SAssignNew(TitleText, STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("Delete recording #%d?"), RecordingId)))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SAssignNew(NameText, STextBlock)
					.Text(FText::FromString(RecordingName))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("DeleteWarning",
						"This removes the recording and its AI replay permission. "
						"This cannot be undone."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility(bDeleteBlocked ? EVisibility::Visible : EVisibility::Collapsed)
					.ColorAndOpacity(FLinearColor(0.90f, 0.76f, 0.55f, 1.0f))
					.Text(LOCTEXT("DeleteBlocked",
						"This recording is in use; it becomes deletable once the operation stops."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SAssignNew(CancelButton, SButton)
						.Text(LOCTEXT("CancelLabel", "Cancel"))
						.OnClicked(this, &SCortexReplayDeleteDialog::OnCancelClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(ConfirmButton, SButton)
						.Text(LOCTEXT("DeleteLabel", "Delete"))
						.IsEnabled(!bDeleteBlocked)
						.OnClicked(this, &SCortexReplayDeleteDialog::OnConfirmClicked)
					]
				]
			]
		]
	];
}

int32 SCortexReplayDeleteDialog::GetRecordingId() const
{
	return RecordingId;
}

bool SCortexReplayDeleteDialog::IsDeleteBlocked() const
{
	return bDeleteBlocked;
}

TSharedPtr<SWidget> SCortexReplayDeleteDialog::GetInitialFocusWidget() const
{
	// The confirmation defaults to Cancel.
	return CancelButton;
}

void SCortexReplayDeleteDialog::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Give Cancel the initial keyboard focus once the confirmation is live in a widget path.
	if (!bRequestedInitialFocus && GetVisibility().IsVisible() && CancelButton.IsValid()
		&& FSlateApplication::IsInitialized())
	{
		bRequestedInitialFocus = true;
		FSlateApplication::Get().SetKeyboardFocus(CancelButton, EFocusCause::SetDirectly);
	}
}

FReply SCortexReplayDeleteDialog::OnCancelClicked()
{
	OnDismissed.ExecuteIfBound();
	return FReply::Handled();
}

FReply SCortexReplayDeleteDialog::OnConfirmClicked()
{
	if (!bDeleteBlocked)
	{
		OnConfirmed.ExecuteIfBound(RecordingId);
	}
	return FReply::Handled();
}

bool SCortexReplayDeleteDialog::SupportsKeyboardFocus() const
{
	return true;
}

FReply SCortexReplayDeleteDialog::OnKeyDown(const FGeometry& /*MyGeometry*/, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return OnCancelClicked();
	}
	return FReply::Unhandled();
}

void SCortexReplayWindow::Construct(const FArguments& InArgs)
{
	Service = InArgs._Service;
	SetCanTick(true);

	ChildSlot
	[
		SNew(SOverlay)

		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)

			// Record / Stop and the short current-operation label.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 8.0f, 8.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SAssignNew(RecordButton, SButton)
					.Text(LOCTEXT("RecordLabel", "Record"))
					.OnClicked(this, &SCortexReplayWindow::OnRecordClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SAssignNew(StopButton, SButton)
					.Text(LOCTEXT("StopLabel", "Stop"))
					.OnClicked(this, &SCortexReplayWindow::OnStopClicked)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(OperationLabel, STextBlock)
					.Text(LOCTEXT("ReadyLabel", "Ready"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Column captions.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10.0f, 0.0f, 8.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("HeadRecording", "ID · Recording / Description"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("HeadActions", "Map · Created · AI use · Actions"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(RowContainer, SVerticalBox)
				]
			]

			// Short local last-playback summary for the selected record.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10.0f, 6.0f)
			[
				SAssignNew(PlaybackSummaryText, STextBlock)
				.Text(LOCTEXT("NotReplayed", "Not replayed yet"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		// Popup layer: metadata dialog and delete confirmation render here.
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SAssignNew(PopupLayer, SBox)
			.Visibility(EVisibility::Collapsed)
		]
	];

	RefreshLibrary();
}

void SCortexReplayWindow::RefreshLibrary()
{
	Entries.Reset();
	if (Service.IsValid())
	{
		const FCortexCommandResult Listed = Service->ListHumanRecordingEntries(Entries);
		if (!Listed.bSuccess)
		{
			LastStatusMessage = Listed.ErrorMessage;
		}
	}

	if (FindRecordingIndex(SelectedRecordingId) == INDEX_NONE)
	{
		SelectedRecordingId = 0;
	}

	// The rebuilt rows reflect the current backend state, so a message produced by the action that
	// triggered this refresh must not be cleared as "stale" by the next tick.
	LastOperationSignature = BuildRefreshKey();
	LastOperationIdentity = BuildOperationIdentity();

	RebuildRows();
	UpdateOperationLabel();
	UpdateToolbarEnablement();
	UpdatePlaybackSummary();
}

const TArray<TSharedPtr<SCortexReplayRecordingRow>>& SCortexReplayWindow::GetRows() const
{
	return Rows;
}

TSharedPtr<SCortexReplayRecordingRow> SCortexReplayWindow::GetRow(int32 RecordingId) const
{
	for (const TSharedPtr<SCortexReplayRecordingRow>& Row : Rows)
	{
		if (Row.IsValid() && Row->GetRecordingId() == RecordingId)
		{
			return Row;
		}
	}
	return nullptr;
}

void SCortexReplayWindow::SelectRecording(int32 RecordingId)
{
	SelectedRecordingId = RecordingId;
	RebuildRows();
	UpdatePlaybackSummary();
}

int32 SCortexReplayWindow::GetSelectedRecordingId() const
{
	return SelectedRecordingId;
}

FReply SCortexReplayWindow::OnRecordClicked()
{
	BeginRecord();
	return FReply::Handled();
}

FReply SCortexReplayWindow::OnStopClicked()
{
	StopActiveOperation();
	return FReply::Handled();
}

void SCortexReplayWindow::BeginRecord()
{
	if (!Service.IsValid())
	{
		return;
	}

	LastStatusMessage.Reset();

	// Record ALWAYS starts an owned capture on the saved editor map. An unrelated PIE session is
	// never borrowed or adopted: the owned request is refused as busy and the refusal stays visible.
	if (!GEditor)
	{
		LastStatusMessage = TEXT("No editor world is available to record");
		UpdateOperationLabel();
		return;
	}
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld)
	{
		LastStatusMessage = TEXT("No saved editor map is available to record");
		UpdateOperationLabel();
		return;
	}

	const FCortexCommandResult Started = Service->StartCapture(
		UWorld::RemovePIEPrefix(EditorWorld->GetPackage()->GetName()));
	if (!Started.bSuccess)
	{
		SurfaceOperationError(Started);
		return;
	}
	RefreshLibrary();
}

void SCortexReplayWindow::StopActiveOperation()
{
	if (!Service.IsValid())
	{
		return;
	}

	LastStatusMessage.Reset();

	const FCortexCommandResult Operation = Service->GetCurrentOperation();
	if (Operation.bSuccess && Operation.Data.IsValid())
	{
		const FString Kind = Operation.Data->GetStringField(TEXT("kind"));
		if (Kind == TEXT("capture"))
		{
			// A stop that discards the capture (never reached Recording, or faulted) must tell the
			// human why nothing was saved instead of silently returning to Ready.
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			if (!Stopped.bSuccess)
			{
				SurfaceOperationError(Stopped);
			}
		}
		else if (Kind == TEXT("replay"))
		{
			FGuid RunId;
			FGuid::Parse(Operation.Data->GetStringField(TEXT("run_id")), RunId);
			Service->CancelReplay(RunId, false);
		}
	}
	RefreshLibrary();
}

FText SCortexReplayWindow::GetOperationLabel() const
{
	return BuildOperationLabel();
}

TSharedPtr<SCortexReplayMetadataDialog> SCortexReplayWindow::OpenMetadataDialog(int32 RecordingId)
{
	const int32 Index = FindRecordingIndex(RecordingId);
	if (Index == INDEX_NONE || !Entries[Index].Metadata.IsSet())
	{
		return nullptr;
	}

	MetadataDialog = SNew(SCortexReplayMetadataDialog)
		.Recording(Entries[Index].Metadata.GetValue())
		.OnCommitted_Lambda([this, RecordingId](const FString& Name, const FString& Description,
			bool bAIEnabled, bool bCountdownEnabled)
		{
			HandleMetadataCommitted(RecordingId, Name, Description, bAIEnabled, bCountdownEnabled);
		})
		.OnDismissed(this, &SCortexReplayWindow::HandleMetadataDismissed);

	ShowPopup(MetadataDialog);
	return MetadataDialog;
}

TSharedPtr<SCortexReplayMetadataDialog> SCortexReplayWindow::GetOpenMetadataDialog() const
{
	return MetadataDialog;
}

void SCortexReplayWindow::CloseMetadataDialog()
{
	MetadataDialog.Reset();
	if (!DeleteDialog.IsValid())
	{
		HidePopup();
	}
}

TSharedPtr<SCortexReplayDeleteDialog> SCortexReplayWindow::OpenDeleteConfirmation(int32 RecordingId)
{
	const int32 Index = FindRecordingIndex(RecordingId);
	if (Index == INDEX_NONE)
	{
		return nullptr;
	}

	const bool bBlocked = Service.IsValid() && Service->IsRecordInUse(RecordingId);
	const FString RecordingName = Entries[Index].Metadata.IsSet()
		? Entries[Index].Metadata->Name : FString(TEXT("Unloadable recording"));
	DeleteDialog = SNew(SCortexReplayDeleteDialog)
		.RecordingId(RecordingId)
		.RecordingName(RecordingName)
		.bDeleteBlocked(bBlocked)
		.OnConfirmed(this, &SCortexReplayWindow::HandleDeleteConfirmed)
		.OnDismissed(this, &SCortexReplayWindow::HandleDeleteDismissed);

	ShowPopup(DeleteDialog);
	return DeleteDialog;
}

TSharedPtr<SCortexReplayDeleteDialog> SCortexReplayWindow::GetDeleteConfirmation() const
{
	return DeleteDialog;
}

bool SCortexReplayWindow::CanDeleteRecording(int32 RecordingId) const
{
	return Service.IsValid() && !Service->IsRecordInUse(RecordingId);
}

ECortexReplayPlaybackSummary SCortexReplayWindow::GetSelectedPlaybackSummaryKind() const
{
	if (SelectedRecordingId == 0 || !Service.IsValid())
	{
		return ECortexReplayPlaybackSummary::None;
	}
	if (IsReplayActiveForRecording(SelectedRecordingId))
	{
		return ECortexReplayPlaybackSummary::ActiveRun;
	}

	const FCortexCommandResult Last = Service->GetLastRunForRecording(SelectedRecordingId);
	if (Last.bSuccess && Last.Data.IsValid())
	{
		return ECortexReplayPlaybackSummary::PreviousTerminal;
	}
	return ECortexReplayPlaybackSummary::None;
}

FText SCortexReplayWindow::GetSelectedPlaybackSummaryText() const
{
	return BuildPlaybackSummaryText();
}

void SCortexReplayWindow::SetCompactLayout(bool bCompact)
{
	if (bCompactLayout == bCompact)
	{
		return;
	}
	bCompactLayout = bCompact;
	RebuildRows();
}

bool SCortexReplayWindow::IsCompactLayout() const
{
	return bCompactLayout;
}

void SCortexReplayWindow::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// The real docked width decides the compact row layout for a narrow arrangement.
	UpdateCompactLayoutFromGeometry(AllottedGeometry);

	// A different operation than the one last seen (for example a transport-started run) supersedes
	// the message of the previous human action.
	const FString OperationIdentity = BuildOperationIdentity();
	if (OperationIdentity != LastOperationIdentity)
	{
		LastOperationIdentity = OperationIdentity;
		if (!OperationIdentity.IsEmpty())
		{
			LastStatusMessage.Reset();
		}
	}

	// Backend operation and library-revision transitions refresh the rows and their availability,
	// so a transport-started run disables Play/Delete and a recording published asynchronously
	// (even after the operation went idle) appears without any external refresh call.
	const FString RefreshKey = BuildRefreshKey();
	if (RefreshKey != LastOperationSignature)
	{
		RefreshLibrary();
	}

	TimeSinceOperationRefresh += InDeltaTime;
	if (TimeSinceOperationRefresh < 0.2)
	{
		return;
	}
	TimeSinceOperationRefresh = 0.0;

	UpdateOperationLabel();
	UpdateToolbarEnablement();
	UpdatePlaybackSummary();
}

void SCortexReplayWindow::UpdateCompactLayoutFromGeometry(const FGeometry& AllottedGeometry)
{
	// Below this width the wider compaction from the mockup (smaller map/date/AI columns) wins.
	static constexpr float CompactLayoutWidthThreshold = 700.0f;
	const bool bCompact = AllottedGeometry.GetLocalSize().X > 0.0f
		&& AllottedGeometry.GetLocalSize().X < CompactLayoutWidthThreshold;
	if (bCompact != bCompactLayout)
	{
		bCompactLayout = bCompact;
		RebuildRows();
	}
}

FString SCortexReplayWindow::BuildRefreshKey() const
{
	// The library revision catches recordings published/deleted/metadata-saved by the backend even
	// when the operation has already returned to idle, without re-reading the filesystem.
	FString Key = Service.IsValid()
		? FString::Printf(TEXT("lib:%lld"), Service->GetLibraryRevision()) : FString();

	const FCortexCommandResult Operation = Service.IsValid()
		? Service->GetCurrentOperation() : FCortexCommandResult();
	if (!Operation.bSuccess || !Operation.Data.IsValid())
	{
		return Key;
	}

	Key += TEXT("|");
	Key += Operation.Data->GetStringField(TEXT("state"));
	Key += TEXT("|");
	Key += MakeOperationIdentity(*Operation.Data);
	return Key;
}

FString SCortexReplayWindow::BuildOperationIdentity() const
{
	const FCortexCommandResult Operation = Service.IsValid()
		? Service->GetCurrentOperation() : FCortexCommandResult();
	if (!Operation.bSuccess || !Operation.Data.IsValid())
	{
		return FString();
	}
	return MakeOperationIdentity(*Operation.Data);
}

void SCortexReplayWindow::SurfaceOperationError(const FCortexCommandResult& Result)
{
	LastStatusMessage = Result.ErrorMessage.IsEmpty()
		? FString(TEXT("The replay operation was rejected")) : Result.ErrorMessage;
	UpdateOperationLabel();
}

void SCortexReplayWindow::RebuildRows()
{
	Rows.Reset();
	if (RowContainer.IsValid())
	{
		RowContainer->ClearChildren();
	}

	// Any capture or run owns the target: no recording can start, so every Play is disabled.
	bool bAnyOperation = false;
	if (Service.IsValid())
	{
		const FCortexCommandResult Operation = Service->GetCurrentOperation();
		bAnyOperation = Operation.bSuccess && Operation.Data.IsValid();
	}

	for (const FCortexReplayLibraryEntry& Entry : Entries)
	{
		const bool bUnloadable = !Entry.Metadata.IsSet();
		const bool bInUse = Service.IsValid() && Service->IsRecordInUse(Entry.RecordingId);

		FCortexReplayMetadata RowMetadata;
		RowMetadata.RecordingId = Entry.RecordingId;
		FString LoadErrorText;
		if (bUnloadable)
		{
			LoadErrorText = Entry.LoadError.ErrorMessage.IsEmpty()
				? Entry.LoadError.ErrorCode
				: FString::Printf(TEXT("%s: %s"), *Entry.LoadError.ErrorCode, *Entry.LoadError.ErrorMessage);
			if (LoadErrorText.IsEmpty())
			{
				LoadErrorText = TEXT("The recording could not be loaded");
			}
		}
		else
		{
			RowMetadata = Entry.Metadata.GetValue();
		}

		TSharedPtr<SCortexReplayRecordingRow> Row = SNew(SCortexReplayRecordingRow)
			.Recording(RowMetadata)
			.LoadErrorText(LoadErrorText)
			.bSelected(Entry.RecordingId == SelectedRecordingId)
			.bCompact(bCompactLayout)
			.bPlayEnabled(!bAnyOperation && !bUnloadable)
			.bEditEnabled(!bUnloadable)
			.bDeleteEnabled(!bInUse)
			.OnPlay(this, &SCortexReplayWindow::HandlePlayClicked)
			.OnEdit(this, &SCortexReplayWindow::HandleEditClicked)
			.OnDelete(this, &SCortexReplayWindow::HandleDeleteClicked)
			.OnSelected(this, &SCortexReplayWindow::HandleRowSelected);

		Rows.Add(Row);
		if (RowContainer.IsValid())
		{
			RowContainer->AddSlot().AutoHeight()[ Row.ToSharedRef() ];
		}
	}
}

void SCortexReplayWindow::UpdateOperationLabel()
{
	if (OperationLabel.IsValid())
	{
		OperationLabel->SetText(BuildOperationLabel());
	}
}

void SCortexReplayWindow::UpdatePlaybackSummary()
{
	if (PlaybackSummaryText.IsValid())
	{
		PlaybackSummaryText->SetText(BuildPlaybackSummaryText());
	}
}

void SCortexReplayWindow::UpdateToolbarEnablement()
{
	bool bHasOperation = false;
	if (Service.IsValid())
	{
		const FCortexCommandResult Operation = Service->GetCurrentOperation();
		bHasOperation = Operation.bSuccess && Operation.Data.IsValid();
	}
	if (RecordButton.IsValid())
	{
		RecordButton->SetEnabled(!bHasOperation);
	}
	if (StopButton.IsValid())
	{
		// Stop stays available for every active operation, including a blocked wait.
		StopButton->SetEnabled(bHasOperation);
	}
}

int32 SCortexReplayWindow::FindRecordingIndex(int32 RecordingId) const
{
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].RecordingId == RecordingId)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool SCortexReplayWindow::IsReplayActiveForRecording(int32 RecordingId) const
{
	if (!Service.IsValid())
	{
		return false;
	}
	const FCortexCommandResult Operation = Service->GetCurrentOperation();
	if (!Operation.bSuccess || !Operation.Data.IsValid())
	{
		return false;
	}
	FString Kind;
	if (!Operation.Data->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("replay"))
	{
		return false;
	}
	return static_cast<int32>(Operation.Data->GetNumberField(TEXT("recording_id"))) == RecordingId;
}

/**
 * Human-visible reason for a retained terminal capture failure, or an empty string when the last
 * capture published. An immutable publication failure releases the capture operation, so this is
 * the only surface that keeps "why nothing was saved" visible while the backend is idle.
 */
FString BuildRetainedCaptureReason(const FJsonObject& Outcome)
{
	if (!Outcome.GetBoolField(TEXT("publication_failed")))
	{
		return FString();
	}
	const int32 RecordingId = static_cast<int32>(Outcome.GetNumberField(TEXT("recording_id")));
	FString Label = FString::Printf(TEXT("Recording #%d · save failed"), RecordingId);
	const TSharedPtr<FJsonObject>* PublicationError = nullptr;
	if (Outcome.TryGetObjectField(TEXT("publication_error"), PublicationError)
		&& PublicationError != nullptr && PublicationError->IsValid())
	{
		const FString Message = (*PublicationError)->GetStringField(TEXT("message"));
		if (!Message.IsEmpty())
		{
			Label += FString::Printf(TEXT(": %s"), *Message);
		}
	}
	return Label;
}

FText SCortexReplayWindow::BuildOperationLabel() const
{
	if (!Service.IsValid())
	{
		return LastStatusMessage.IsEmpty() ? LOCTEXT("ReadyLabel", "Ready")
			: FText::FromString(LastStatusMessage);
	}

	const FCortexCommandResult Operation = Service->GetCurrentOperation();
	if (!Operation.bSuccess)
	{
		return FText::FromString(Operation.ErrorMessage);
	}
	if (!Operation.Data.IsValid())
	{
		// No active operation: a retained terminal capture failure (an immutable publication error
		// released the operation) stays visible, then any rejection message from the last human
		// action, then the ready label.
		const FCortexCommandResult LastCapture = Service->GetLastCaptureResult();
		if (LastCapture.bSuccess && LastCapture.Data.IsValid())
		{
			const FString RetainedReason = BuildRetainedCaptureReason(*LastCapture.Data);
			if (!RetainedReason.IsEmpty())
			{
				return FText::FromString(RetainedReason);
			}
		}
		return LastStatusMessage.IsEmpty() ? LOCTEXT("ReadyLabel", "Ready")
			: FText::FromString(LastStatusMessage);
	}

	const FString Kind = Operation.Data->GetStringField(TEXT("kind"));
	const int32 RecordingId = static_cast<int32>(Operation.Data->GetNumberField(TEXT("recording_id")));
	const FString State = Operation.Data->GetStringField(TEXT("state"));

	FString Label;
	if (Kind == TEXT("replay"))
	{
		Label = FString::Printf(TEXT("Replaying #%d · %s"), RecordingId, *State);
	}
	else
	{
		// Every capture is an owned fresh-PIE capture; the origin field stays "human" for provenance.
		Label = FString::Printf(TEXT("Recording #%d · %s · %s"), RecordingId, TEXT("owned"), *State);
		if (Operation.Data->GetBoolField(TEXT("publication_failed")))
		{
			// The human must see why nothing was saved: name the retained publication reason.
			const TSharedPtr<FJsonObject>* PublicationError = nullptr;
			if (Operation.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
				&& PublicationError != nullptr && PublicationError->IsValid())
			{
				const FString PublicationMessage = (*PublicationError)->GetStringField(TEXT("message"));
				Label += PublicationMessage.IsEmpty()
					? TEXT(" · save failed")
					: FString::Printf(TEXT(" · save failed: %s"), *PublicationMessage);
			}
			else
			{
				Label += TEXT(" · save failed");
			}
		}
		if (Operation.Data->GetBoolField(TEXT("faulted")))
		{
			const TSharedPtr<FJsonObject>* FaultError = nullptr;
			if (Operation.Data->TryGetObjectField(TEXT("fault_error"), FaultError)
				&& FaultError != nullptr && FaultError->IsValid())
			{
				const FString FaultMessage = (*FaultError)->GetStringField(TEXT("message"));
				Label += FaultMessage.IsEmpty()
					? TEXT(" · faulted")
					: FString::Printf(TEXT(" · faulted: %s"), *FaultMessage);
			}
			else
			{
				Label += TEXT(" · faulted");
			}
		}
	}

	const TSharedPtr<FJsonObject>* Waiting = nullptr;
	if (Operation.Data->TryGetObjectField(TEXT("waiting"), Waiting) && Waiting != nullptr
		&& Waiting->IsValid())
	{
		Label += FString::Printf(TEXT(" · waiting: %s (%.2fs event / %.2fs run)"),
			*(*Waiting)->GetStringField(TEXT("reason")),
			(*Waiting)->GetNumberField(TEXT("remaining_event_seconds")),
			(*Waiting)->GetNumberField(TEXT("remaining_run_seconds")));
	}
	// A fresh rejection/outcome from the last human action (for example a stop that discarded the
	// capture) stays visible next to the transiently still-active operation.
	if (!LastStatusMessage.IsEmpty())
	{
		Label += FString::Printf(TEXT(" · %s"), *LastStatusMessage);
	}
	return FText::FromString(Label);
}

FText SCortexReplayWindow::BuildPlaybackSummaryText() const
{
	if (SelectedRecordingId == 0 || !Service.IsValid())
	{
		return LOCTEXT("NotReplayed", "Not replayed yet");
	}

	FString RetainedState;
	FString RetainedCause;
	FString RetainedBehind;
	const FCortexCommandResult Last = Service->GetLastRunForRecording(SelectedRecordingId);
	if (Last.bSuccess && Last.Data.IsValid())
	{
		RetainedState = Last.Data->GetStringField(TEXT("state"));
		RetainedCause = DescribeRunCause(*Last.Data);
		RetainedBehind = DescribeBehind(*Last.Data);
	}

	if (IsReplayActiveForRecording(SelectedRecordingId))
	{
		// A previous result is never presented as the verdict of the active run.
		FString Active = TEXT("Playback in progress.");
		const FCortexCommandResult Operation = Service->GetCurrentOperation();
		if (Operation.bSuccess && Operation.Data.IsValid())
		{
			Active += DescribeBehind(*Operation.Data);
		}
		if (!RetainedState.IsEmpty())
		{
			Active += FString::Printf(
				TEXT(" Previous run: %s%s. Gameplay correctness is not evaluated."),
				*RetainedState, *RetainedCause);
		}
		return FText::FromString(Active);
	}

	if (RetainedState.IsEmpty())
	{
		return LOCTEXT("NotReplayed", "Not replayed yet");
	}
	return FText::FromString(FString::Printf(
		TEXT("%s%s.%s Gameplay correctness is not evaluated."),
		*RetainedState, *RetainedCause, *RetainedBehind));
}

void SCortexReplayWindow::ShowPopup(const TSharedPtr<SWidget>& Content)
{
	PopupContent = Content;
	if (PopupLayer.IsValid() && Content.IsValid())
	{
		PopupLayer->SetContent(Content.ToSharedRef());
		PopupLayer->SetVisibility(EVisibility::Visible);
	}
}

void SCortexReplayWindow::HidePopup()
{
	// If the popup held keyboard focus, hand it back to the window on dismissal.
	const bool bPopupHadFocus = PopupContent.IsValid()
		&& PopupContent->HasAnyUserFocusOrFocusedDescendants();
	PopupContent.Reset();
	if (PopupLayer.IsValid())
	{
		PopupLayer->SetContent(SNullWidget::NullWidget);
		PopupLayer->SetVisibility(EVisibility::Collapsed);
	}
	if (bPopupHadFocus && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::SetDirectly);
	}
}


void SCortexReplayWindow::HandleMetadataCommitted(int32 RecordingId, const FString& Name,
	const FString& Description, bool bAIEnabled, bool bCountdownEnabled)
{
	if (!Service.IsValid())
	{
		return;
	}

	LastStatusMessage.Reset();

	const FCortexCommandResult Saved =
		Service->SaveMetadata(RecordingId, Name, Description, bAIEnabled, bCountdownEnabled);
	if (!Saved.bSuccess)
	{
		// Keep the popup and its drafts so the human can correct the invalid value; the native
		// failure is shown instead of silently discarding the edit.
		if (MetadataDialog.IsValid())
		{
			MetadataDialog->SetCommitError(Saved.ErrorMessage);
		}
		return;
	}

	MetadataDialog.Reset();
	HidePopup();
	RefreshLibrary();
}

void SCortexReplayWindow::HandleMetadataDismissed()
{
	MetadataDialog.Reset();
	HidePopup();
}

void SCortexReplayWindow::HandleDeleteConfirmed(int32 RecordingId)
{
	DeleteDialog.Reset();
	HidePopup();
	if (!Service.IsValid())
	{
		return;
	}

	LastStatusMessage.Reset();
	const FCortexCommandResult Deleted = Service->DeleteRecording(RecordingId);
	if (Deleted.bSuccess)
	{
		if (SelectedRecordingId == RecordingId)
		{
			SelectedRecordingId = 0;
		}
	}
	RefreshLibrary();
	if (!Deleted.bSuccess)
	{
		// Surfaced after the refresh, which re-reads the library and would otherwise overwrite it.
		SurfaceOperationError(Deleted);
	}
}

void SCortexReplayWindow::HandleDeleteDismissed()
{
	DeleteDialog.Reset();
	HidePopup();
}

void SCortexReplayWindow::HandlePlayClicked(int32 RecordingId)
{
	if (!Service.IsValid())
	{
		return;
	}

	LastStatusMessage.Reset();
	const FCortexCommandResult Started = Service->StartReplay(RecordingId, ECortexReplayOrigin::Human);
	if (!Started.bSuccess)
	{
		SurfaceOperationError(Started);
		return;
	}
	SelectedRecordingId = RecordingId;
	RefreshLibrary();
}

void SCortexReplayWindow::HandleEditClicked(int32 RecordingId)
{
	OpenMetadataDialog(RecordingId);
}

void SCortexReplayWindow::HandleDeleteClicked(int32 RecordingId)
{
	OpenDeleteConfirmation(RecordingId);
}

void SCortexReplayWindow::HandleRowSelected(int32 RecordingId)
{
	SelectRecording(RecordingId);
}

#undef LOCTEXT_NAMESPACE
