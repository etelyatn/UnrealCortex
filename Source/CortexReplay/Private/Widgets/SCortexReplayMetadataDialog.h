#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"
#include "Input/Reply.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FKeyEvent;
class SButton;
class SCheckBox;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class STextBlock;

/** Commit of the three editable metadata fields; only Save fires this. */
DECLARE_DELEGATE_ThreeParams(FOnCortexReplayMetadataCommitted,
	const FString& /*Name*/, const FString& /*Description*/, bool /*bAIEnabled*/);

/** Cancel/Escape dismissal; drafts are discarded by the owner. */
DECLARE_DELEGATE(FOnCortexReplayMetadataDismissed);

/**
 * Editable metadata popup for one recording.
 *
 * The dialog owns only the editable draft name, description and AI permission. The recording ID,
 * creation date, canonical recorded map and guard coverage are read-only context, and the map is
 * shown in full and never editable. A partial-coverage recording surfaces an explicit warning that
 * names the unavailable UI presses; the press-only protection limit is always stated, because
 * enabling AI use never upgrades coverage. Save commits the three drafts together through
 * OnCommitted; Cancel (or Escape) discards drafts through OnDismissed.
 */
class SCortexReplayMetadataDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexReplayMetadataDialog) {}
		SLATE_ARGUMENT(FCortexReplayMetadata, Recording)
		SLATE_EVENT(FOnCortexReplayMetadataCommitted, OnCommitted)
		SLATE_EVENT(FOnCortexReplayMetadataDismissed, OnDismissed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	int32 GetRecordingId() const;
	const FCortexReplayMetadata& GetRecording() const;

	/** Draft accessors shared with the editable controls' own text/permission callbacks. */
	FString GetDraftName() const;
	FString GetDraftDescription() const;
	bool IsDraftAIEnabled() const;
	void EditDraftName(const FString& Name);
	void EditDraftDescription(const FString& Description);
	void SetDraftAIEnabled(bool bEnabled);

	/** Full canonical recorded map path shown read-only for context. */
	FText GetReadOnlyMapPath() const;

	/** True when this recording captured UI presses without an identifiable target guard. */
	bool HasCoverageWarning() const;

	/** Press-only / partial-coverage warning text; non-empty even without unavailable guards. */
	FText GetCoverageWarningText() const;

	/** Shows a native Save failure without discarding the drafts or closing the popup. */
	void SetCommitError(const FString& Message);

	/** The popup's own actions, wired to the Save/Cancel buttons and the Escape key. */
	FReply OnSaveClicked();
	FReply OnCancelClicked();

	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	TSharedPtr<STextBlock> IDText;
	TSharedPtr<STextBlock> DateText;
	TSharedPtr<STextBlock> MapText;
	TSharedPtr<STextBlock> CoverageWarning;
	/** Native Save failure, visible only after a rejected commit. */
	TSharedPtr<STextBlock> CommitError;

	TSharedPtr<SEditableTextBox> NameEdit;
	TSharedPtr<SMultiLineEditableTextBox> DescriptionEdit;
	TSharedPtr<SCheckBox> AIEdit;

	TSharedPtr<SButton> SaveButton;
	TSharedPtr<SButton> CancelButton;

private:
	void HandleNameChanged(const FText& NewText);
	void HandleDescriptionChanged(const FText& NewText);
	void HandleAIChanged(ECheckBoxState NewState);

	FCortexReplayMetadata Recording;
	FString DraftName;
	FString DraftDescription;
	bool bDraftAIEnabled = false;
	FOnCortexReplayMetadataCommitted OnCommitted;
	FOnCortexReplayMetadataDismissed OnDismissed;
};
