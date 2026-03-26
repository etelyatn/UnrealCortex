#pragma once

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
    TSharedPtr<STextBlock> ModeLabel;
    TSharedPtr<STextBlock> ModelLabel;

    TArray<FString> ContextItems;

    FOnCortexSendMessage OnSendMessage;
    FOnCortexCancel OnCancel;
    bool bIsStreaming = false;
};
