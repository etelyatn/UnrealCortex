#pragma once

#include "CoreMinimal.h"
#include "CortexReplayService.h"
#include "CortexReplayTypes.h"
#include "Input/Reply.h"
#include "Templates/SharedPointer.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SBox;
class SButton;
class SCortexReplayMetadataDialog;
class SCortexReplayRecordingRow;
class STextBlock;
class SVerticalBox;
struct FKeyEvent;

/**
 * Which local-playback summary the selected row currently shows.
 *
 * Bound separately from the current-operation status so a previous terminal result can never
 * masquerade as the verdict of an active or new run.
 */
enum class ECortexReplayPlaybackSummary : uint8
{
	/** No retained local run for the selection. */
	None,
	/** A terminal result is retained and no run for this recording is active. */
	PreviousTerminal,
	/** A run for this recording is active; any retained terminal result is shown as previous. */
	ActiveRun
};

/** Deletion confirmation; fires only after the human confirms. */
DECLARE_DELEGATE_OneParam(FOnCortexReplayDeleteConfirmed, int32 /*RecordingId*/);

/** Explicit capture-target choice; fires with the chosen candidate index. */
DECLARE_DELEGATE_OneParam(FOnCortexReplayTargetChosen, int32 /*CandidateIndex*/);

/**
 * Delete confirmation popup.
 *
 * Names the recording and its ID, explains that removal cannot be undone by the plugin, and gives
 * Cancel initial keyboard focus. Deletion of the active recording is blocked until it stops.
 */
class SCortexReplayDeleteDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayDeleteDialog) {}
		SLATE_ARGUMENT(int32, RecordingId)
		SLATE_ARGUMENT(FString, RecordingName)
		SLATE_ARGUMENT(bool, bDeleteBlocked)
		SLATE_EVENT(FOnCortexReplayDeleteConfirmed, OnConfirmed)
		SLATE_EVENT(FSimpleDelegate, OnDismissed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	int32 GetRecordingId() const;
	bool IsDeleteBlocked() const;

	/** Widget that receives initial keyboard focus; the confirmation defaults to Cancel. */
	TSharedPtr<SWidget> GetInitialFocusWidget() const;

	FReply OnCancelClicked();
	FReply OnConfirmClicked();

	virtual bool SupportsKeyboardFocus() const override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	TSharedPtr<STextBlock> TitleText;
	TSharedPtr<STextBlock> NameText;
	TSharedPtr<SButton> CancelButton;
	TSharedPtr<SButton> ConfirmButton;

private:
	int32 RecordingId = 0;
	FString RecordingName;
	bool bDeleteBlocked = false;
	bool bRequestedInitialFocus = false;
	FOnCortexReplayDeleteConfirmed OnConfirmed;
	FSimpleDelegate OnDismissed;
};

/**
 * Transient target-choice popup shown only when Record sees several ready PIE candidates.
 *
 * Candidates display map / local-player index / viewport label. No candidate is selected by
 * default: the human must choose explicitly, and Cancel selects none (never the first world).
 */
class SCortexReplayTargetChoiceDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayTargetChoiceDialog) {}
		SLATE_ARGUMENT(TArray<FCortexReplayCaptureTargetChoice>, Candidates)
		SLATE_EVENT(FOnCortexReplayTargetChosen, OnChosen)
		SLATE_EVENT(FSimpleDelegate, OnDismissed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	int32 GetCandidateCount() const;
	FString GetCandidateLabel(int32 Index) const;

	/** INDEX_NONE until a candidate is explicitly chosen; never defaults to the first. */
	int32 GetSelectedIndex() const;

	void SelectCandidate(int32 Index);
	void Cancel();
	FReply OnCancelClicked();

	TSharedPtr<SButton> CancelButton;

private:
	TArray<FCortexReplayCaptureTargetChoice> Candidates;
	int32 SelectedIndex = INDEX_NONE;
	FOnCortexReplayTargetChosen OnChosen;
	FSimpleDelegate OnDismissed;
};

/**
 * Independent human recording-library window hosted in a nomad tab.
 *
 * Compact library with a top Record/Stop toolbar and a short operation label, one row per human
 * recording, an Edit metadata popup, a delete confirmation and an explicit capture-target choice.
 * The window never owns backend runs: closing it is not Stop, and reopening it reflects the current
 * service state. There is no AI-enable or authoring shortcut here; the only way to change permission
 * is the Edit popup's Save.
 */
class SCortexReplayWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayWindow) {}
		SLATE_ARGUMENT(TSharedPtr<FCortexReplayService>, Service)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-reads the human library and the selected record's status from the service. */
	void RefreshLibrary();

	const TArray<TSharedPtr<SCortexReplayRecordingRow>>& GetRows() const;
	TSharedPtr<SCortexReplayRecordingRow> GetRow(int32 RecordingId) const;

	void SelectRecording(int32 RecordingId);
	int32 GetSelectedRecordingId() const;

	/** Toolbar actions. */
	FReply OnRecordClicked();
	FReply OnStopClicked();
	void BeginRecord();

	/**
	 * Post-enumeration Record entry: routes an explicit candidate set to an owned saved-map
	 * capture (empty), one explicitly selected target, or the transient choice popup. Public so the
	 * record path can be exercised without a live enumeration.
	 */
	void BeginRecordForCandidates(const TArray<FCortexReplayCaptureTargetChoice>& Candidates);

	void StopActiveOperation();
	FText GetOperationLabel() const;

	/**
	 * Turns service candidates into a live capture target. Zero candidates leaves the owned
	 * saved-editor-map path; one ready candidate is selected explicitly; several require the
	 * transient choice popup. Never attaches to the first world.
	 */
	TSharedPtr<SCortexReplayTargetChoiceDialog> PromptForCaptureTarget(
		const TArray<FCortexReplayCaptureTargetChoice>& Candidates);
	bool HasChosenCaptureTarget() const;
	FCortexReplayCaptureTargetChoice GetChosenCaptureTarget() const;

	/** Edit popup lifecycle; metadata drafts are committed through the service on Save. */
	TSharedPtr<SCortexReplayMetadataDialog> OpenMetadataDialog(int32 RecordingId);
	TSharedPtr<SCortexReplayMetadataDialog> GetOpenMetadataDialog() const;
	void CloseMetadataDialog();

	/** Delete confirmation lifecycle; the active recording's deletion stays blocked. */
	TSharedPtr<SCortexReplayDeleteDialog> OpenDeleteConfirmation(int32 RecordingId);
	TSharedPtr<SCortexReplayDeleteDialog> GetDeleteConfirmation() const;
	bool CanDeleteRecording(int32 RecordingId) const;

	/** Selected-record local last-playback summary, separate from the current-operation label. */
	ECortexReplayPlaybackSummary GetSelectedPlaybackSummaryKind() const;
	FText GetSelectedPlaybackSummaryText() const;

	/** Compact (narrow-dock) layout keeps every required row field visible. */
	void SetCompactLayout(bool bCompact);
	bool IsCompactLayout() const;

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

	/** Short current-operation label ("Ready", owned/borrowed capture, wait reason/budget). */
	TSharedPtr<STextBlock> OperationLabel;

	/** Short local last-playback summary for the selected record. */
	TSharedPtr<STextBlock> PlaybackSummaryText;

private:
	/** Rebuilds rows and the popup-free parts of the window from the current service state. */
	void RebuildRows();
	void UpdateOperationLabel();
	void UpdatePlaybackSummary();
	void UpdateToolbarEnablement();

	int32 FindRecordingIndex(int32 RecordingId) const;
	FText BuildOperationLabel() const;
	FText BuildPlaybackSummaryText() const;
	bool IsReplayActiveForRecording(int32 RecordingId) const;
	bool HasAnyPieWorldContext() const;
	FString BuildRefreshKey() const;
	void UpdateCompactLayoutFromGeometry(const FGeometry& AllottedGeometry);
	void SurfaceOperationError(const FCortexCommandResult& Result);

	void ShowPopup(const TSharedPtr<SWidget>& Content);
	void HidePopup();

	void HandlePlayClicked(int32 RecordingId);
	void HandleEditClicked(int32 RecordingId);
	void HandleDeleteClicked(int32 RecordingId);
	void HandleRowSelected(int32 RecordingId);

	void HandleTargetChosen(int32 CandidateIndex);
	void HandleTargetChoiceCancelled();
	void StartBorrowedCaptureAtIndex(int32 CandidateIndex);

	void HandleMetadataCommitted(int32 RecordingId, const FString& Name,
		const FString& Description, bool bAIEnabled);
	void HandleMetadataDismissed();
	void HandleDeleteConfirmed(int32 RecordingId);
	void HandleDeleteDismissed();

	TSharedPtr<FCortexReplayService> Service;

	TArray<FCortexReplayMetadata> Recordings;
	TArray<TSharedPtr<SCortexReplayRecordingRow>> Rows;

	TSharedPtr<SBox> PopupLayer;
	TSharedPtr<SVerticalBox> RowContainer;
	TSharedPtr<SButton> RecordButton;
	TSharedPtr<SButton> StopButton;

	TSharedPtr<SCortexReplayMetadataDialog> MetadataDialog;
	TSharedPtr<SCortexReplayDeleteDialog> DeleteDialog;
	TSharedPtr<SCortexReplayTargetChoiceDialog> TargetChoiceDialog;

	TArray<FCortexReplayCaptureTargetChoice> PendingCandidates;
	int32 ChosenTargetIndex = INDEX_NONE;
	bool bPendingRecordStart = false;
	int32 SelectedRecordingId = 0;
	bool bCompactLayout = false;
	/** Last backend operation signature; a change refreshes the rows (start/completion/publication). */
	FString LastOperationSignature;
	TSharedPtr<SWidget> PopupContent;
	FString LastStatusMessage;
	double TimeSinceOperationRefresh = 0.0;
};
