#include "Widgets/SCortexInputArea.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Rendering/CortexFrontendColors.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexInputArea::Construct(const FArguments& InArgs)
{
    OnSendMessage = InArgs._OnSendMessage;
    OnCancel = InArgs._OnCancel;

    ChildSlot
    [
        SNew(SVerticalBox)
        // Chip row
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(ChipRow, SWrapBox)
            .UseAllottedWidth(true)
            .Visibility(EVisibility::Collapsed)
        ]
        // Input row (existing layout)
        + SVerticalBox::Slot()
        .AutoHeight()
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
        // Prepend context chips as @path references
        FString FullPrompt;
        for (const FString& Item : ContextItems)
        {
            FullPrompt += FString::Printf(TEXT("@%s\n"), *Item);
        }
        FullPrompt += Text;

        OnSendMessage.ExecuteIfBound(FullPrompt);
        ClearContextItems();
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

void SCortexInputArea::AddContextItem(const FString& Path)
{
    ContextItems.Add(Path);
    RebuildChips();
}

void SCortexInputArea::RemoveContextItem(int32 Index)
{
    if (ContextItems.IsValidIndex(Index))
    {
        ContextItems.RemoveAt(Index);
        RebuildChips();
    }
}

void SCortexInputArea::ClearContextItems()
{
    ContextItems.Empty();
    RebuildChips();
}

const TArray<FString>& SCortexInputArea::GetContextItems() const
{
    return ContextItems;
}

void SCortexInputArea::RebuildChips()
{
    if (!ChipRow.IsValid())
    {
        return;
    }

    ChipRow->ClearChildren();

    for (int32 i = 0; i < ContextItems.Num(); ++i)
    {
        const FString& Item = ContextItems[i];
        FString Filename = FPaths::GetCleanFilename(Item);

        const int32 ChipIndex = i;
        ChipRow->AddSlot()
        .Padding(2.0f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor(CortexColors::ChipBackground)
            .Padding(FMargin(6.0f, 2.0f))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Filename))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::ChipNameColor))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SButton)
                    .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                    .OnClicked_Lambda([this, ChipIndex]()
                    {
                        RemoveContextItem(ChipIndex);
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("\u2715")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                        .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                    ]
                ]
            ]
        ];
    }

    ChipRow->SetVisibility(ContextItems.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed);
}
