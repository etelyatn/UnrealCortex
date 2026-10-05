#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "Styling/SlateColor.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SButton;
class STextBlock;

/** A row action; the recording ID identifies which row was activated. */
DECLARE_DELEGATE_OneParam(FOnCortexReplayRowAction, int32 /*RecordingId*/);

/**
 * One compact library row (approximately 48 px tall) for the independent Replay window.
 *
 * Layout: integer ID before the title, a one-line description beneath it, a read-only short map
 * name whose tooltip and accessible name expose the full canonical asset path, the creation
 * date/time, a read-only AI-use indicator, and Play/Edit/Delete icon actions.
 *
 * The AI indicator is deliberately a non-interactive read-only surface: it is never an editable
 * checkbox or a keyboard toggle, and permission can only change through the Edit popup. The map
 * column is likewise read-only and never an editable text field. Compact mode keeps every required
 * field present when the window is docked narrow.
 */
class SCortexReplayRecordingRow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayRecordingRow) {}
		SLATE_ARGUMENT(FCortexReplayMetadata, Recording)
		SLATE_ARGUMENT(bool, bSelected)
		SLATE_ARGUMENT(bool, bCompact)
		SLATE_ARGUMENT(bool, bPlayEnabled)
		SLATE_ARGUMENT(bool, bEditEnabled)
		SLATE_ARGUMENT(bool, bDeleteEnabled)
		SLATE_EVENT(FOnCortexReplayRowAction, OnPlay)
		SLATE_EVENT(FOnCortexReplayRowAction, OnEdit)
		SLATE_EVENT(FOnCortexReplayRowAction, OnDelete)
		SLATE_EVENT(FOnCortexReplayRowAction, OnSelected)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override;

	int32 GetRecordingId() const;
	const FCortexReplayMetadata& GetRecording() const;
	bool IsSelected() const;
	bool IsCompact() const;

	/** Short map label shown in the column; the tooltip carries the full canonical path. */
	FText GetMapLabel() const;
	FText GetMapTooltipText() const;

	/** Read-only AI-use indicator text ("AI" / "AI off") and its Edit-directing tooltip. */
	FText GetAIIndicatorText() const;
	FText GetAIIndicatorTooltip() const;

	/** The displayed read-only map column. Never an editable text field. */
	TSharedPtr<STextBlock> MapText;

	/** The displayed read-only AI-use indicator. Never an editable checkbox. */
	TSharedPtr<STextBlock> AIIndicator;

	TSharedPtr<STextBlock> IDText;
	TSharedPtr<STextBlock> TitleText;
	TSharedPtr<STextBlock> DescriptionText;
	TSharedPtr<STextBlock> DateText;

	TSharedPtr<SButton> PlayButton;
	TSharedPtr<SButton> EditButton;
	TSharedPtr<SButton> DeleteButton;

private:
	FSlateColor GetRowBackground() const;
	FReply HandlePlayClicked();
	FReply HandleEditClicked();
	FReply HandleDeleteClicked();

	FCortexReplayMetadata Recording;
	bool bSelected = false;
	bool bCompact = false;
	bool bPlayEnabled = true;
	bool bEditEnabled = true;
	bool bDeleteEnabled = true;
	FOnCortexReplayRowAction OnPlay;
	FOnCortexReplayRowAction OnEdit;
	FOnCortexReplayRowAction OnDelete;
	FOnCortexReplayRowAction OnSelected;
};
