#include "Widgets/SCortexCodeBlock.h"

#include "CortexFrontendModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"
#include "Framework/Text/ITextLayoutMarshaller.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/TextLayout.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Rendering/CortexFrontendColors.h"
#include "Rendering/CortexSyntaxHighlighter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

// ---------------------------------------------------------------------------
// FCortexCodeMarshaller — applies syntax highlighting to a text layout.
// Private to this translation unit.
// ---------------------------------------------------------------------------

class FCortexCodeMarshaller : public FBaseTextLayoutMarshaller
{
public:
    static TSharedRef<FCortexCodeMarshaller> Create()
    {
        return MakeShareable(new FCortexCodeMarshaller());
    }

    // ITextLayoutMarshaller
    virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
    virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;

private:
    FCortexCodeMarshaller() = default;

    /** Return the linear color for a given token type. */
    static FLinearColor ColorForTokenType(ECortexSyntaxTokenType Type);
};

// ---------------------------------------------------------------------------

static FLinearColor ColorFromHex(const TCHAR* Hex)
{
    return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
}

FLinearColor FCortexCodeMarshaller::ColorForTokenType(ECortexSyntaxTokenType Type)
{
    switch (Type)
    {
        case ECortexSyntaxTokenType::Keyword:      return ColorFromHex(TEXT("569cd6"));
        case ECortexSyntaxTokenType::String:       return ColorFromHex(TEXT("ce9178"));
        case ECortexSyntaxTokenType::Comment:      return ColorFromHex(TEXT("6a9955"));
        case ECortexSyntaxTokenType::Preprocessor: return ColorFromHex(TEXT("c586c0"));
        case ECortexSyntaxTokenType::Number:       return ColorFromHex(TEXT("b5cea8"));
        case ECortexSyntaxTokenType::UEType:       return ColorFromHex(TEXT("4ec9b0"));
        case ECortexSyntaxTokenType::Function:     return ColorFromHex(TEXT("dcdcaa"));
        case ECortexSyntaxTokenType::Default:
        default:                                   return ColorFromHex(TEXT("cccccc"));
    }
}

void FCortexCodeMarshaller::SetText(const FString& SourceString, FTextLayout& TargetTextLayout)
{
    const FTextBlockStyle DefaultStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));

    // Split the source into per-line ranges so that each call to
    // TokenizeBlock aligns with the same line indices.
    TArray<FTextRange> LineRanges;
    FTextRange::CalculateLineRangesFromString(SourceString, LineRanges);

    // Tokenize the entire block at once.
    const TArray<TArray<FCortexSyntaxRun>> TokenLines =
        CortexSyntaxHighlighter::TokenizeBlock(SourceString);

    TArray<FTextLayout::FNewLineData> LinesToAdd;
    LinesToAdd.Reserve(LineRanges.Num());

    for (int32 LineIdx = 0; LineIdx < LineRanges.Num(); ++LineIdx)
    {
        const FTextRange& LineRange = LineRanges[LineIdx];

        // Shared pointer to the line's text slice (within the full source).
        TSharedRef<FString> LineText =
            MakeShareable(new FString(SourceString.Mid(LineRange.BeginIndex, LineRange.Len())));

        TArray<TSharedRef<IRun>> Runs;

        if (TokenLines.IsValidIndex(LineIdx) && !TokenLines[LineIdx].IsEmpty())
        {
            const TArray<FCortexSyntaxRun>& TokenRuns = TokenLines[LineIdx];

            int32 LastRunEnd = 0;
            for (const FCortexSyntaxRun& SyntaxRun : TokenRuns)
            {
                if (SyntaxRun.Text.IsEmpty())
                {
                    continue;
                }

                // Fill gap before this run if any (chars not covered by tokenizer).
                if (SyntaxRun.StartIndex > LastRunEnd)
                {
                    FTextBlockStyle GapStyle = DefaultStyle;
                    GapStyle.SetColorAndOpacity(
                        FSlateColor(ColorForTokenType(ECortexSyntaxTokenType::Default)));
                    Runs.Add(FSlateTextRun::Create(
                        FRunInfo(), LineText, GapStyle,
                        FTextRange(LastRunEnd, SyntaxRun.StartIndex)));
                }

                FTextBlockStyle TokenStyle = DefaultStyle;
                TokenStyle.SetColorAndOpacity(
                    FSlateColor(ColorForTokenType(SyntaxRun.Type)));

                const int32 RunStart = SyntaxRun.StartIndex;
                const int32 RunEnd   = RunStart + SyntaxRun.Text.Len();
                const FTextRange TokenRange(RunStart, RunEnd);

                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, TokenStyle, TokenRange));

                LastRunEnd = RunEnd;
            }

            // If tokenizer left trailing characters uncovered, fill with default style.
            if (LastRunEnd < LineText->Len())
            {
                FTextBlockStyle TrailingStyle = DefaultStyle;
                TrailingStyle.SetColorAndOpacity(
                    FSlateColor(ColorForTokenType(ECortexSyntaxTokenType::Default)));
                Runs.Add(FSlateTextRun::Create(
                    FRunInfo(), LineText, TrailingStyle,
                    FTextRange(LastRunEnd, LineText->Len())));
            }
        }

        // Fallback: render the whole line with default style.
        if (Runs.IsEmpty())
        {
            FTextBlockStyle FallbackStyle = DefaultStyle;
            FallbackStyle.SetColorAndOpacity(
                FSlateColor(ColorForTokenType(ECortexSyntaxTokenType::Default)));
            Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, FallbackStyle));
        }

        LinesToAdd.Emplace(MoveTemp(LineText), MoveTemp(Runs));
    }

    TargetTextLayout.AddLines(LinesToAdd);
}

