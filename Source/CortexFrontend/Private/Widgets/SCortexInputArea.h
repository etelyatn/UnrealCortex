#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Rendering/CortexFrontendColors.h"

class SButton;
class SMultiLineEditableTextBox;

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
    FReply OnCancelClicked();
    void HandleSendOrNewline();
    void RebuildChips();

    TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
    TSharedPtr<SButton> SendButton;
    TSharedPtr<SButton> CancelButton;
    TSharedPtr<SWrapBox> ChipRow;

    TArray<FString> ContextItems;

    FOnCortexSendMessage OnSendMessage;
    FOnCortexCancel OnCancel;
    bool bIsStreaming = false;
};
