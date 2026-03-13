#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

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

private:
    FReply OnSendClicked();
    FReply OnCancelClicked();
    void HandleSendOrNewline();

    TSharedPtr<SMultiLineEditableTextBox> InputTextBox;
    TSharedPtr<SButton> SendButton;
    TSharedPtr<SButton> CancelButton;

    FOnCortexSendMessage OnSendMessage;
    FOnCortexCancel OnCancel;
    bool bIsStreaming = false;
};
