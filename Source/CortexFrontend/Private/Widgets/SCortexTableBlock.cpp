#include "Widgets/SCortexTableBlock.h"

#include "Rendering/CortexFrontendColors.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"

// ---------------------------------------------------------------------------
// ClassifyTableCell
// ---------------------------------------------------------------------------

ECortexTableCellType SCortexTableBlock::ClassifyTableCell(const FString& Value)
{
    const FString Upper = Value.ToUpper();

    // Badge_OK
    if (Upper == TEXT("PASS") || Upper == TEXT("SUCCESS") || Upper == TEXT("OK")
        || Upper == TEXT("YES") || Upper == TEXT("TRUE") || Upper == TEXT("DONE")
        || Upper == TEXT("COMPLETE") || Upper == TEXT("COMPILED"))
    {
        return ECortexTableCellType::Badge_OK;
    }

    // Badge_Error
    if (Upper == TEXT("FAIL") || Upper == TEXT("ERROR") || Upper == TEXT("NO")
        || Upper == TEXT("FALSE") || Upper == TEXT("MISSING") || Upper == TEXT("BROKEN"))
    {
        return ECortexTableCellType::Badge_Error;
    }

    // Badge_Warning
    if (Upper == TEXT("WARNING") || Upper == TEXT("WARN")
        || Upper == TEXT("DEPRECATED") || Upper == TEXT("PARTIAL"))
    {
        return ECortexTableCellType::Badge_Warning;
    }

    // Badge_Info
    if (Upper == TEXT("SKIP") || Upper == TEXT("INFO") || Upper == TEXT("N/A")
        || Upper == TEXT("NONE") || Upper == TEXT("PENDING"))
    {
        return ECortexTableCellType::Badge_Info;
    }

    // Progress — ends with "%" and prefix is a valid number
    if (Value.EndsWith(TEXT("%")))
    {
        const FString NumPart = Value.LeftChop(1);
        if (!NumPart.IsEmpty())
        {
            bool bIsNumeric = true;
            bool bHasDot = false;
            for (int32 i = 0; i < NumPart.Len(); ++i)
            {
                const TCHAR Ch = NumPart[i];
                if (Ch == TEXT('.'))
                {
                    if (bHasDot)
                    {
                        bIsNumeric = false;
                        break;
                    }
                    bHasDot = true;
                }
                else if (!FChar::IsDigit(Ch))
                {
                    bIsNumeric = false;
                    break;
                }
            }
            if (bIsNumeric)
            {
                return ECortexTableCellType::Progress;
            }
        }
    }

    // Mono — path/code patterns
    if (Value.StartsWith(TEXT("/")) || Value.Contains(TEXT("::"))
        || Value.Contains(TEXT(".cpp")) || Value.Contains(TEXT(".h"))
        || Value.Contains(TEXT(".uasset")))
    {
        return ECortexTableCellType::Mono;
    }

    return ECortexTableCellType::Plain;
}

// ---------------------------------------------------------------------------
// BuildCellWidget
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SCortexTableBlock::BuildCellWidget(const FString& Value) const
{
    const ECortexTableCellType CellType = ClassifyTableCell(Value);

    switch (CellType)
    {
    case ECortexTableCellType::Badge_OK:
    case ECortexTableCellType::Badge_Error:
    case ECortexTableCellType::Badge_Warning:
    case ECortexTableCellType::Badge_Info:
    {
        FLinearColor BgColor;
        FLinearColor TextColor;

        switch (CellType)
        {
        case ECortexTableCellType::Badge_OK:
            BgColor = CortexColors::BadgeOkBackground;
            TextColor = CortexColors::BadgeOkText;
            break;
        case ECortexTableCellType::Badge_Error:
            BgColor = CortexColors::BadgeErrorBackground;
            TextColor = CortexColors::BadgeErrorText;
            break;
        case ECortexTableCellType::Badge_Warning:
            BgColor = CortexColors::BadgeWarnBackground;
            TextColor = CortexColors::BadgeWarnText;
            break;
        default:  // Badge_Info
            BgColor = CortexColors::BadgeInfoBackground;
            TextColor = CortexColors::BadgeInfoText;
            break;
        }

        return SNew(SBox)
            .Padding(FMargin(2.0f, 1.0f))
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("RoundedSelection"))
                .BorderBackgroundColor(BgColor)
                .Padding(FMargin(5.0f, 1.0f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Value))
                    .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                    .ColorAndOpacity(FSlateColor(TextColor))
                ]
            ];
    }

    case ECortexTableCellType::Progress:
    {
        const FString NumPart = Value.LeftChop(1);
        const float Percent = FCString::Atof(*NumPart);
        const float ClampedPercent = FMath::Clamp(Percent, 0.0f, 100.0f);
        const float MaxBarWidth = 60.0f;
        const float BarWidth = ClampedPercent / 100.0f * MaxBarWidth;

        FLinearColor BarColor;
        if (ClampedPercent >= 75.0f)
        {
            BarColor = CortexColors::BadgeOkText;
        }
        else if (ClampedPercent >= 50.0f)
        {
            BarColor = CortexColors::BadgeWarnText;
        }
        else
        {
            BarColor = CortexColors::BadgeErrorText;
        }

        return SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
            [
                SNew(STextBlock)
                .Text(FText::FromString(Value))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SNew(SBox)
                .WidthOverride(BarWidth)
                .HeightOverride(4.0f)
                [
                    SNew(SBorder)
                    .BorderImage(FStyleDefaults::GetNoBrush())
                    .BorderBackgroundColor(BarColor)
                ]
            ];
    }

    case ECortexTableCellType::Mono:
        return SNew(STextBlock)
            .Text(FText::FromString(Value))
            .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
            .ColorAndOpacity(FSlateColor(CortexColors::MonoText));

    default:  // Plain
        return SNew(STextBlock)
            .Text(FText::FromString(Value))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
            .ColorAndOpacity(FSlateColor(CortexColors::TextPrimary));
    }
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------

