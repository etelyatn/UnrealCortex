#include "Widgets/SCortexChatMessage.h"

#include "Rendering/CortexChatMarshaller.h"
#include "Rendering/CortexFrontendColors.h"
#include "Rendering/CortexMarkdownParser.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCortexCodeBlock.h"

void SCortexChatMessage::Construct(const FArguments& InArgs)
{
    SetCanTick(true);

    bIsUser = InArgs._IsUser;
    bIsStreaming = InArgs._IsStreaming;

    PrefixChar = bIsUser ? TEXT(">") : TEXT("\u25CF");  // ● dot
    const FLinearColor PrefixColor = bIsUser
        ? CortexColors::UserPrefixColor
        : CortexColors::AssistantDotColor;

    TSharedRef<SHorizontalBox> MessageRow = SNew(SHorizontalBox)
        // Prefix (> or ●)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(FMargin(0.0f, 2.0f, 8.0f, 0.0f))
        [
            SNew(STextBlock)
            .Text(FText::FromString(PrefixChar))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
            .ColorAndOpacity(FSlateColor(PrefixColor))
        ]
        // Content
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        [
            SAssignNew(ContentBox, SVerticalBox)
        ];

    if (bIsUser)
    {
        // User messages: background highlight + left border
        ChildSlot
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor(CortexColors::UserRowBackground)
            .Padding(FMargin(8.0f, 5.0f))
            [
                SNew(SHorizontalBox)
                // Left accent border (2px)
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SBox)
                    .WidthOverride(2.0f)
                    [
                        SNew(SBorder)
                        .BorderBackgroundColor(CortexColors::UserRowBorderLeft)
                        [
                            SNullWidget::NullWidget
                        ]
                    ]
                ]
                // Content with prefix
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
                [
                    MessageRow
                ]
            ]
        ];
    }
    else
    {
        // Assistant messages: no background, just prefix + content
        ChildSlot
        [
            SNew(SBox)
            .Padding(FMargin(8.0f, 8.0f))
            [
                MessageRow
            ]
        ];
    }

    if (!InArgs._Message.IsEmpty())
    {
        SetText(InArgs._Message);
    }
}

