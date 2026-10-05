#include "Widgets/SCortexReplayRecordingRow.h"

#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CortexReplayWindow"

namespace
{
constexpr float RowHeightDesktop = 48.0f;
constexpr float RowHeightCompact = 40.0f;
constexpr float MapColumnDesktop = 120.0f;
constexpr float MapColumnCompact = 92.0f;
constexpr float DateColumnDesktop = 106.0f;
constexpr float DateColumnCompact = 82.0f;
constexpr float AIColumnDesktop = 52.0f;
constexpr float AIColumnCompact = 42.0f;
constexpr float ActionButtonWidth = 27.0f;

FText ShortMapLabel(const FString& MapAssetPath)
{
	if (MapAssetPath.IsEmpty())
	{
		return LOCTEXT("NoRecordedMap", "(no map)");
	}
	return FText::FromString(FPaths::GetBaseFilename(MapAssetPath));
}

FText FormatCreatedAt(const FDateTime& CreatedAtUtc)
{
	return FText::FromString(CreatedAtUtc.ToString(TEXT("%Y-%m-%d %H:%M")));
}
} // namespace

void SCortexReplayRecordingRow::Construct(const FArguments& InArgs)
{
	Recording = InArgs._Recording;
	bSelected = InArgs._bSelected;
	bCompact = InArgs._bCompact;
	bPlayEnabled = InArgs._bPlayEnabled;
	bEditEnabled = InArgs._bEditEnabled;
	bDeleteEnabled = InArgs._bDeleteEnabled;
	OnPlay = InArgs._OnPlay;
	OnEdit = InArgs._OnEdit;
	OnDelete = InArgs._OnDelete;

	const int32 RecordingId = Recording.RecordingId;
	const FMargin RowPadding = bCompact ? FMargin(6.0f, 2.0f) : FMargin(10.0f, 4.0f);
	const float MapWidth = bCompact ? MapColumnCompact : MapColumnDesktop;
	const float DateWidth = bCompact ? DateColumnCompact : DateColumnDesktop;
	const float AIWidth = bCompact ? AIColumnCompact : AIColumnDesktop;

	ChildSlot
	[
		SNew(SBox)
		.MinDesiredHeight(bCompact ? RowHeightCompact : RowHeightDesktop)
		.MaxDesiredHeight(bCompact ? RowHeightCompact : RowHeightDesktop)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(this, &SCortexReplayRecordingRow::GetRowBackground)
			.Padding(RowPadding)
			[
				SNew(SHorizontalBox)

				// ID before the title, one-line description beneath.
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 5.0f, 0.0f)
						[
							SAssignNew(IDText, STextBlock)
							.Text(FText::FromString(FString::Printf(TEXT("#%d"), RecordingId)))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SAssignNew(TitleText, STextBlock)
							.Text(FText::FromString(Recording.Name))
							.ToolTipText(FText::FromString(Recording.Description))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(DescriptionText, STextBlock)
						.Text(FText::FromString(Recording.Description))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]

				// Read-only short map with the full canonical path in the tooltip/accessible name.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(MapWidth)
					[
						SAssignNew(MapText, STextBlock)
						.Text(ShortMapLabel(Recording.MapAssetPath))
						.ToolTipText(FText::FromString(Recording.MapAssetPath))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(DateWidth)
					[
						SAssignNew(DateText, STextBlock)
						.Text(FormatCreatedAt(Recording.CreatedAtUtc))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]

				// Read-only AI-use indicator: a text surface, never a checkbox or keyboard toggle.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(AIWidth)
					[
						SAssignNew(AIIndicator, STextBlock)
						.Text(Recording.bAIEnabled ? LOCTEXT("AIOn", "AI on") : LOCTEXT("AIOff", "AI off"))
						.ToolTipText(LOCTEXT("AIIndicatorTooltip",
							"AI use is read-only in the list. Change it in Edit."))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(1.0f, 0.0f)
					[
						SAssignNew(PlayButton, SButton)
						.ToolTipText(LOCTEXT("PlayRowTooltip", "Play this recording"))
						.ContentPadding(FMargin(4.0f, 2.0f))
						.IsEnabled(bPlayEnabled)
						.OnClicked(this, &SCortexReplayRecordingRow::HandlePlayClicked)
						[
							SNew(SImage).Image(FAppStyle::GetBrush("Icons.Play"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(1.0f, 0.0f)
					[
						SAssignNew(EditButton, SButton)
						.ToolTipText(LOCTEXT("EditRowTooltip", "Edit this recording's name, description and AI permission"))
						.ContentPadding(FMargin(4.0f, 2.0f))
						.IsEnabled(bEditEnabled)
						.OnClicked(this, &SCortexReplayRecordingRow::HandleEditClicked)
						[
							SNew(SImage).Image(FAppStyle::GetBrush("ContentBrowser.AssetActions.Edit"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(1.0f, 0.0f)
					[
						SAssignNew(DeleteButton, SButton)
						.ToolTipText(LOCTEXT("DeleteRowTooltip", "Delete this recording"))
						.ContentPadding(FMargin(4.0f, 2.0f))
						.IsEnabled(bDeleteEnabled)
						.OnClicked(this, &SCortexReplayRecordingRow::HandleDeleteClicked)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("ContentBrowser.AssetActions.Delete"))
							.ColorAndOpacity(FLinearColor(0.85f, 0.22f, 0.22f, 1.0f))
						]
					]
				]
			]
		]
	];
}

int32 SCortexReplayRecordingRow::GetRecordingId() const
{
	return Recording.RecordingId;
}

const FCortexReplayMetadata& SCortexReplayRecordingRow::GetRecording() const
{
	return Recording;
}

bool SCortexReplayRecordingRow::IsSelected() const
{
	return bSelected;
}

bool SCortexReplayRecordingRow::IsCompact() const
{
	return bCompact;
}

FText SCortexReplayRecordingRow::GetMapLabel() const
{
	return ShortMapLabel(Recording.MapAssetPath);
}

FText SCortexReplayRecordingRow::GetMapTooltipText() const
{
	return FText::FromString(Recording.MapAssetPath);
}

FText SCortexReplayRecordingRow::GetAIIndicatorText() const
{
	return Recording.bAIEnabled ? LOCTEXT("AIOn", "AI on") : LOCTEXT("AIOff", "AI off");
}

FText SCortexReplayRecordingRow::GetAIIndicatorTooltip() const
{
	return LOCTEXT("AIIndicatorTooltip",
		"AI use is read-only in the list. Change it in Edit.");
}

FSlateColor SCortexReplayRecordingRow::GetRowBackground() const
{
	return bSelected ? FSlateColor(FLinearColor(0.10f, 0.15f, 0.22f, 1.0f))
		: FSlateColor(FLinearColor::Transparent);
}

FReply SCortexReplayRecordingRow::HandlePlayClicked()
{
	OnPlay.ExecuteIfBound(Recording.RecordingId);
	return FReply::Handled();
}

FReply SCortexReplayRecordingRow::HandleEditClicked()
{
	OnEdit.ExecuteIfBound(Recording.RecordingId);
	return FReply::Handled();
}

FReply SCortexReplayRecordingRow::HandleDeleteClicked()
{
	OnDelete.ExecuteIfBound(Recording.RecordingId);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
