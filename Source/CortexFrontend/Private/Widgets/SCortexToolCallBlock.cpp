#include "Widgets/SCortexToolCallBlock.h"

#include "Rendering/CortexFrontendColors.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

FCortexFrontendToolCategory SCortexToolCallBlock::CategorizeToolCall(const FString& ToolName)
{
    if (ToolName == TEXT("Read") || ToolName == TEXT("read_file"))
    {
        return { TEXT("\u25CF"), CortexColors::IconRead, TEXT("Read") };
    }
    if (ToolName == TEXT("Glob") || ToolName == TEXT("Grep")
        || ToolName == TEXT("search") || ToolName == TEXT("find"))
    {
        return { TEXT("\u25C6"), CortexColors::IconSearch, TEXT("Search") };
    }
    if (ToolName == TEXT("Edit") || ToolName == TEXT("Write")
        || ToolName == TEXT("write_file") || ToolName == TEXT("edit"))
    {
        return { TEXT("\u25A0"), CortexColors::IconEdit, TEXT("Edit") };
    }
    if (ToolName == TEXT("Bash") || ToolName == TEXT("bash"))
    {
        return { TEXT("$"), CortexColors::IconShell, TEXT("Shell") };
    }
    if (ToolName.StartsWith(TEXT("mcp__cortex"))
        || ToolName.Contains(TEXT("blueprint."))
        || ToolName.Contains(TEXT("data.")))
    {
        return { TEXT("\u25B2"), CortexColors::IconMcp, TEXT("MCP") };
    }
    return { TEXT("\u25CB"), CortexColors::IconDefault, TEXT("Tool") };
}

void SCortexToolCallBlock::Construct(const FArguments& InArgs)
{
    ToolCallList = InArgs._ToolCalls;
    OnToggled = InArgs._OnToggled;

    ToolCallBrush = MakeUnique<FSlateRoundedBoxBrush>(
        CortexColors::ToolBlockBackground, 6.0f,
        CortexColors::ToolBlockBorder, 1.0f);

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(ToolCallBrush.Get())
        .Padding(0.0f)
        [
            SAssignNew(ContentBox, SVerticalBox)
        ]
    ];

    RebuildContent();
}

FReply SCortexToolCallBlock::OnToggleExpand()
{
    bIsExpanded = !bIsExpanded;
    RebuildContent();
    OnToggled.ExecuteIfBound();
    return FReply::Handled();
}

static FString FormatDuration(int32 Ms)
{
    if (Ms < 1000)
    {
        return FString::Printf(TEXT("%dms"), Ms);
    }
    if (Ms < 60000)
    {
        return FString::Printf(TEXT("%.1fs"), static_cast<float>(Ms) / 1000.0f);
    }
    const int32 Minutes = Ms / 60000;
    const float Seconds = (Ms % 60000) / 1000.0f;
    return FString::Printf(TEXT("%dm %.0fs"), Minutes, Seconds);
}

static FString ExtractFilePath(const FString& ToolInput)
{
    static const TArray<FString> Keys = {
        TEXT("\"file_path\":\""),
        TEXT("\"path\":\""),
        TEXT("\"pattern\":\"")
    };

    for (const FString& Key : Keys)
    {
        int32 KeyIdx = ToolInput.Find(Key, ESearchCase::CaseSensitive);
        if (KeyIdx == INDEX_NONE)
        {
            continue;
        }
        const int32 ValueStart = KeyIdx + Key.Len();
        int32 ValueEnd = ValueStart;
        while (ValueEnd < ToolInput.Len()
            && ToolInput[ValueEnd] != TEXT('"')
            && ToolInput[ValueEnd] != TEXT('\n'))
        {
            ++ValueEnd;
        }
        if (ValueEnd > ValueStart)
        {
            return ToolInput.Mid(ValueStart, ValueEnd - ValueStart);
        }
    }
    return FString();
}

void SCortexToolCallBlock::RebuildContent()
{
    ContentBox->ClearChildren();

    int32 TotalMs = 0;
    for (const TSharedPtr<FCortexChatEntry>& Call : ToolCallList)
    {
        TotalMs += Call->DurationMs;
    }

    const FString Arrow = bIsExpanded ? TEXT("\u25bc") : TEXT("\u25b6");
    const FString CountText = FString::Printf(TEXT("%d tool call%s"),
        ToolCallList.Num(),
        ToolCallList.Num() == 1 ? TEXT("") : TEXT("s"));
    const FString DurationText = FormatDuration(TotalMs);

    // Header row — clickable toggle
    ContentBox->AddSlot()
    .AutoHeight()
    [
        SNew(SButton)
        .ButtonStyle(FCoreStyle::Get(), "NoBorder")
        .OnClicked(this, &SCortexToolCallBlock::OnToggleExpand)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Arrow))
                .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(CountText))
                .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(DurationText))
                .ColorAndOpacity(FSlateColor(CortexColors::ToolDurationColor))
            ]
        ]
    ];

    if (bIsExpanded)
    {
        for (const TSharedPtr<FCortexChatEntry>& Call : ToolCallList)
        {
            const FCortexFrontendToolCategory Cat = CategorizeToolCall(Call->ToolName);

            FString Detail = ExtractFilePath(Call->ToolInput);
            if (Detail.IsEmpty())
            {
                Detail = Call->ToolName;
            }

            const FString DurText = FormatDuration(Call->DurationMs);

            ContentBox->AddSlot()
            .AutoHeight()
            .Padding(8.0f, 1.0f, 8.0f, 1.0f)
            [
                SNew(SHorizontalBox)
                // Icon
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.0f, 0.0f, 5.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Cat.Icon))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(Cat.Color))
                ]
                // Label
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.0f, 0.0f, 6.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Cat.Label))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::ToolLabelColor))
                ]
                // File / Detail
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                .Padding(0.0f, 0.0f, 6.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Detail))
                    .Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::ToolFileColor))
                    .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                ]
                // Duration
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(DurText))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                    .ColorAndOpacity(FSlateColor(CortexColors::ToolDurationColor))
                ]
            ];
        }
    }
}