TSharedRef<SWidget> SCortexChatMessage::BuildContentForText(const FString& Text)
{
    const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Text);
    TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

    for (const FCortexMarkdownBlock& Block : Blocks)
    {
        switch (Block.Type)
        {
        case ECortexMarkdownBlockType::Header:
        {
            const float FontSize = Block.HeaderLevel <= 1 ? 14.0f : (Block.HeaderLevel <= 2 ? 12.0f : 11.0f);
            const FString HeaderText = bIsUser ? Block.RawText : CortexMarkdownParser::ToRichText(Block.RawText);
            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 4.0f, 0.0f, 2.0f)
            [
                bIsUser
                ? static_cast<TSharedRef<SWidget>>(
                    SNew(STextBlock)
                    .Text(FText::FromString(HeaderText))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", static_cast<int32>(FontSize)))
                    .AutoWrapText(true))
                : static_cast<TSharedRef<SWidget>>(
                    SNew(SRichTextBlock)
                    .Text(FText::FromString(HeaderText))
                    .DecoratorStyleSet(&FCortexRichTextStyle::Get())
                    .WrapTextAt(TAttribute<float>::CreateSP(this, &SCortexChatMessage::GetWrapWidth)))
            ];
            break;
        }

        case ECortexMarkdownBlockType::CodeBlock:
        {
            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 4.0f)
            [
                SNew(SCortexCodeBlock)
                .Code(Block.RawText)
                .Language(Block.Language)
            ];
            break;
        }

        case ECortexMarkdownBlockType::UnorderedList:
        {
            TSharedRef<SVerticalBox> ListBox = SNew(SVerticalBox);
            for (const FString& Item : Block.ListItems)
            {
                const FString DisplayText = bIsUser
                    ? (TEXT("\u2022 ") + Item)
                    : (TEXT("\u2022 ") + CortexMarkdownParser::ToRichText(Item));
                TSharedRef<SWidget> ItemWidget = SNullWidget::NullWidget;
                if (bIsUser)
                {
                    ItemWidget = SNew(STextBlock)
                        .Text(FText::FromString(DisplayText))
                        .AutoWrapText(true);
                }
                else
                {
                    ItemWidget = SNew(SMultiLineEditableText)
                        .Marshaller(FCortexChatMarshaller::Create())
                        .Text(FText::FromString(DisplayText))
                        .IsReadOnly(true)
                        .WrapTextAt(TAttribute<float>::CreateSP(this, &SCortexChatMessage::GetWrapWidth));
                }

                ListBox->AddSlot()
                .AutoHeight()
                .Padding(0.0f, 1.0f)
                [
                    ItemWidget
                ];
            }
            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [ ListBox ];
            break;
        }

        case ECortexMarkdownBlockType::OrderedList:
        {
            TSharedRef<SVerticalBox> ListBox = SNew(SVerticalBox);
            for (int32 i = 0; i < Block.ListItems.Num(); ++i)
            {
                const FString DisplayText = bIsUser
                    ? (FString::Printf(TEXT("%d. "), i + 1) + Block.ListItems[i])
                    : (FString::Printf(TEXT("%d. "), i + 1) + CortexMarkdownParser::ToRichText(Block.ListItems[i]));
                TSharedRef<SWidget> ItemWidget = SNullWidget::NullWidget;
                if (bIsUser)
                {
                    ItemWidget = SNew(STextBlock)
                        .Text(FText::FromString(DisplayText))
                        .AutoWrapText(true);
                }
                else
                {
                    ItemWidget = SNew(SMultiLineEditableText)
                        .Marshaller(FCortexChatMarshaller::Create())
                        .Text(FText::FromString(DisplayText))
                        .IsReadOnly(true)
                        .WrapTextAt(TAttribute<float>::CreateSP(this, &SCortexChatMessage::GetWrapWidth));
                }

                ListBox->AddSlot()
                .AutoHeight()
                .Padding(0.0f, 1.0f)
                [
                    ItemWidget
                ];
            }
            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [ ListBox ];
            break;
        }

        case ECortexMarkdownBlockType::Paragraph:
        default:
        {
            const FString DisplayText = bIsUser
                ? Block.RawText
                : CortexMarkdownParser::ToRichText(Block.RawText);

            TSharedRef<SWidget> TextWidget = SNullWidget::NullWidget;
            if (bIsUser)
            {
                TextWidget = SNew(STextBlock)
                    .Text(FText::FromString(DisplayText))
                    .AutoWrapText(true);
            }
            else
            {
                TextWidget = SNew(SMultiLineEditableText)
                    .Marshaller(FCortexChatMarshaller::Create())
                    .Text(FText::FromString(DisplayText))
                    .IsReadOnly(true)
                    .WrapTextAt(TAttribute<float>::CreateSP(this, &SCortexChatMessage::GetWrapWidth));
            }

            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [
                TextWidget
            ];
            break;
        }
        }
    }

    return Box;
}

void SCortexChatMessage::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    // Subtract the 8px left+right padding from the content column to get usable text width.
    const float NewWidth = FMath::Max(100.0f, AllottedGeometry.GetLocalSize().X - 16.0f);
    WrapWidth = NewWidth;
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
}

void SCortexChatMessage::SetText(const FString& NewText)
{
    if (!ContentBox.IsValid())
    {
        return;
    }

    ContentBox->ClearChildren();

    if (NewText.IsEmpty())
    {
        return;
    }

    if (bIsStreaming)
    {
        // During streaming: single plain text block — no full markdown parse
        ContentBox->AddSlot()
        .AutoHeight()
        [
            SNew(STextBlock)
            .Text(FText::FromString(NewText))
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor(CortexColors::TextSecondary))
        ];
        return;
    }

    ContentBox->AddSlot()
    .AutoHeight()
    [
        BuildContentForText(NewText)
    ];
}
