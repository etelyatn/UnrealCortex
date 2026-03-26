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
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    const FButtonStyle& GetHoverButtonStyle()
    {
        static FButtonStyle Style = []()
        {
            FButtonStyle S = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder");

            FSlateBrush HoveredBrush = *FAppStyle::GetBrush("WhiteBrush");
            HoveredBrush.TintColor = FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2a2a2a"))));
            S.SetHovered(HoveredBrush);

            FSlateBrush PressedBrush = *FAppStyle::GetBrush("WhiteBrush");
            PressedBrush.TintColor = FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("333333"))));
            S.SetPressed(PressedBrush);

            return S;
        }();
        return Style;
    }
}

void SCortexInputArea::Construct(const FArguments& InArgs)
{
    OnSendMessage = InArgs._OnSendMessage;
    OnCancel = InArgs._OnCancel;

    ModeBrush    = MakeUnique<FSlateRoundedBoxBrush>(CortexColors::ModeButtonBackground, 4.0f);
    SendBrush    = MakeUnique<FSlateRoundedBoxBrush>(FLinearColor::White, 14.0f);
    DropdownBrush = MakeUnique<FSlateRoundedBoxBrush>(
        CortexColors::ChipBackground, 6.0f,
        CortexColors::CodeBorder, 1.0f);

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
        .Padding(FMargin(12.0f, 8.0f, 12.0f, 4.0f))
        [
            SNew(SBox)
            .MinDesiredHeight(44.0f)
            [
                SAssignNew(InputTextBox, SMultiLineEditableTextBox)
                .HintText(FText::FromString(TEXT("@ for context or ask anything...")))
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
        ]
        // Section 3: Controls row
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(FMargin(12.0f, 4.0f, 12.0f, 10.0f))
        [
            SNew(SHorizontalBox)
            // Mode selector
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SAssignNew(ModeDropdown, SMenuAnchor)
                .Placement(MenuPlacement_AboveAnchor)
                .UseApplicationMenuStack(true)
                .OnGetMenuContent_Lambda([weakThis = TWeakPtr<SCortexInputArea>(SharedThis(this))]() -> TSharedRef<SWidget>
                {
                    TSharedPtr<SCortexInputArea> Self = weakThis.Pin();
                    if (!Self.IsValid()) { return SNullWidget::NullWidget; }

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
                            .ButtonStyle(&GetHoverButtonStyle())
                            .ContentPadding(FMargin(12.0f, 6.0f))
                            .OnClicked_Lambda([Self, Mode]()
                            {
                                FCortexFrontendSettings::Get().SetAccessMode(Mode);
                                if (Self->ModeLabel.IsValid())
                                {
                                    Self->ModeLabel->SetText(FText::FromString(FCortexFrontendSettings::Get().GetAccessModeString()));
                                }
                                Self->ModeDropdown->SetIsOpen(false);
                                return FReply::Handled();
                            })
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(ModeStr))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
                            ]
                        ];
                    }
                    return SNew(SBorder)
                        .BorderImage(Self->DropdownBrush.Get())
                        .Padding(FMargin(0.0f, 4.0f))
                        [ Menu ];
                })
                [
                    SNew(SButton)
                    .ButtonStyle(&GetHoverButtonStyle())
                    .ContentPadding(0.0f)
                    .OnClicked_Lambda([this]()
                    {
                        ModeDropdown->SetIsOpen(true);
                        return FReply::Handled();
                    })
                    [
                        SNew(SBorder)
                        .BorderImage(ModeBrush.Get())
                        .Padding(FMargin(10.0f, 3.0f))
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot()
                            .AutoWidth()
                            .VAlign(VAlign_Center)
                            [
                                SAssignNew(ModeLabel, STextBlock)
                                .Text(FText::FromString(FCortexFrontendSettings::Get().GetAccessModeString()))
                                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
                                .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText))
                            ]
                            + SHorizontalBox::Slot()
                            .AutoWidth()
                            .VAlign(VAlign_Center)
                            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("\u25B2")))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
                                .ColorAndOpacity(FSlateColor(FLinearColor(0.29f, 0.87f, 0.50f, 0.6f)))
                            ]
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
                .UseApplicationMenuStack(true)
                .OnGetMenuContent_Lambda([weakThis = TWeakPtr<SCortexInputArea>(SharedThis(this))]() -> TSharedRef<SWidget>
                {
                    TSharedPtr<SCortexInputArea> Self = weakThis.Pin();
                    if (!Self.IsValid()) { return SNullWidget::NullWidget; }

                    TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);

                    // === Model section ===
                    Menu->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(12.0f, 6.0f, 12.0f, 2.0f))
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("MODEL")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                        .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                    ];

                    const FString CurrentModel = FCortexFrontendSettings::Get().GetSelectedModel();
                    for (const FString& ModelId : FCortexFrontendSettings::Get().GetAvailableModels())
                    {
                        const bool bIsSelected = (ModelId == CurrentModel);
                        Menu->AddSlot()
                        .AutoHeight()
                        [
                            SNew(SButton)
                            .ButtonStyle(&GetHoverButtonStyle())
                            .ContentPadding(FMargin(12.0f, 6.0f))
                            .OnClicked_Lambda([Self, ModelId]()
                            {
                                FCortexFrontendSettings::Get().SetSelectedModel(ModelId);
                                if (Self->ModelLabel.IsValid())
                                {
                                    Self->ModelLabel->SetText(FText::FromString(
                                        FCortexFrontendSettings::Get().GetSelectedModel()));
                                }
                                Self->ModelDropdown->SetIsOpen(false);
                                return FReply::Handled();
                            })
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .VAlign(VAlign_Center)
                                .Padding(0.0f, 0.0f, 6.0f, 0.0f)
                                [
                                    SNew(STextBlock)
                                    .Text(FText::FromString(bIsSelected ? TEXT("\u2713") : TEXT(" ")))
                                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                    .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText))
                                ]
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .VAlign(VAlign_Center)
                                [
                                    SNew(STextBlock)
                                    .Text(FText::FromString(ModelId))
                                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                    .ColorAndOpacity(FSlateColor(CortexColors::TextSecondary))
                                ]
                            ]
                        ];
                    }

                    // === Separator ===
                    Menu->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(8.0f, 4.0f))
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                        .BorderBackgroundColor(CortexColors::ToolBlockBorder)
                        [
                            SNew(SBox).HeightOverride(1.0f)
                        ]
                    ];

                    // === Effort section ===
                    Menu->AddSlot()
                    .AutoHeight()
                    .Padding(FMargin(12.0f, 2.0f, 12.0f, 2.0f))
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("EFFORT")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                        .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                    ];

                    const ECortexEffortLevel CurrentEffort = FCortexFrontendSettings::Get().GetEffortLevel();
                    for (ECortexEffortLevel Level : {
                        ECortexEffortLevel::Default,
                        ECortexEffortLevel::Low,
                        ECortexEffortLevel::Medium,
                        ECortexEffortLevel::High,
                        ECortexEffortLevel::Maximum })
                    {
                        FString LevelStr;
                        switch (Level)
                        {
                        case ECortexEffortLevel::Default: LevelStr = TEXT("Default"); break;
                        case ECortexEffortLevel::Low:     LevelStr = TEXT("Low"); break;
                        case ECortexEffortLevel::Medium:  LevelStr = TEXT("Medium"); break;
                        case ECortexEffortLevel::High:    LevelStr = TEXT("High"); break;
                        case ECortexEffortLevel::Maximum: LevelStr = TEXT("Max"); break;
                        }
                        const bool bIsSelected = (Level == CurrentEffort);
                        Menu->AddSlot()
                        .AutoHeight()
                        [
                            SNew(SButton)
                            .ButtonStyle(&GetHoverButtonStyle())
                            .ContentPadding(FMargin(12.0f, 6.0f))
                            .OnClicked_Lambda([Self, Level, LevelStr]()
                            {
                                FCortexFrontendSettings::Get().SetEffortLevel(Level);
                                if (Self->EffortLabel.IsValid())
                                {
                                    const bool bShowEffort = (Level != ECortexEffortLevel::Default);
                                    Self->EffortLabel->SetText(FText::FromString(bShowEffort ? LevelStr : TEXT("")));
                                    Self->EffortLabel->SetVisibility(bShowEffort ? EVisibility::Visible : EVisibility::Collapsed);
                                }
                                Self->ModelDropdown->SetIsOpen(false);
                                return FReply::Handled();
                            })
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .VAlign(VAlign_Center)
                                .Padding(0.0f, 0.0f, 6.0f, 0.0f)
                                [
                                    SNew(STextBlock)
                                    .Text(FText::FromString(bIsSelected ? TEXT("\u2713") : TEXT(" ")))
                                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                    .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText))
                                ]
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .VAlign(VAlign_Center)
                                [
                                    SNew(STextBlock)
                                    .Text(FText::FromString(LevelStr))
                                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                    .ColorAndOpacity(FSlateColor(CortexColors::TextSecondary))
                                ]
                            ]
                        ];
                    }

                    return SNew(SBorder)
                        .BorderImage(Self->DropdownBrush.Get())
                        .Padding(FMargin(0.0f, 4.0f))
                        [ Menu ];
                })
                [
                    SNew(SButton)
                    .ButtonStyle(&GetHoverButtonStyle())
                    .ContentPadding(FMargin(6.0f, 3.0f))
                    .OnClicked_Lambda([this]()
                    {
                        ModelDropdown->SetIsOpen(true);
                        return FReply::Handled();
                    })
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        [
                            SAssignNew(ModelLabel, STextBlock)
                            .Text(FText::FromString(FCortexFrontendSettings::Get().GetSelectedModel()))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                            .ColorAndOpacity(FSlateColor(CortexColors::TextSecondary))
                        ]
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                        [
                            SAssignNew(EffortLabel, STextBlock)
                            .Text(FText::FromString(
                                FCortexFrontendSettings::Get().GetEffortLevel() == ECortexEffortLevel::Default
                                    ? TEXT("") : FCortexFrontendSettings::Get().GetEffortLevelString()))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                            .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                            .Visibility(FCortexFrontendSettings::Get().GetEffortLevel() == ECortexEffortLevel::Default
                                ? EVisibility::Collapsed : EVisibility::Visible)
                        ]
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("\u25BC")))
                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 6))
                            .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                        ]
                    ]
                ]
            ]
            // Spacer
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SSpacer)
            ]
            // Project Context toggle (right-aligned)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda([]()
                    {
                        return FCortexFrontendSettings::Get().GetProjectContext()
                            ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
                    {
                        FCortexFrontendSettings::Get().SetProjectContext(
                            NewState == ECheckBoxState::Checked);
                    })
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Project Context")))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                ]
            ]
            // Settings popup
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SAssignNew(SettingsPopup, SMenuAnchor)
                .Placement(MenuPlacement_AboveAnchor)
                .UseApplicationMenuStack(true)
                .OnGetMenuContent_Lambda([weakThis = TWeakPtr<SCortexInputArea>(SharedThis(this))]() -> TSharedRef<SWidget>
                {
                    TSharedPtr<SCortexInputArea> Self = weakThis.Pin();
                    if (!Self.IsValid()) { return SNullWidget::NullWidget; }

                    // Pre-build radio indicator widgets so they can be captured by value in
                    // OnClicked lambdas below. Avoids stale member handles between popup opens.
                    const bool bIsDirect = FCortexFrontendSettings::Get().GetWorkflowMode() == ECortexWorkflowMode::Direct;
                    TSharedPtr<STextBlock> LocalDirectRadio =
                        SNew(STextBlock)
                        .Text(FText::FromString(bIsDirect ? TEXT("\u25CF") : TEXT("\u25CB")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText));

                    TSharedPtr<STextBlock> LocalThoroughRadio =
                        SNew(STextBlock)
                        .Text(FText::FromString(bIsDirect ? TEXT("\u25CB") : TEXT("\u25CF")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                        .ColorAndOpacity(FSlateColor(CortexColors::ModeButtonText));

                    return SNew(SBorder)
                        .BorderImage(Self->DropdownBrush.Get())
                        .Padding(FMargin(12.0f, 8.0f))
                        [
                            SNew(SVerticalBox)
                            // Workflow Mode
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("WORKFLOW")))
                                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                                .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                            ]
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                [
                                    SNew(SButton)
                                    .ButtonStyle(&GetHoverButtonStyle())
                                    .ContentPadding(FMargin(10.0f, 4.0f))
                                    .OnClicked_Lambda([LocalDirectRadio, LocalThoroughRadio]()
                                    {
                                        FCortexFrontendSettings::Get().SetWorkflowMode(ECortexWorkflowMode::Direct);
                                        // Update radio indicators without closing popup (user may also want to edit directive)
                                        if (LocalDirectRadio.IsValid()) LocalDirectRadio->SetText(FText::FromString(TEXT("\u25CF")));
                                        if (LocalThoroughRadio.IsValid()) LocalThoroughRadio->SetText(FText::FromString(TEXT("\u25CB")));
                                        return FReply::Handled();
                                    })
                                    [
                                        SNew(SHorizontalBox)
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        .VAlign(VAlign_Center)
                                        .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                                        [
                                            LocalDirectRadio.ToSharedRef()
                                        ]
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        .VAlign(VAlign_Center)
                                        [
                                            SNew(STextBlock)
                                            .Text(FText::FromString(TEXT("Direct")))
                                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                            .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
                                        ]
                                    ]
                                ]
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                                [
                                    SNew(SButton)
                                    .ButtonStyle(&GetHoverButtonStyle())
                                    .ContentPadding(FMargin(10.0f, 4.0f))
                                    .OnClicked_Lambda([LocalDirectRadio, LocalThoroughRadio]()
                                    {
                                        FCortexFrontendSettings::Get().SetWorkflowMode(ECortexWorkflowMode::Thorough);
                                        if (LocalDirectRadio.IsValid()) LocalDirectRadio->SetText(FText::FromString(TEXT("\u25CB")));
                                        if (LocalThoroughRadio.IsValid()) LocalThoroughRadio->SetText(FText::FromString(TEXT("\u25CF")));
                                        return FReply::Handled();
                                    })
                                    [
                                        SNew(SHorizontalBox)
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        .VAlign(VAlign_Center)
                                        .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                                        [
                                            LocalThoroughRadio.ToSharedRef()
                                        ]
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        .VAlign(VAlign_Center)
                                        [
                                            SNew(STextBlock)
                                            .Text(FText::FromString(TEXT("Thorough")))
                                            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                            .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
                                        ]
                                    ]
                                ]
                            ]
                            // Separator
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(SBorder)
                                .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                                .BorderBackgroundColor(CortexColors::ToolBlockBorder)
                                [
                                    SNew(SBox).HeightOverride(1.0f)
                                ]
                            ]
                            // Custom Directive
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("CUSTOM DIRECTIVE")))
                                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                                .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                            ]
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            [
                                SNew(SBox)
                                .MinDesiredWidth(250.0f)
                                [
                                    SNew(SEditableTextBox)
                                    .Text(FText::FromString(FCortexFrontendSettings::Get().GetCustomDirective()))
                                    .HintText(FText::FromString(TEXT("Extra instructions for the AI...")))
                                    .OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type)
                                    {
                                        FCortexFrontendSettings::Get().SetCustomDirective(Text.ToString());
                                    })
                                ]
                            ]
                        ];
                })
                [
                    SNew(SButton)
                    .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                    .OnClicked_Lambda([this]()
                    {
                        SettingsPopup->SetIsOpen(true);
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("\u2699")))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
                        .ColorAndOpacity(FSlateColor(CortexColors::MutedTextColor))
                    ]
                ]
            ]
            // ActionButton (send/cancel) — circular blue button
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(8.0f, 0.0f, 0.0f, 0.0f)
            [
                SAssignNew(ActionButton, SButton)
                .ButtonStyle(FCoreStyle::Get(), "NoBorder")
                .ContentPadding(0.0f)
                .OnClicked(this, &SCortexInputArea::OnSendClicked)
                .OnHovered_Lambda([this]()
                {
                    if (ActionBorder.IsValid())
                    {
                        ActionBorder->SetBorderBackgroundColor(bIsStreaming
                            ? FLinearColor(1.0f, 0.35f, 0.35f, 0.9f)
                            : FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("2b96ff"))));
                    }
                })
                .OnUnhovered_Lambda([this]()
                {
                    if (ActionBorder.IsValid())
                    {
                        ActionBorder->SetBorderBackgroundColor(bIsStreaming
                            ? FLinearColor(0.94f, 0.27f, 0.27f, 0.8f)
                            : CortexColors::SendButtonColor);
                    }
                })
                [
                    SNew(SBox)
                    .WidthOverride(28.0f)
                    .HeightOverride(28.0f)
                    [
                        SAssignNew(ActionBorder, SBorder)
                        .HAlign(HAlign_Center)
                        .VAlign(VAlign_Center)
                        .BorderImage(SendBrush.Get())
                        .BorderBackgroundColor(CortexColors::SendButtonColor)
                        .Padding(0.0f)
                        [
                            SAssignNew(ActionIcon, STextBlock)
                            .Text(FText::FromString(TEXT("\u2191")))
                            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
                            .ColorAndOpacity(FSlateColor(FLinearColor::White))
                        ]
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
    if (ActionIcon.IsValid())
    {
        ActionIcon->SetText(FText::FromString(bStreaming ? TEXT("\u25A0") : TEXT("\u2191")));
    }
    if (ActionBorder.IsValid())
    {
        ActionBorder->SetBorderBackgroundColor(bStreaming
            ? FLinearColor(0.94f, 0.27f, 0.27f, 0.8f)  // Red tint for cancel
            : CortexColors::SendButtonColor);
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
