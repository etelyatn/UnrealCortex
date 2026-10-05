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

// ---------------------------------------------------------------------------------------------
// SCortexReplayTargetChoiceDialog
// ---------------------------------------------------------------------------------------------

void SCortexReplayTargetChoiceDialog::Construct(const FArguments& InArgs)
{
	Candidates = InArgs._Candidates;
	OnChosen = InArgs._OnChosen;
	OnDismissed = InArgs._OnDismissed;
	SelectedIndex = INDEX_NONE;

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		List->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(SButton)
			.HAlign(HAlign_Left)
			.ContentPadding(FMargin(8.0f, 4.0f))
			.Text(FText::FromString(GetCandidateLabel(Index)))
			.OnClicked_Lambda([this, Index]()
			{
				SelectCandidate(Index);
				return FReply::Handled();
			})
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.Padding(12.0f)
		[
			SNew(SBox)
			.WidthOverride(420.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ChooseTargetTitle", "Choose the PIE target to record"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					List
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SAssignNew(CancelButton, SButton)
					.Text(LOCTEXT("CancelLabel", "Cancel"))
					.OnClicked(this, &SCortexReplayTargetChoiceDialog::OnCancelClicked)
				]
			]
		]
	];
}

int32 SCortexReplayTargetChoiceDialog::GetCandidateCount() const
{
	return Candidates.Num();
}

FString SCortexReplayTargetChoiceDialog::GetCandidateLabel(int32 Index) const
{
	if (!Candidates.IsValidIndex(Index))
	{
		return FString();
	}
	const FCortexReplayCaptureTargetChoice& Candidate = Candidates[Index];
	return FString::Printf(TEXT("%s · player %d · %s"),
		*FPaths::GetBaseFilename(Candidate.MapAssetPath),
		Candidate.LocalPlayerIndex,
		*Candidate.ViewportLabel);
}

int32 SCortexReplayTargetChoiceDialog::GetSelectedIndex() const
{
	return SelectedIndex;
}

void SCortexReplayTargetChoiceDialog::SelectCandidate(int32 Index)
{
	if (!Candidates.IsValidIndex(Index))
	{
		return;
	}
	SelectedIndex = Index;
	OnChosen.ExecuteIfBound(Index);
}

void SCortexReplayTargetChoiceDialog::Cancel()
{
	SelectedIndex = INDEX_NONE;
	OnDismissed.ExecuteIfBound();
}

FReply SCortexReplayTargetChoiceDialog::OnCancelClicked()
{
	Cancel();
	return FReply::Handled();
}

// ---------------------------------------------------------------------------------------------
// SCortexReplayWindow
// ---------------------------------------------------------------------------------------------

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

		// Popup layer: metadata dialog, delete confirmation and target choice render here.
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
	Recordings.Reset();
	if (Service.IsValid())
	{
		Service->ListHumanRecordings(Recordings);
	}

	const bool bSelectionStillValid = Recordings.ContainsByPredicate(
		[this](const FCortexReplayMetadata& Metadata)
		{
			return Metadata.RecordingId == SelectedRecordingId;
		});
	if (!bSelectionStillValid)
	{
		SelectedRecordingId = 0;
	}

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

	// Genuinely absent PIE routes to an owned saved-map capture; existing-but-unready or
	// ambiguous PIE stays a fatal selection error (never a first-world fallback).
	const bool bHasPie = HasAnyPieWorldContext();
	TArray<FCortexReplayCaptureTargetChoice> Candidates;
	const FCortexCommandResult Enumerated = Service->EnumerateHumanCaptureTargets(Candidates);
	if (Enumerated.bSuccess)
	{
		BeginRecordForCandidates(Candidates);
		return;
	}
	if (bHasPie)
	{
		SurfaceOperationError(Enumerated);
		return;
	}
	BeginRecordForCandidates(TArray<FCortexReplayCaptureTargetChoice>());
}

