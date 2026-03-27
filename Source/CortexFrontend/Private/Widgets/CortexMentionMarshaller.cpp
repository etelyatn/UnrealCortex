#include "Widgets/CortexMentionMarshaller.h"

#include "Framework/Text/SlateTextRun.h"

TSharedRef<FCortexMentionMarshaller> FCortexMentionMarshaller::Create(const FSlateFontInfo& InFont)
{
    return MakeShared<FCortexMentionMarshaller>(InFont);
}

FCortexMentionMarshaller::FCortexMentionMarshaller(const FSlateFontInfo& InFont)
{
    NormalStyle = FTextBlockStyle::GetDefault();
    NormalStyle.SetFont(InFont);
    NormalStyle.SetColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f));

    MentionStyle = NormalStyle;
    MentionStyle.SetColorAndOpacity(FLinearColor(0.4f, 0.7f, 1.0f)); // Light blue
}

void FCortexMentionMarshaller::SetText(const FString& SourceString, FTextLayout& TargetTextLayout)
{
    TArray<FTextLayout::FNewLineData> LinesToAdd;

    TArray<FString> Lines;
    SourceString.ParseIntoArrayLines(Lines, false);
    if (Lines.IsEmpty()) Lines.Add(TEXT(""));

    for (const FString& LineStr : Lines)
    {
        TSharedRef<FString> LineText = MakeShared<FString>(LineStr);
        TArray<TSharedRef<IRun>> Runs;

        int32 Pos = 0;
        const int32 Len = LineStr.Len();

        while (Pos < Len)
        {
            // Find next '@'
            int32 AtPos = INDEX_NONE;
            for (int32 i = Pos; i < Len; ++i)
            {
                if (LineStr[i] == TEXT('@'))
                {
                    AtPos = i;
                    break;
                }
            }

            if (AtPos == INDEX_NONE)
            {
                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, NormalStyle, FTextRange(Pos, Len)));
                break;
            }

            // Normal text before '@'
            if (AtPos > Pos)
            {
                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, NormalStyle, FTextRange(Pos, AtPos)));
            }

            // Mention span: '@' + non-whitespace chars
            int32 MentionEnd = AtPos + 1;
            while (MentionEnd < Len && !FChar::IsWhitespace(LineStr[MentionEnd]))
            {
                ++MentionEnd;
            }

            if (MentionEnd > AtPos + 1)
            {
                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, MentionStyle, FTextRange(AtPos, MentionEnd)));
            }
            else
            {
                // Lone '@' with no word — normal style
                Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, NormalStyle, FTextRange(AtPos, AtPos + 1)));
            }

            Pos = MentionEnd;
        }

        if (Runs.IsEmpty())
        {
            Runs.Add(FSlateTextRun::Create(FRunInfo(), LineText, NormalStyle, FTextRange(0, 0)));
        }

        LinesToAdd.Add(FTextLayout::FNewLineData(MoveTemp(LineText), MoveTemp(Runs)));
    }

    TargetTextLayout.AddLines(LinesToAdd);
}

void FCortexMentionMarshaller::GetText(FString& TargetString, const FTextLayout& SourceTextLayout)
{
    SourceTextLayout.GetAsText(TargetString);
}