void SCortexTableBlock::Construct(const FArguments& InArgs)
{
    const TArray<FString>& Headers = InArgs._Headers;
    const TArray<FCortexFrontendTableRowData>& AllRows = InArgs._Rows;

    const int32 TotalRows = AllRows.Num();
    const int32 DisplayCount = FMath::Min(TotalRows, 100);

    TableBrush = MakeUnique<FSlateRoundedBoxBrush>(
        CortexColors::TableBackground, 6.0f,
        CortexColors::TableBorder, 1.0f);

    TSharedRef<SVerticalBox> TableBody = SNew(SVerticalBox);

    // Title bar
    TableBody->AddSlot()
    .AutoHeight()
    .Padding(FMargin(8.0f, 6.0f, 8.0f, 4.0f))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
        [
            SNew(STextBlock)
            .Text(FText::FromString(TEXT("Table")))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
            .ColorAndOpacity(FSlateColor(CortexColors::TableHeaderText))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SBorder)
            .BorderImage(FStyleDefaults::GetNoBrush())
            .BorderBackgroundColor(CortexColors::CountBadgeBackground)
            .Padding(FMargin(5.0f, 1.0f))
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(TEXT("%d rows"), TotalRows)))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .ColorAndOpacity(FSlateColor(CortexColors::TableHeaderText))
            ]
        ]
    ];

    // Header row
    if (Headers.Num() > 0)
    {
        TSharedRef<SHorizontalBox> HeaderRow = SNew(SHorizontalBox);
        for (const FString& Header : Headers)
        {
            HeaderRow->AddSlot()
            .FillWidth(1.0f)
            .Padding(FMargin(6.0f, 3.0f))
            [
                SNew(STextBlock)
                .Text(FText::FromString(Header.ToUpper()))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
                .ColorAndOpacity(FSlateColor(CortexColors::TableHeaderText))
            ];
        }

        TableBody->AddSlot()
        .AutoHeight()
        [
            SNew(SBorder)
            .BorderImage(FStyleDefaults::GetNoBrush())
            .BorderBackgroundColor(CortexColors::TableHeaderBackground)
            .Padding(FMargin(0.0f))
            [
                HeaderRow
            ]
        ];
    }

    // Data rows
    for (int32 RowIdx = 0; RowIdx < DisplayCount; ++RowIdx)
    {
        const FCortexFrontendTableRowData& RowData = AllRows[RowIdx];

        TSharedRef<SHorizontalBox> DataRow = SNew(SHorizontalBox);
        for (const FString& CellValue : RowData)
        {
            DataRow->AddSlot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            .Padding(FMargin(6.0f, 2.0f))
            [
                BuildCellWidget(CellValue)
            ];
        }

        TableBody->AddSlot()
        .AutoHeight()
        [
            DataRow
        ];
    }

    // Truncation footer
    if (TotalRows > 100)
    {
        TableBody->AddSlot()
        .AutoHeight()
        .Padding(FMargin(8.0f, 4.0f))
        [
            SNew(STextBlock)
            .Text(FText::FromString(FString::Printf(TEXT("Showing 100 of %d rows"), TotalRows)))
            .Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
            .ColorAndOpacity(FSlateColor(CortexColors::TableHeaderText))
        ];
    }

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(TableBrush.Get())
        .Padding(FMargin(0.0f))
        [
            TableBody
        ]
    ];
}
