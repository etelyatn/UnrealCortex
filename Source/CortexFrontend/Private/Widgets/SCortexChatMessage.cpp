#include "Widgets/SCortexChatMessage.h"

#include "Rendering/CortexMarkdownParser.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCortexCodeBlock.h"

void SCortexChatMessage::Construct(const FArguments& InArgs)
{
    bIsUser = InArgs._IsUser;
    bIsStreaming = InArgs._IsStreaming;

    const FLinearColor AccentColor = bIsUser
        ? FLinearColor(0.055f, 0.647f, 0.914f)  // #0ea5e9 user blue
        : FLinearColor(0.231f, 0.549f, 0.231f);  // #3b8c3b assistant green

    ChildSlot
    [
        SNew(SVerticalBox)
        // Role label
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 2.0f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(bIsUser ? TEXT("You") : TEXT("Claude")))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
            .ColorAndOpacity(FSlateColor(AccentColor))
        ]
        // Left border accent + content
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SHorizontalBox)
            // Left color accent border
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SBox)
                .WidthOverride(3.0f)
                [
                    SNew(SBorder)
                    .BorderBackgroundColor(AccentColor)
                    [
                        SNullWidget::NullWidget
                    ]
                ]
            ]
            // Message content
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .Padding(FMargin(8.0f, 4.0f))
            [
                SAssignNew(ContentBox, SVerticalBox)
            ]
        ]
    ];

    if (!InArgs._Message.IsEmpty())
    {
        SetText(InArgs._Message);
    }
}

TSharedRef<SWidget> SCortexChatMessage::BuildContentForText(const FString& Text) const
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
                SNew(STextBlock)
                .Text(FText::FromString(HeaderText))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", static_cast<int32>(FontSize)))
                .AutoWrapText(true)
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
                ListBox->AddSlot()
                .AutoHeight()
                .Padding(0.0f, 1.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(DisplayText))
                    .AutoWrapText(true)
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
                ListBox->AddSlot()
                .AutoHeight()
                .Padding(0.0f, 1.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(DisplayText))
                    .AutoWrapText(true)
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
            Box->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 2.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(DisplayText))
                .AutoWrapText(true)
            ];
            break;
        }
        }
    }

    return Box;
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
            .ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("cccccc")))))
        ];
        return;
    }

    ContentBox->AddSlot()
    .AutoHeight()
    [
        BuildContentForText(NewText)
    ];
}
