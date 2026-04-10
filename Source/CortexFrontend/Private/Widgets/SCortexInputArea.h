#pragma once

#include "AutoComplete/CortexAutoCompleteTypes.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Rendering/CortexFrontendColors.h"

class SBorder;
class SButton;
class SMultiLineEditableTextBox;
class STextBlock;
class SCortexAutoCompletePopup;
class FCortexMentionMarshaller;

DECLARE_DELEGATE_OneParam(FOnCortexSendMessage, const FString&);
DECLARE_DELEGATE(FOnCortexCancel);

class SCortexInputArea : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexInputArea) {}
        SLATE_EVENT(FOnCortexSendMessage, OnSendMessage)
        SLATE_EVENT(FOnCortexCancel, OnCancel)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    ~SCortexInputArea();

    void SetInputEnabled(bool bEnabled);
    void SetStreaming(bool bStreaming);
    void ClearInput();
    void FocusInput();

    // Context chip API (replaces AddContextItem/GetContextItems)
    void AddContextChip(const FCortexContextChip& Chip);
    void RemoveContextChip(int32 Index);
    void ClearContextChips();
    const TArray<FCortexContextChip>& GetContextChips() const;

    // Test helpers — expose internal state for automation tests
    void HandleTextChanged(const FText& NewText);
    bool IsAutoCompleteOpen() const;
    int32 GetAutoCompleteSelectedIndex() const { return AutoCompleteSelectedIndex; }
    const TArray<TSharedPtr<FCortexAutoCompleteItem>>& GetFilteredItems() const { return FilteredItems; }
    FText GetInputText() const;
    void TestResolveAndSend(const FString& Message)
    {
        TArray<FCortexContextChip> Chips = ContextItems;
        ClearContextChips();
        ResolveAndSend(Chips, Message);
    }
    static FString ParseFrontmatterField(const FString& FileContent, const FString& FieldName);

    virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
    virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;

private:
    FReply OnSendClicked();
    void HandleSendOrNewline();
    void HandleAttachEditorContextShortcut();
    void ResolveAndSend(const TArray<FCortexContextChip>& Chips, const FString& Message);
    FString ResolveProviderChip(const FString& Label);
    FString ResolveAssetChip(const FCortexContextChip& Chip, bool& bOutSuccess);
    void RebuildChips();

    // Autocomplete
    void OpenPopup();
    void ClosePopup();
    void FilterItems(const FString& Query);
    void CommitSelection();
    void LoadAssetCache();
    void DiscoverSkillsAndAgents();
    void PopulateProviders();
    void PopulateCoreCommands();

    // Widgets
    TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
    TSharedPtr<SButton> ActionButton;
    TSharedPtr<STextBlock> ActionIcon;
    TSharedPtr<SBorder> ActionBorder;
    TSharedPtr<SWrapBox> ChipRow;
    TSharedPtr<SMenuAnchor> ModeDropdown;
    TSharedPtr<SMenuAnchor> ModelDropdown;
    TSharedPtr<SMenuAnchor> SettingsPopup;
    TSharedPtr<SMenuAnchor> AutoCompleteAnchor;
    TSharedPtr<SCortexAutoCompletePopup> AutoCompletePopup;
    TSharedPtr<STextBlock> ModeLabel;
    TSharedPtr<STextBlock> ModelLabel;
    TSharedPtr<STextBlock> EffortLabel;

    // Context chips
    TArray<FCortexContextChip> ContextItems;

    // Autocomplete state
    FText PreviousText;
    int32 TriggerOffset = INDEX_NONE;
    TCHAR ActiveTrigger = TEXT('\0');
    int32 AutoCompleteSelectedIndex = 0;
    bool bAutoCompleteOpen = false;
    TArray<TSharedPtr<FCortexAutoCompleteItem>> FilteredItems;
    TArray<TSharedPtr<FCortexAutoCompleteItem>> ProviderItems;
    TArray<TSharedPtr<FCortexAutoCompleteItem>> AssetCache;
    TArray<TSharedPtr<FCortexAutoCompleteItem>> CommandCache;
    bool bAssetCacheLoading = false;
    FTSTicker::FDelegateHandle DiscoveryTickerHandle;
    FDelegateHandle AssetLoadedDelegateHandle;
    TSharedPtr<FCortexMentionMarshaller> MentionMarshaller;

    // Brushes
    TUniquePtr<FSlateRoundedBoxBrush> ModeBrush;
    TUniquePtr<FSlateRoundedBoxBrush> SendBrush;
    TUniquePtr<FSlateRoundedBoxBrush> DropdownBrush;

    FOnCortexSendMessage OnSendMessage;
    FOnCortexCancel OnCancel;
    bool bIsStreaming = false;
};
