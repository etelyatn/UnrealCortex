#pragma once

#include "Brushes/SlateRoundedBoxBrush.h"
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Rendering/CortexFrontendColors.h"

class SBorder;
class SButton;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class STextBlock;

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

    void SetInputEnabled(bool bEnabled);
    void SetStreaming(bool bStreaming);
    void ClearInput();
    void FocusInput();

    void AddContextItem(const FString& Path);
    void RemoveContextItem(int32 Index);
    void ClearContextItems();
    const TArray<FString>& GetContextItems() const;

private:
    FReply OnSendClicked();
    void HandleSendOrNewline();
    void RebuildChips();

    TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
    TSharedPtr<SButton> ActionButton;
    TSharedPtr<STextBlock> ActionIcon;
    TSharedPtr<SBorder> ActionBorder;
    TSharedPtr<SWrapBox> ChipRow;
    TSharedPtr<SMenuAnchor> ModeDropdown;
    TSharedPtr<SMenuAnchor> ModelDropdown;
    TSharedPtr<SMenuAnchor> SettingsPopup;
    TSharedPtr<SEditableTextBox> DirectiveTextBox;
    TSharedPtr<STextBlock> DirectRadioIndicator;
    TSharedPtr<STextBlock> ThoroughRadioIndicator;
    TSharedPtr<STextBlock> ModeLabel;
    TSharedPtr<STextBlock> ModelLabel;
    TSharedPtr<STextBlock> EffortLabel;

    TArray<FString> ContextItems;

    TUniquePtr<FSlateRoundedBoxBrush> ModeBrush;
    TUniquePtr<FSlateRoundedBoxBrush> SendBrush;
    TUniquePtr<FSlateRoundedBoxBrush> DropdownBrush;

    FOnCortexSendMessage OnSendMessage;
    FOnCortexCancel OnCancel;
    bool bIsStreaming = false;
};
