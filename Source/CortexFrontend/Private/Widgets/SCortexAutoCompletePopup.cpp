#include "Widgets/SCortexAutoCompletePopup.h"

#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Rendering/CortexFrontendColors.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexAutoCompletePopup::Construct(const FArguments& InArgs)
{
    PopupBrush = MakeUnique<FSlateRoundedBoxBrush>(
        CortexColors::ToolBlockBackground, 6.0f,
        CortexColors::ToolBlockBorder, 1.0f);

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(PopupBrush.Get())
        .Padding(FMargin(0.0f, 4.0f))
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SBox)
                .MaxDesiredHeight(280.0f) // ~10 rows at 28px each
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SAssignNew(RowContainer, SVerticalBox)
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(FMargin(12.0f, 4.0f, 12.0f, 4.0f))
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Press Enter / Tab to insert, Esc to dismiss")))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
            ]
        ]
    ];
}

void SCortexAutoCompletePopup::Refresh(
    const TArray<TSharedPtr<FCortexAutoCompleteItem>>& Items,
    int32 SelectedIndex,
    int32 DividerAfterIndex)
{
    ItemCount = Items.Num();

    if (!RowContainer.IsValid()) return;
    RowContainer->ClearChildren();

    for (int32 i = 0; i < Items.Num(); ++i)
    {
        const TSharedPtr<FCortexAutoCompleteItem>& Item = Items[i];
        const bool bSelected = (i == SelectedIndex);
        const bool bIsProvider = (Item->Kind == ECortexAutoCompleteKind::ContextProvider);

        RowContainer->AddSlot()
        .AutoHeight()
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
            .BorderBackgroundColor(bSelected ? CortexColors::ToolBlockBorder : FLinearColor::Transparent)
            .Padding(FMargin(12.0f, 5.0f))
            .OnMouseButtonDown_Lambda([this, i](const FGeometry&, const FPointerEvent& Event) -> FReply
            {
                if (Event.GetEffectingButton() == EKeys::LeftMouseButton && OnItemSelected)
                {
                    OnItemSelected(i);
                    return FReply::Handled();
                }
                return FReply::Unhandled();
            })
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->Name))
                    .Font(FCoreStyle::GetDefaultFontStyle(bIsProvider ? "Bold" : "Regular", 10))
                    .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(FMargin(12.0f, 0.0f, 0.0f, 0.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->Description.Left(60) + (Item->Description.Len() > 60 ? TEXT("...") : TEXT(""))))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                ]
            ]
        ];

        // Insert divider after specified index
        if (i == DividerAfterIndex && i < Items.Num() - 1)
        {
            RowContainer->AddSlot()
            .AutoHeight()
            .Padding(FMargin(8.0f, 2.0f))
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
                .BorderBackgroundColor(CortexColors::ToolBlockBorder)
                .Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
            ];
        }
    }
}
