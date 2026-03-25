#include "Widgets/SCortexInputArea.h"

#include "CortexFrontendSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Rendering/CortexFrontendColors.h"
#include "Session/CortexSessionTypes.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
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
        // Section 1: Chip row
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SAssignNew(ChipRow, SWrapBox)
            .UseAllottedSize(true)
            .Visibility(EVisibility::Collapsed)
        ]
        // Section 2: Textarea
        + SVerticalBox::Slot()
        .AutoHeight()
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
        // Section 3: Controls row
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.0f, 2.0f)
        [
            SNew(SHorizontalBox)
            // "+" button
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SButton)
                .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("+")))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
                    .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                ]
            ]
            // Mode selector
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SAssignNew(ModeDropdown, SMenuAnchor)
                .Placement(MenuPlacement_AboveAnchor)
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
                    for (ECortexAccessMode Mode : { ECortexAccessMode::ReadOnly, ECortexAccessMode::Guided, ECortexAccessMode::FullAccess })
                    {
                        FString ModeStr;
                        switch (Mode)
                        {
                        case ECortexAccessMode::ReadOnly:   ModeStr = TEXT("Read-Only"); break;
                        case ECortexAccessMode::Guided:     ModeStr = TEXT("Guided"); break;
                        case ECortexAccessMode::FullAccess: ModeStr = TEXT("Full Access"); break;
                        }
                        Menu->AddSlot()
                        .AutoHeight()
                        [
                            SNew(SButton)
                            .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                            .OnClicked_Lambda([this, Mode]()
                            {
                                FCortexFrontendSettings::Get().SetAccessMode(Mode);
                                if (ModeLabel.IsValid())
                                {
                                    ModeLabel->SetText(FText::FromString(FCortexFrontendSettings::Get().GetAccessModeString()));
                                }
                                ModeDropdown->SetIsOpen(false);
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(ModeStr))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                            ]
                        ];
                    }
                    return Menu;
                })
                [
                    SNew(SButton)
                    .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                    .OnClicked_Lambda([this]()
                    {
                        ModeDropdown->SetIsOpen(true);
                        return FReply::Handled();
                    })
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                        .BorderBackgroundColor(CortexColors::ModeButtonBackground)
                        .Padding(FMargin(8.0f, 2.0f))
                        [
                            SAssignNew(ModeLabel, STextBlock)
                            .Text(FText::FromString(FCortexFrontendSettings::Get().GetAccessModeString()))
                            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                            .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText))
                        ]
                    ]
                ]
            ]
            // Model selector
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SAssignNew(ModelDropdown, SMenuAnchor)
                .Placement(MenuPlacement_AboveAnchor)
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
                    for (const FString& ModelId : FCortexFrontendSettings::Get().GetAvailableModels())
                    {
                        Menu->AddSlot()
                        .AutoHeight()
                        [
                            SNew(SButton)
                            .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                            .OnClicked_Lambda([this, ModelId]()
                            {
                                FCortexFrontendSettings::Get().SetSelectedModel(ModelId);
                                if (ModelLabel.IsValid())
                                {
                                    ModelLabel->SetText(FText::FromString(FCortexFrontendSettings::Get().GetSelectedModel()));
                                }
                                ModelDropdown->SetIsOpen(false);
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(ModelId))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                            ]
                        ];
                    }
                    return Menu;
                })
                [
                    SNew(SButton)
                    .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                    .OnClicked_Lambda([this]()
                    {
                        ModelDropdown->SetIsOpen(true);
                        return FReply::Handled();
                    })
                    [
                        SAssignNew(ModelLabel, STextBlock)
                        .Text(FText::FromString(FCortexFrontendSettings::Get().GetSelectedModel()))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                        .ColorAndOpacity(FSlateColor(CortexColors::TextSecondary))
                    ]
                ]
            ]
            // Spacer
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SSpacer)
            ]
            // Settings button
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("\u2699")))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
                    .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                ]
            ]
            // ActionButton (send/cancel)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SAssignNew(ActionButton, SButton)
                .OnClicked(this, &SCortexInputArea::OnSendClicked)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                    .BorderBackgroundColor(CortexColors::SendButtonColor)
                    .Padding(FMargin(8.0f, 4.0f))
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("\u2191")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
                        .ColorAndOpacity(FSlateColor(FLinearColor::White))
                    ]
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
    if (ActionButton.IsValid())
    {
        ActionButton->SetEnabled(bEnabled);
    }
}

void SCortexInputArea::SetStreaming(bool bStreaming)
{
    bIsStreaming = bStreaming;
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
    if (bIsStreaming)
    {
        OnCancel.ExecuteIfBound();
    }
    else
    {
        HandleSendOrNewline();
    }
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
