#include "Widgets/SCortexInputArea.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "CortexFrontendSettings.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "HAL/FileManager.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "IMessageLogListing.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/CortexFrontendColors.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Session/CortexSessionTypes.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/SCortexAutoCompletePopup.h"
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

    AutoCompletePopup = SNew(SCortexAutoCompletePopup);
    PopulateProviders();
    PopulateCoreCommands();

    // Defer skill/agent discovery one frame to avoid Game Thread I/O during construction
    DiscoveryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis = TWeakPtr<SCortexInputArea>(SharedThis(this))](float) -> bool
        {
            if (TSharedPtr<SCortexInputArea> Self = WeakThis.Pin())
            {
                Self->DiscoverSkillsAndAgents();
            }
            return false; // Fire once only
        }),
        0.0f);

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
        // Section 2: Textarea (wrapped in SMenuAnchor for autocomplete popup)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(FMargin(12.0f, 8.0f, 12.0f, 4.0f))
        [
            SAssignNew(AutoCompleteAnchor, SMenuAnchor)
            .Placement(MenuPlacement_AboveAnchor)
            .UseApplicationMenuStack(false) // Popup never steals focus; dismiss handled by OnFocusLost
            .OnGetMenuContent_Lambda([PopupWidget = AutoCompletePopup]() -> TSharedRef<SWidget>
            {
                return PopupWidget.IsValid() ? PopupWidget.ToSharedRef() : SNullWidget::NullWidget;
            })
            [
                SNew(SBox)
                .MinDesiredHeight(44.0f)
                [
                    SAssignNew(InputTextBox, SMultiLineEditableTextBox)
                    .HintText(FText::FromString(TEXT("@ for context or ask anything...")))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
                    .AutoWrapText(true)
                    .OnTextChanged_Lambda([this](const FText& NewText)
                    {
                        HandleTextChanged(NewText);
                    })
                    .OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent) -> FReply
                    {
                        if (bAutoCompleteOpen)
                        {
                            if (KeyEvent.GetKey() == EKeys::Up)
                            {
                                AutoCompleteSelectedIndex = FMath::Max(0, AutoCompleteSelectedIndex - 1);
                                if (AutoCompletePopup.IsValid())
                                {
                                    AutoCompletePopup->Refresh(FilteredItems, AutoCompleteSelectedIndex,
                                        ActiveTrigger == TEXT('@') ? ProviderItems.Num() - 1 : INDEX_NONE);
                                }
                                return FReply::Handled();
                            }
                            if (KeyEvent.GetKey() == EKeys::Down)
                            {
                                AutoCompleteSelectedIndex = FMath::Min(FilteredItems.Num() - 1, AutoCompleteSelectedIndex + 1);
                                if (AutoCompletePopup.IsValid())
                                {
                                    AutoCompletePopup->Refresh(FilteredItems, AutoCompleteSelectedIndex,
                                        ActiveTrigger == TEXT('@') ? ProviderItems.Num() - 1 : INDEX_NONE);
                                }
                                return FReply::Handled();
                            }
                            if (KeyEvent.GetKey() == EKeys::Enter || KeyEvent.GetKey() == EKeys::Tab)
                            {
                                CommitSelection();
                                return FReply::Handled();
                            }
                            if (KeyEvent.GetKey() == EKeys::Escape)
                            {
                                ClosePopup();
                                return FReply::Handled();
                            }
                        }
                        if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown() && !bAutoCompleteOpen)
                        {
                            HandleSendOrNewline();
                            return FReply::Handled();
                        }
                        return FReply::Unhandled();
                    })
                ]
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
                    .ToolTipText(FText::FromString(TEXT("Permission level for file access and tool use")))
                    .OnClicked_Lambda([this]()
                    {
                        ModeDropdown->SetIsOpen(!ModeDropdown->IsOpen());
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
                    .ToolTipText(FText::FromString(TEXT("Select AI model and effort level")))
                    .OnClicked_Lambda([this]()
                    {
                        ModelDropdown->SetIsOpen(!ModelDropdown->IsOpen());
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
                                .ToolTipText(FText::FromString(TEXT("Controls how the AI approaches tasks")))
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
                                    .ToolTipText(FText::FromString(TEXT("Execute tasks immediately without extra planning")))
                                    .OnClicked_Lambda([LocalDirectRadio, LocalThoroughRadio]()
                                    {
                                        FCortexFrontendSettings::Get().SetWorkflowMode(ECortexWorkflowMode::Direct);
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
                                    .ToolTipText(FText::FromString(TEXT("Plan and review before executing — better for complex tasks")))
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
                            // Project Context
                            + SVerticalBox::Slot()
                            .AutoHeight()
                            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot()
                                .AutoWidth()
                                .VAlign(VAlign_Center)
                                [
                                    SNew(SCheckBox)
                                    .ToolTipText(FText::FromString(TEXT("Include CLAUDE.md and project context in every message")))
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
                                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
                                    .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
                                    .ToolTipText(FText::FromString(TEXT("Include CLAUDE.md and project context in every message")))
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
                                .ToolTipText(FText::FromString(TEXT("Extra instructions appended to every message")))
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
                                    .ToolTipText(FText::FromString(TEXT("Text here is appended to every message you send")))
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
                    .ToolTipText(FText::FromString(TEXT("Workflow, project context, and custom directive settings")))
                    .OnClicked_Lambda([this]()
                    {
                        SettingsPopup->SetIsOpen(!SettingsPopup->IsOpen());
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
    if (bIsStreaming || !InputTextBox.IsValid()) return;

    FString Text = InputTextBox->GetText().ToString();
    Text.TrimStartAndEndInline();
    if (Text.IsEmpty()) return;

    // Lock input immediately — prevents double-send
    bIsStreaming = true;
    SetInputEnabled(false);

    // Capture state before clearing
    TArray<FCortexContextChip> ChipsToResolve = ContextItems;
    const FString RawMessage = Text;
    ClearContextChips();
    InputTextBox->SetText(FText::GetEmpty());
    PreviousText = FText::GetEmpty();

    // Defer resolution to next Game Thread tick so we don't block inside the key-down event handler
    AsyncTask(ENamedThreads::GameThread,
        [WeakThis = TWeakPtr<SCortexInputArea>(SharedThis(this)),
         ChipsToResolve,
         RawMessage]()
    {
        TSharedPtr<SCortexInputArea> Self = WeakThis.Pin();
        if (!Self.IsValid()) return;
        Self->ResolveAndSend(ChipsToResolve, RawMessage);
    });
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

void SCortexInputArea::AddContextChip(const FCortexContextChip& Chip)
{
    ContextItems.Add(Chip);
    RebuildChips();
}

void SCortexInputArea::RemoveContextChip(int32 Index)
{
    if (ContextItems.IsValidIndex(Index))
    {
        ContextItems.RemoveAt(Index);
        RebuildChips();
    }
}

void SCortexInputArea::ClearContextChips()
{
    ContextItems.Empty();
    RebuildChips();
}

const TArray<FCortexContextChip>& SCortexInputArea::GetContextChips() const
{
    return ContextItems;
}

void SCortexInputArea::RebuildChips()
{
    if (!ChipRow.IsValid()) return;
    ChipRow->ClearChildren();

    for (int32 i = 0; i < ContextItems.Num(); ++i)
    {
        const FCortexContextChip& Chip = ContextItems[i];
        const FString DisplayLabel = Chip.Kind == ECortexContextChipKind::Provider
            ? FString::Printf(TEXT("@%s"), *Chip.Label)
            : FPaths::GetCleanFilename(Chip.Label);

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
                    .Text(FText::FromString(DisplayLabel))
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
                        RemoveContextChip(ChipIndex);
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

SCortexInputArea::~SCortexInputArea()
{
    if (DiscoveryTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DiscoveryTickerHandle);
    }
    if (AssetLoadedDelegateHandle.IsValid())
    {
        if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
        {
            IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            AssetRegistry.OnFilesLoaded().Remove(AssetLoadedDelegateHandle);
        }
    }
}

void SCortexInputArea::HandleTextChanged(const FText& NewText)
{
    const FString NewStr = NewText.ToString();
    const FString OldStr = PreviousText.ToString();
    PreviousText = NewText;

    // Only process single-character insertions (guards against paste, IME, select-all+type)
    const int32 LenDiff = NewStr.Len() - OldStr.Len();

    if (bAutoCompleteOpen)
    {
        // Check if trigger character still present at TriggerOffset
        if (!NewStr.IsValidIndex(TriggerOffset) || NewStr[TriggerOffset] != ActiveTrigger)
        {
            ClosePopup();
            return;
        }
        // Re-filter with updated query (everything after trigger char)
        FilterItems(NewStr.Mid(TriggerOffset + 1));
        return;
    }

    // Must be a single-char insertion to trigger
    if (LenDiff != 1) return;

    // Find insertion position: first character that differs
    int32 InsertPos = 0;
    while (InsertPos < OldStr.Len() && OldStr[InsertPos] == NewStr[InsertPos])
    {
        ++InsertPos;
    }
    const TCHAR InsertedChar = NewStr[InsertPos];

    if (InsertedChar == TEXT('@'))
    {
        TriggerOffset = InsertPos;
        ActiveTrigger = TEXT('@');
        LoadAssetCache(); // lazy load — no-op if already populated
        OpenPopup();
        FilterItems(TEXT(""));
    }
    else if (InsertedChar == TEXT('/') && InsertPos == 0)
    {
        TriggerOffset = 0;
        ActiveTrigger = TEXT('/');
        OpenPopup();
        FilterItems(TEXT(""));
    }
}

bool SCortexInputArea::IsAutoCompleteOpen() const
{
    return bAutoCompleteOpen;
}

void SCortexInputArea::OpenPopup()
{
    bAutoCompleteOpen = true;
    if (AutoCompleteAnchor.IsValid())
    {
        AutoCompleteAnchor->SetIsOpen(true);
    }
}

void SCortexInputArea::ClosePopup()
{
    bAutoCompleteOpen = false;
    TriggerOffset = INDEX_NONE;
    ActiveTrigger = TEXT('\0');
    FilteredItems.Reset();
    AutoCompleteSelectedIndex = 0;
    if (AutoCompleteAnchor.IsValid())
    {
        AutoCompleteAnchor->SetIsOpen(false);
    }
    if (AutoCompletePopup.IsValid())
    {
        AutoCompletePopup->Refresh({}, INDEX_NONE, INDEX_NONE);
    }
}
void SCortexInputArea::PopulateProviders()
{
    auto MakeProvider = [](const FString& Name, const FString& Desc) -> TSharedPtr<FCortexAutoCompleteItem>
    {
        auto Item = MakeShared<FCortexAutoCompleteItem>();
        Item->Name = Name;
        Item->Description = Desc;
        Item->Kind = ECortexAutoCompleteKind::ContextProvider;
        return Item;
    };

    ProviderItems.Reset();
    ProviderItems.Add(MakeProvider(TEXT("thisAsset"), TEXT("Refer to currently open asset")));
    ProviderItems.Add(MakeProvider(TEXT("selection"), TEXT("Refer to selected actors")));
    ProviderItems.Add(MakeProvider(TEXT("problems"), TEXT("Refer to current problems")));
}

void SCortexInputArea::FilterItems(const FString& Query)
{
    FilteredItems.Reset();
    AutoCompleteSelectedIndex = 0;
    int32 DividerAfterIndex = INDEX_NONE;

    if (ActiveTrigger == TEXT('@'))
    {
        // Filter providers by query
        for (const TSharedPtr<FCortexAutoCompleteItem>& Item : ProviderItems)
        {
            if (Query.IsEmpty() || CortexAutoComplete::FilterAndScore(Query, Item->Name) > 0)
            {
                FilteredItems.Add(Item);
            }
        }
        const int32 NumProviders = FilteredItems.Num();
        if (NumProviders > 0) DividerAfterIndex = NumProviders - 1;

        // Add fuzzy-matched assets (up to 10 total)
        if (!Query.IsEmpty())
        {
            TArray<TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>> Scored;
            for (const TSharedPtr<FCortexAutoCompleteItem>& Asset : AssetCache)
            {
                const int32 Score = CortexAutoComplete::FilterAndScore(Query, Asset->Name);
                if (Score > 0) Scored.Add({Score, Asset});
            }
            Scored.Sort([](const TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>& A,
                          const TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>& B)
            {
                return A.Key > B.Key;
            });
            const int32 MaxTotal = 10;
            for (int32 i = 0; i < Scored.Num() && FilteredItems.Num() < MaxTotal; ++i)
            {
                FilteredItems.Add(Scored[i].Value);
            }
        }
        else if (bAssetCacheLoading)
        {
            auto Loading = MakeShared<FCortexAutoCompleteItem>();
            Loading->Name = TEXT("Scanning assets...");
            Loading->Description = TEXT("");
            Loading->Kind = ECortexAutoCompleteKind::Asset;
            FilteredItems.Add(Loading);
        }
    }
    else if (ActiveTrigger == TEXT('/'))
    {
        TArray<TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>> Scored;
        for (const TSharedPtr<FCortexAutoCompleteItem>& Cmd : CommandCache)
        {
            const int32 Score = CortexAutoComplete::FilterAndScore(Query, Cmd->Name);
            if (Score > 0) Scored.Add({Score, Cmd});
        }
        Scored.Sort([](const TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>& A,
                      const TPair<int32, TSharedPtr<FCortexAutoCompleteItem>>& B)
        {
            return A.Key > B.Key;
        });
        for (int32 i = 0; i < FMath::Min(Scored.Num(), 10); ++i)
        {
            FilteredItems.Add(Scored[i].Value);
        }
    }

    if (AutoCompletePopup.IsValid())
    {
        AutoCompletePopup->Refresh(FilteredItems, AutoCompleteSelectedIndex, DividerAfterIndex);
    }
}

void SCortexInputArea::CommitSelection()
{
    if (!FilteredItems.IsValidIndex(AutoCompleteSelectedIndex)) return;
    const TSharedPtr<FCortexAutoCompleteItem>& Selected = FilteredItems[AutoCompleteSelectedIndex];

    if (ActiveTrigger == TEXT('@'))
    {
        FCortexContextChip Chip;
        Chip.Label = Selected->Name;
        if (Selected->Kind == ECortexAutoCompleteKind::ContextProvider)
        {
            Chip.Kind = ECortexContextChipKind::Provider;
        }
        else
        {
            Chip.Kind = ECortexContextChipKind::Asset;
            Chip.AssetClass = Selected->AssetClass;
            Chip.RouterCommand = Selected->RouterCommand;
            Chip.Label = Selected->FullPath;
        }
        AddContextChip(Chip);

        // Remove @query from input text
        if (InputTextBox.IsValid())
        {
            const FString Current = InputTextBox->GetText().ToString();
            const FString Cleared = Current.Left(TriggerOffset);
            InputTextBox->SetText(FText::FromString(Cleared));
            PreviousText = FText::FromString(Cleared);
        }
    }
    else if (ActiveTrigger == TEXT('/'))
    {
        // Replace input with "/CommandName " ready for user's prompt
        if (InputTextBox.IsValid())
        {
            const FString NewText = TEXT("/") + Selected->Name + TEXT(" ");
            InputTextBox->SetText(FText::FromString(NewText));
            PreviousText = FText::FromString(NewText);
        }
    }

    ClosePopup();
}

void SCortexInputArea::LoadAssetCache()
{
    if (!AssetCache.IsEmpty()) return; // Already loaded

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    if (AssetRegistry.IsLoadingAssets())
    {
        bAssetCacheLoading = true;
        AssetLoadedDelegateHandle = AssetRegistry.OnFilesLoaded().AddLambda([WeakThis = TWeakPtr<SCortexInputArea>(SharedThis(this))]()
        {
            if (TSharedPtr<SCortexInputArea> Self = WeakThis.Pin())
            {
                Self->bAssetCacheLoading = false;
                Self->LoadAssetCache();
                if (Self->bAutoCompleteOpen && Self->ActiveTrigger == TEXT('@'))
                {
                    const FString Query = Self->PreviousText.ToString().Mid(Self->TriggerOffset + 1);
                    Self->FilterItems(Query);
                }
            }
        });
        return;
    }

    // Router command mapping by Asset Registry class name
    auto GetRouterCommand = [](const FString& ClassName) -> FString
    {
        if (ClassName == TEXT("Blueprint"))                 return TEXT("blueprint.get_blueprint");
        if (ClassName == TEXT("WidgetBlueprint"))           return TEXT("umg.get_widget");
        if (ClassName == TEXT("DataTable"))                 return TEXT("data.get_datatable");
        if (ClassName == TEXT("Material"))                  return TEXT("material.get_material");
        if (ClassName == TEXT("MaterialInstanceConstant"))  return TEXT("material.get_material");
        if (ClassName == TEXT("MaterialInterface"))         return TEXT("material.get_material");
        return TEXT(""); // Unknown — fall back to raw @path
    };

    AssetRegistry.EnumerateAllAssets([this, &GetRouterCommand](const FAssetData& AssetData)
    {
        if (!AssetData.PackagePath.ToString().StartsWith(TEXT("/Game"))) return true;

        const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
        const FString PackagePath = AssetData.PackagePath.ToString();
        const FString RelFolder = PackagePath.Mid(6); // strip "/Game/"

        auto Item = MakeShared<FCortexAutoCompleteItem>();
        Item->Name = AssetData.AssetName.ToString();
        Item->Description = FString::Printf(TEXT("%s · %s"), *ClassName, *RelFolder);
        Item->FullPath = AssetData.GetObjectPathString();
        Item->RouterCommand = GetRouterCommand(ClassName);
        Item->AssetClass = ClassName;
        Item->Kind = ECortexAutoCompleteKind::Asset;
        AssetCache.Add(Item);
        return true; // Continue enumeration
    }, UE::AssetRegistry::EEnumerateAssetsFlags::OnlyOnDiskAssets);
}

void SCortexInputArea::PopulateCoreCommands()
{
    auto MakeCmd = [](const FString& Name, const FString& Desc, ECortexAutoCompleteKind Kind) -> TSharedPtr<FCortexAutoCompleteItem>
    {
        auto Item = MakeShared<FCortexAutoCompleteItem>();
        Item->Name = Name;
        Item->Description = Desc;
        Item->Kind = Kind;
        return Item;
    };

    CommandCache.Reset();
    CommandCache.Add(MakeCmd(TEXT("help"),    TEXT("Get help with Claude Code"),    ECortexAutoCompleteKind::CoreCommand));
    CommandCache.Add(MakeCmd(TEXT("clear"),   TEXT("Clear conversation history"),  ECortexAutoCompleteKind::CoreCommand));
    CommandCache.Add(MakeCmd(TEXT("compact"), TEXT("Compact conversation context"), ECortexAutoCompleteKind::CoreCommand));
}

FString SCortexInputArea::ParseFrontmatterField(const FString& FileContent, const FString& FieldName)
{
    // Find text between first and second --- delimiters
    const int32 FirstDelim = FileContent.Find(TEXT("---"));
    if (FirstDelim == INDEX_NONE) return TEXT("");
    const int32 ContentStart = FirstDelim + 3;
    const int32 SecondDelim = FileContent.Find(TEXT("---"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);
    if (SecondDelim == INDEX_NONE) return TEXT("");
    const FString Frontmatter = FileContent.Mid(ContentStart, SecondDelim - ContentStart);

    // Line-by-line parse: find "FieldName: value"
    TArray<FString> Lines;
    Frontmatter.ParseIntoArrayLines(Lines);
    const FString Prefix = FieldName + TEXT(":");
    for (const FString& Line : Lines)
    {
        if (Line.StartsWith(Prefix))
        {
            FString Value = Line.Mid(Prefix.Len());
            Value.TrimStartAndEndInline();
            return Value;
        }
    }
    return TEXT("");
}

void SCortexInputArea::DiscoverSkillsAndAgents()
{
    const FString ToolkitRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectDir() / TEXT("cortex-toolkit"));

    auto ScanDir = [this](const FString& DirPath, ECortexAutoCompleteKind Kind)
    {
        if (!FPaths::DirectoryExists(DirPath)) return;

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *DirPath, TEXT("*.md"), true, false);

        for (const FString& FilePath : Files)
        {
            FString Content;
            if (!FFileHelper::LoadFileToString(Content, *FilePath)) continue;

            FString Name = ParseFrontmatterField(Content, TEXT("name"));
            FString Desc = ParseFrontmatterField(Content, TEXT("description"));
            if (Name.IsEmpty()) continue;

            auto Item = MakeShared<FCortexAutoCompleteItem>();
            Item->Name = Name;
            Item->Description = Desc.IsEmpty() ? FilePath : Desc;
            Item->Kind = Kind;
            CommandCache.Add(Item);
        }
    };

    ScanDir(ToolkitRoot / TEXT("skills"), ECortexAutoCompleteKind::Skill);
    ScanDir(ToolkitRoot / TEXT("agents"), ECortexAutoCompleteKind::Agent);
}

void SCortexInputArea::ResolveAndSend(const TArray<FCortexContextChip>& Chips, const FString& Message)
{
    FString Preamble;

    for (const FCortexContextChip& Chip : Chips)
    {
        if (Chip.Kind == ECortexContextChipKind::Provider)
        {
            const FString Resolved = ResolveProviderChip(Chip.Label);
            if (!Resolved.IsEmpty())
            {
                Preamble += FString::Printf(TEXT("## Context: %s\n%s\n\n"), *Chip.Label, *Resolved);
            }
            // Empty resolution: silent drop — no section header emitted
        }
        else if (Chip.Kind == ECortexContextChipKind::Asset)
        {
            bool bSuccess = false;
            const FString Resolved = ResolveAssetChip(Chip, bSuccess);
            if (bSuccess)
            {
                Preamble += FString::Printf(TEXT("## Context: %s\n%s\n\n"), *Chip.Label, *Resolved);
            }
            else
            {
                // Fall back: prepend raw @path text
                Preamble += FString::Printf(TEXT("@%s\n"), *Chip.Label);
            }
        }
        else // RawText
        {
            Preamble += FString::Printf(TEXT("@%s\n"), *Chip.Label);
        }
    }

    OnSendMessage.ExecuteIfBound(Preamble + Message);
}

FString SCortexInputArea::ResolveProviderChip(const FString& Label)
{
    if (Label == TEXT("thisAsset"))
    {
        if (!GEditor) return TEXT("");
        UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (!Subsystem) return TEXT("");
        TArray<UObject*> EditedAssets = Subsystem->GetAllEditedAssets();
        if (EditedAssets.IsEmpty()) return TEXT("");

        FString Result;
        for (UObject* Asset : EditedAssets)
        {
            if (Asset)
            {
                Result += FString::Printf(TEXT("- %s (%s)\n"),
                    *Asset->GetName(), *Asset->GetClass()->GetName());
            }
        }
        return Result;
    }
    else if (Label == TEXT("selection"))
    {
        if (!GEditor) return TEXT("");
        USelection* Selection = GEditor->GetSelectedActors();
        if (!Selection || Selection->Num() == 0) return TEXT("");

        FString Result;
        for (FSelectionIterator It(*Selection); It; ++It)
        {
            if (AActor* Actor = Cast<AActor>(*It))
            {
                Result += FString::Printf(TEXT("- %s (%s) at %s\n"),
                    *Actor->GetActorLabel(),
                    *Actor->GetClass()->GetName(),
                    *Actor->GetActorLocation().ToString());
            }
        }
        return Result;
    }
    else if (Label == TEXT("problems"))
    {
        FMessageLogModule* LogModule = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog"));
        if (!LogModule) return TEXT("");

        TSharedRef<IMessageLogListing> Listing = LogModule->GetLogListing(TEXT("BlueprintLog"));
        const TArray<TSharedRef<FTokenizedMessage>>& AllMessages = Listing->GetFilteredMessages();
        if (AllMessages.IsEmpty()) return TEXT("");

        FString Result;
        for (const TSharedRef<FTokenizedMessage>& Msg : AllMessages)
        {
            const EMessageSeverity::Type Severity = Msg->GetSeverity();
            if (Severity == EMessageSeverity::Error)
            {
                Result += FString::Printf(TEXT("ERROR: %s\n"), *Msg->ToText().ToString());
            }
            else if (Severity == EMessageSeverity::Warning)
            {
                Result += FString::Printf(TEXT("WARNING: %s\n"), *Msg->ToText().ToString());
            }
        }
        return Result;
    }
    return TEXT("");
}

FString SCortexInputArea::ResolveAssetChip(const FCortexContextChip& Chip, bool& bOutSuccess)
{
    bOutSuccess = false;
    if (Chip.RouterCommand.IsEmpty()) return TEXT("");
    // Router integration deferred — see tech-debt
    return TEXT("");
}

FReply SCortexInputArea::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    // Autocomplete navigation duplicated here because:
    // 1. OnKeyDownHandler_Lambda handles keys when the inner SMultiLineEditableTextBox has focus (normal usage)
    // 2. This override handles keys when tests call OnKeyDown directly on the compound widget
    if (bAutoCompleteOpen)
    {
        if (InKeyEvent.GetKey() == EKeys::Up)
        {
            AutoCompleteSelectedIndex = FMath::Max(0, AutoCompleteSelectedIndex - 1);
            if (AutoCompletePopup.IsValid())
            {
                AutoCompletePopup->Refresh(FilteredItems, AutoCompleteSelectedIndex,
                    ActiveTrigger == TEXT('@') ? ProviderItems.Num() - 1 : INDEX_NONE);
            }
            return FReply::Handled();
        }
        if (InKeyEvent.GetKey() == EKeys::Down)
        {
            AutoCompleteSelectedIndex = FMath::Min(FilteredItems.Num() - 1, AutoCompleteSelectedIndex + 1);
            if (AutoCompletePopup.IsValid())
            {
                AutoCompletePopup->Refresh(FilteredItems, AutoCompleteSelectedIndex,
                    ActiveTrigger == TEXT('@') ? ProviderItems.Num() - 1 : INDEX_NONE);
            }
            return FReply::Handled();
        }
        if (InKeyEvent.GetKey() == EKeys::Enter || InKeyEvent.GetKey() == EKeys::Tab)
        {
            CommitSelection();
            return FReply::Handled();
        }
        if (InKeyEvent.GetKey() == EKeys::Escape)
        {
            ClosePopup();
            return FReply::Handled();
        }
    }
    if (InputTextBox.IsValid())
    {
        return InputTextBox->OnKeyDown(MyGeometry, InKeyEvent);
    }
    return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SCortexInputArea::OnFocusLost(const FFocusEvent& InFocusEvent)
{
    SCompoundWidget::OnFocusLost(InFocusEvent);
    if (bAutoCompleteOpen)
    {
        ClosePopup();
    }
}
