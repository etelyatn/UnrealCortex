#include "Widgets/SCortexReplayMetadataDialog.h"

#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CortexReplayWindow"

void SCortexReplayMetadataDialog::Construct(const FArguments& InArgs)
{
	Recording = InArgs._Recording;
	OnCommitted = InArgs._OnCommitted;
	OnDismissed = InArgs._OnDismissed;

	DraftName = Recording.Name;
	DraftDescription = Recording.Description;
	bDraftAIEnabled = Recording.bAIEnabled;

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
					SAssignNew(IDText, STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("Edit recording #%d"), Recording.RecordingId)))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SAssignNew(MapText, STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("Recorded map (read-only): %s"), *Recording.MapAssetPath)))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SAssignNew(DateText, STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("Created (read-only): %s"),
						*Recording.CreatedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M UTC")))))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SAssignNew(CoverageWarning, STextBlock)
					.Text(GetCoverageWarningText())
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor(0.90f, 0.76f, 0.55f, 1.0f))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SAssignNew(CommitError, STextBlock)
					.AutoWrapText(true)
					.Visibility(EVisibility::Collapsed)
					.ColorAndOpacity(FLinearColor(0.92f, 0.35f, 0.35f, 1.0f))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("NameLabel", "Name"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SAssignNew(NameEdit, SEditableTextBox)
					.Text(FText::FromString(DraftName))
					.OnTextChanged_Lambda([this](const FText& NewText)
					{
						HandleNameChanged(NewText);
					})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("DescriptionLabel", "Short description"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SAssignNew(DescriptionEdit, SMultiLineEditableTextBox)
					.Text(FText::FromString(DraftDescription))
					.OnTextChanged_Lambda([this](const FText& NewText)
					{
						HandleDescriptionChanged(NewText);
					})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SAssignNew(AIEdit, SCheckBox)
					.IsChecked(bDraftAIEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						HandleAIChanged(NewState);
					})
					[
						SNew(STextBlock).Text(LOCTEXT("AIEditLabel", "AI may replay this recording"))
					]
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
						.OnClicked(this, &SCortexReplayMetadataDialog::OnCancelClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(SaveButton, SButton)
						.Text(LOCTEXT("SaveLabel", "Save"))
						.OnClicked(this, &SCortexReplayMetadataDialog::OnSaveClicked)
					]
				]
			]
		]
	];
}

int32 SCortexReplayMetadataDialog::GetRecordingId() const
{
	return Recording.RecordingId;
}

const FCortexReplayMetadata& SCortexReplayMetadataDialog::GetRecording() const
{
	return Recording;
}

FString SCortexReplayMetadataDialog::GetDraftName() const
{
	return DraftName;
}

FString SCortexReplayMetadataDialog::GetDraftDescription() const
{
	return DraftDescription;
}

bool SCortexReplayMetadataDialog::IsDraftAIEnabled() const
{
	return bDraftAIEnabled;
}

void SCortexReplayMetadataDialog::EditDraftName(const FString& Name)
{
	DraftName = Name;
	if (NameEdit.IsValid())
	{
		NameEdit->SetText(FText::FromString(Name));
	}
}

void SCortexReplayMetadataDialog::EditDraftDescription(const FString& Description)
{
	DraftDescription = Description;
	if (DescriptionEdit.IsValid())
	{
		DescriptionEdit->SetText(FText::FromString(Description));
	}
}

void SCortexReplayMetadataDialog::SetDraftAIEnabled(bool bEnabled)
{
	bDraftAIEnabled = bEnabled;
	if (AIEdit.IsValid())
	{
		AIEdit->SetIsChecked(bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
	}
}

FText SCortexReplayMetadataDialog::GetReadOnlyMapPath() const
{
	return FText::FromString(Recording.MapAssetPath);
}

bool SCortexReplayMetadataDialog::HasCoverageWarning() const
{
	return Recording.GuardCoverage.UIUnavailablePresses > 0;
}

FText SCortexReplayMetadataDialog::GetCoverageWarningText() const
{
	const FString PressOnly = TEXT(
		"Protection is press-only: releases and drag effects are not verified. "
		"This is not a gameplay verdict.");

	if (Recording.GuardCoverage.UIUnavailablePresses > 0)
	{
		return FText::FromString(FString::Printf(
			TEXT("Protection warning: %d UI press has no identifiable target guard; pose checks only "
				"for that press. %s"),
			Recording.GuardCoverage.UIUnavailablePresses, *PressOnly));
	}

	return FText::FromString(FString::Printf(
		TEXT("No unavailable UI guards in this recording. %s"), *PressOnly));
}

void SCortexReplayMetadataDialog::SetCommitError(const FString& Message)
{
	if (CommitError.IsValid())
	{
		CommitError->SetText(FText::FromString(Message));
		CommitError->SetVisibility(Message.IsEmpty()
			? EVisibility::Collapsed : EVisibility::Visible);
	}
}

FReply SCortexReplayMetadataDialog::OnSaveClicked()
{
	OnCommitted.ExecuteIfBound(DraftName, DraftDescription, bDraftAIEnabled);
	return FReply::Handled();
}

FReply SCortexReplayMetadataDialog::OnCancelClicked()
{
	OnDismissed.ExecuteIfBound();
	return FReply::Handled();
}

bool SCortexReplayMetadataDialog::SupportsKeyboardFocus() const
{
	return true;
}

FReply SCortexReplayMetadataDialog::OnKeyDown(const FGeometry& /*MyGeometry*/,
	const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return OnCancelClicked();
	}
	return FReply::Unhandled();
}

void SCortexReplayMetadataDialog::HandleNameChanged(const FText& NewText)
{
	DraftName = NewText.ToString();
}

void SCortexReplayMetadataDialog::HandleDescriptionChanged(const FText& NewText)
{
	DraftDescription = NewText.ToString();
}

void SCortexReplayMetadataDialog::HandleAIChanged(ECheckBoxState NewState)
{
	bDraftAIEnabled = NewState == ECheckBoxState::Checked;
}

#undef LOCTEXT_NAMESPACE