void FCortexCodeMarshaller::GetText(FString& TargetString, const FTextLayout& SourceTextLayout)
{
    SourceTextLayout.GetAsText(TargetString);
}

// ---------------------------------------------------------------------------
// SCortexCodeBlock
// ---------------------------------------------------------------------------

void SCortexCodeBlock::Construct(const FArguments& InArgs)
{
    CodeContent = InArgs._Code;

    TSharedRef<FCortexCodeMarshaller> Marshaller = FCortexCodeMarshaller::Create();

    TSharedRef<SScrollBar> HScrollBar = SNew(SScrollBar).Orientation(Orient_Horizontal);
    TSharedRef<SScrollBar> VScrollBar = SNew(SScrollBar).Orientation(Orient_Vertical);

    CodeBlockBrush = MakeUnique<FSlateRoundedBoxBrush>(
        CortexColors::CodeBackground, 6.0f,
        CortexColors::CodeBorder, 1.0f);

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(CodeBlockBrush.Get())
        .Padding(0.0f)
        [
            SNew(SVerticalBox)
            // Language label + Copy button header
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                .BorderBackgroundColor(CortexColors::CodeHeaderBackground)
                .Padding(0.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .Padding(8.0f, 4.0f)
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(InArgs._Language.ToUpper()))
                        .Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
                        .ColorAndOpacity(FSlateColor(CortexColors::CodeLangColor))
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 2.0f)
                    .VAlign(VAlign_Center)
                    [
                        SNew(SButton)
                        .ButtonStyle(FAppStyle::Get(), TEXT("NoBorder"))
                        .OnClicked(this, &SCortexCodeBlock::OnCopyClicked)
                        .ContentPadding(FMargin(6.0f, 2.0f))
                        [
                            SNew(SBorder)
                            .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                            .BorderBackgroundColor(CortexColors::CodeButtonBorder)
                            .Padding(FMargin(6.0f, 2.0f))
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("Copy")))
                                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                                .ColorAndOpacity(FSlateColor(CortexColors::CodeLangColor))
                            ]
                        ]
                    ]
                ]
            ]
            // Code content with scrollbars — callers that need a height cap should wrap in SBox::MaxDesiredHeight
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush")))
                .BorderBackgroundColor(CortexColors::CodeBackground)
                .Padding(0.0f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .FillHeight(1.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        .Padding(8.0f, 4.0f, 0.0f, 4.0f)
                        [
                            SAssignNew(CodeTextWidget, SMultiLineEditableText)
                            .Text(FText::FromString(CodeContent))
                            .IsReadOnly(true)
                            .AutoWrapText(false)
                            .Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
                            .Marshaller(Marshaller)
                            .HScrollBar(HScrollBar)
                            .VScrollBar(VScrollBar)
                        ]
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        [
                            VScrollBar
                        ]
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        HScrollBar
                    ]
                ]
            ]
        ]
    ];
}

void SCortexCodeBlock::SetCode(const FString& NewCode)
{
    CodeContent = NewCode;

    if (CodeTextWidget.IsValid())
    {
        // SetText triggers the marshaller to re-tokenize and re-apply syntax highlighting.
        CodeTextWidget->SetText(FText::FromString(CodeContent));
    }
}

FReply SCortexCodeBlock::OnCopyClicked()
{
    FPlatformApplicationMisc::ClipboardCopy(*CodeContent);
    return FReply::Handled();
}
