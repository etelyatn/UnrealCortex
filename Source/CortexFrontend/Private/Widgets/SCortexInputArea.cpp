#include "Widgets/SCortexInputArea.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexInputArea::Construct(const FArguments& InArgs)
{
    OnSendMessage = InArgs._OnSendMessage;
    OnCancel = InArgs._OnCancel;

    ChildSlot
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        .Padding(4.0f)
        [
            SAssignNew(InputTextBox, SMultiLineEditableTextBox)
            .HintText(FText::FromString(TEXT("Ask Cortex anything...")))
            .AutoWrapText(true)
            .OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
            {
                if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown())
                {
                    HandleSendOrNewline();
                    return FReply::Handled();
                }
                return FReply::Unhandled();
            })
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f)
        .VAlign(VAlign_Bottom)
        [
            SAssignNew(SendButton, SButton)
            .OnClicked(this, &SCortexInputArea::OnSendClicked)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Send")))
            ]
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f)
        .VAlign(VAlign_Bottom)
        [
            SAssignNew(CancelButton, SButton)
            .OnClicked(this, &SCortexInputArea::OnCancelClicked)
            .Visibility(EVisibility::Collapsed)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Cancel")))
            ]
        ]
    ];
}

void SCortexInputArea::SetInputEnabled(bool bEnabled)
{
    if (InputTextBox.IsValid())
    {
        InputTextBox->SetEnabled(bEnabled);
    }
    if (SendButton.IsValid())
    {
        SendButton->SetEnabled(bEnabled);
    }
}

void SCortexInputArea::SetStreaming(bool bStreaming)
{
    bIsStreaming = bStreaming;
    if (SendButton.IsValid())
    {
        SendButton->SetVisibility(bStreaming ? EVisibility::Collapsed : EVisibility::Visible);
    }
    if (CancelButton.IsValid())
    {
        CancelButton->SetVisibility(bStreaming ? EVisibility::Visible : EVisibility::Collapsed);
    }
}

void SCortexInputArea::ClearInput()
{
    if (InputTextBox.IsValid())
    {
        InputTextBox->SetText(FText::GetEmpty());
    }
}

void SCortexInputArea::FocusInput()
{
    if (InputTextBox.IsValid() && FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().SetKeyboardFocus(InputTextBox);
    }
}

void SCortexInputArea::HandleSendOrNewline()
{
    if (bIsStreaming || !InputTextBox.IsValid())
    {
        return;
    }

    FString Text = InputTextBox->GetText().ToString();
    Text.TrimStartAndEndInline();
    if (!Text.IsEmpty())
    {
        OnSendMessage.ExecuteIfBound(Text);
    }
}

FReply SCortexInputArea::OnSendClicked()
{
    HandleSendOrNewline();
    return FReply::Handled();
}

FReply SCortexInputArea::OnCancelClicked()
{
    OnCancel.ExecuteIfBound();
    return FReply::Handled();
}