void SCortexReplayWindow::BeginRecordForCandidates(
	const TArray<FCortexReplayCaptureTargetChoice>& Candidates)
{
	if (!Service.IsValid())
	{
		return;
	}

	if (Candidates.Num() == 0)
	{
		// No PIE: capture with an owned session on the saved editor map.
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
		return;
	}

	bPendingRecordStart = true;
	TSharedPtr<SCortexReplayTargetChoiceDialog> PopupDialog = PromptForCaptureTarget(Candidates);
	if (PopupDialog.IsValid())
	{
		ShowPopup(PopupDialog);
		return;
	}

	// One ready candidate is selected explicitly (PromptForCaptureTarget chose index 0).
	bPendingRecordStart = false;
	StartBorrowedCaptureAtIndex(0);
}

void SCortexReplayWindow::StopActiveOperation()
{
	if (!Service.IsValid())
	{
		return;
	}

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

TSharedPtr<SCortexReplayTargetChoiceDialog> SCortexReplayWindow::PromptForCaptureTarget(
	const TArray<FCortexReplayCaptureTargetChoice>& Candidates)
{
	PendingCandidates = Candidates;
	ChosenTargetIndex = INDEX_NONE;
	TargetChoiceDialog.Reset();

	if (Candidates.Num() == 0)
	{
		return nullptr;
	}
	if (Candidates.Num() == 1)
	{
		// Explicitly select the single displayed candidate; no popup is needed.
		ChosenTargetIndex = 0;
		return nullptr;
	}

	TargetChoiceDialog = SNew(SCortexReplayTargetChoiceDialog)
		.Candidates(Candidates)
		.OnChosen(this, &SCortexReplayWindow::HandleTargetChosen)
		.OnDismissed(this, &SCortexReplayWindow::HandleTargetChoiceCancelled);
	return TargetChoiceDialog;
}

bool SCortexReplayWindow::HasChosenCaptureTarget() const
{
	return ChosenTargetIndex != INDEX_NONE && PendingCandidates.IsValidIndex(ChosenTargetIndex);
}

FCortexReplayCaptureTargetChoice SCortexReplayWindow::GetChosenCaptureTarget() const
{
	if (HasChosenCaptureTarget())
	{
		return PendingCandidates[ChosenTargetIndex];
	}
	return FCortexReplayCaptureTargetChoice();
}

TSharedPtr<SCortexReplayMetadataDialog> SCortexReplayWindow::OpenMetadataDialog(int32 RecordingId)
{
	const int32 Index = FindRecordingIndex(RecordingId);
	if (Index == INDEX_NONE)
	{
		return nullptr;
	}

	MetadataDialog = SNew(SCortexReplayMetadataDialog)
		.Recording(Recordings[Index])
		.OnCommitted_Lambda([this, RecordingId](const FString& Name, const FString& Description,
			bool bAIEnabled)
		{
			HandleMetadataCommitted(RecordingId, Name, Description, bAIEnabled);
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
	if (!DeleteDialog.IsValid() && !TargetChoiceDialog.IsValid())
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
	DeleteDialog = SNew(SCortexReplayDeleteDialog)
		.RecordingId(RecordingId)
		.RecordingName(Recordings[Index].Name)
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

	// Backend operation and library-revision transitions refresh the rows and their availability,
	// so a transport-started run disables Play/Delete and a recording published asynchronously
	// (even after the operation went idle) appears without any external refresh call.
	const FString RefreshKey = BuildRefreshKey();
	if (RefreshKey != LastOperationSignature)
	{
		LastOperationSignature = RefreshKey;
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
	Key += Operation.Data->GetStringField(TEXT("kind"));
	Key += TEXT("|");
	Key += Operation.Data->GetStringField(TEXT("state"));
	Key += TEXT("|");
	Key += FString::FromInt(
		static_cast<int32>(Operation.Data->GetNumberField(TEXT("recording_id"))));
	FString RunId;
	if (Operation.Data->TryGetStringField(TEXT("run_id"), RunId))
	{
		Key += TEXT("|");
		Key += RunId;
	}
	return Key;
}

bool SCortexReplayWindow::HasAnyPieWorldContext() const
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

	for (const FCortexReplayMetadata& Metadata : Recordings)
	{
		const bool bInUse = Service.IsValid() && Service->IsRecordInUse(Metadata.RecordingId);
		TSharedPtr<SCortexReplayRecordingRow> Row = SNew(SCortexReplayRecordingRow)
			.Recording(Metadata)
			.bSelected(Metadata.RecordingId == SelectedRecordingId)
			.bCompact(bCompactLayout)
			.bPlayEnabled(!bInUse)
			.bEditEnabled(true)
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
	for (int32 Index = 0; Index < Recordings.Num(); ++Index)
	{
		if (Recordings[Index].RecordingId == RecordingId)
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
		// No active operation: any rejection message from the last human action stays visible.
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
		// The backend reports borrowed capture as human origin, so ownership survives a
		// window close/reopen instead of relying on window-local state.
		FString Origin;
		Operation.Data->TryGetStringField(TEXT("origin"), Origin);
		const TCHAR* Ownership = Origin == TEXT("human") ? TEXT("borrowed") : TEXT("owned");
		Label = FString::Printf(TEXT("Recording #%d · %s · %s"), RecordingId, Ownership, *State);
		if (Operation.Data->GetBoolField(TEXT("publication_failed")))
		{
			Label += TEXT(" · save failed");
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
	const FCortexCommandResult Last = Service->GetLastRunForRecording(SelectedRecordingId);
	if (Last.bSuccess && Last.Data.IsValid())
	{
		RetainedState = Last.Data->GetStringField(TEXT("state"));
	}

	if (IsReplayActiveForRecording(SelectedRecordingId))
	{
		// A previous result is never presented as the verdict of the active run.
		return RetainedState.IsEmpty()
			? LOCTEXT("PlaybackInProgress", "Playback in progress.")
			: FText::FromString(FString::Printf(
				TEXT("Playback in progress. Previous run: %s. Gameplay correctness is not evaluated."),
				*RetainedState));
	}

	if (RetainedState.IsEmpty())
	{
		return LOCTEXT("NotReplayed", "Not replayed yet");
	}
	return FText::FromString(FString::Printf(
		TEXT("%s. Gameplay correctness is not evaluated."), *RetainedState));
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

void SCortexReplayWindow::HandleTargetChosen(int32 CandidateIndex)
{
	ChosenTargetIndex = CandidateIndex;
	if (bPendingRecordStart)
	{
		bPendingRecordStart = false;
		HidePopup();
		TargetChoiceDialog.Reset();
		StartBorrowedCaptureAtIndex(CandidateIndex);
	}
}

void SCortexReplayWindow::HandleTargetChoiceCancelled()
{
	// Cancel selects no target; never fall back to the first candidate.
	ChosenTargetIndex = INDEX_NONE;
	bPendingRecordStart = false;
	TargetChoiceDialog.Reset();
	HidePopup();
}

void SCortexReplayWindow::StartBorrowedCaptureAtIndex(int32 CandidateIndex)
{
	if (!Service.IsValid() || !PendingCandidates.IsValidIndex(CandidateIndex))
	{
		return;
	}

	const FCortexReplayCaptureTargetChoice& Choice = PendingCandidates[CandidateIndex];
	UWorld* World = Choice.World.Get();
	if (!World)
	{
		LastStatusMessage = TEXT("The selected capture target is no longer available");
		UpdateOperationLabel();
		return;
	}

	const FCortexCommandResult Started = Service->StartCaptureAtTarget(*World, Choice.LocalPlayerIndex);
	if (!Started.bSuccess)
	{
		// Vanished/ambiguous/held-input admission errors must be visible, not reported as Ready.
		SurfaceOperationError(Started);
		return;
	}
	RefreshLibrary();
}

void SCortexReplayWindow::HandleMetadataCommitted(int32 RecordingId, const FString& Name,
	const FString& Description, bool bAIEnabled)
{
	if (!Service.IsValid())
	{
		return;
	}

	const FCortexCommandResult Saved = Service->SaveMetadata(RecordingId, Name, Description, bAIEnabled);
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
	if (Service.IsValid())
	{
		Service->DeleteRecording(RecordingId);
	}
	DeleteDialog.Reset();
	HidePopup();
	if (SelectedRecordingId == RecordingId)
	{
		SelectedRecordingId = 0;
	}
	RefreshLibrary();
}

void SCortexReplayWindow::HandleDeleteDismissed()
{
	DeleteDialog.Reset();
	HidePopup();
}

void SCortexReplayWindow::HandlePlayClicked(int32 RecordingId)
{
	if (Service.IsValid())
	{
		Service->StartReplay(RecordingId, ECortexReplayOrigin::Human);
	}
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
