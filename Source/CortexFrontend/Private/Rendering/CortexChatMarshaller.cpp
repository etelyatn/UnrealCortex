#include "Rendering/CortexChatMarshaller.h"

#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/TextLayout.h"
#include "Rendering/CortexRichTextStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"

namespace
{
	struct FChatTextRun
	{
		FString Text;
		FName StyleName; // Empty = default style
	};

	/**
	 * Split a plain-text segment on @word patterns, emitting Mention-styled runs for each token.
	 * Must be defined before ParseLineIntoRuns which calls it.
	 */
	void SplitForMentions(const FString& Text, TArray<FChatTextRun>& Out)
	{
		int32 Pos = 0;
		while (Pos < Text.Len())
		{
			const int32 AtPos = Text.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (AtPos == INDEX_NONE)
			{
				if (Pos < Text.Len()) Out.Add({Text.Mid(Pos), FName()});
				break;
			}
			if (AtPos > Pos)
			{
				Out.Add({Text.Mid(Pos, AtPos - Pos), FName()});
			}
			int32 End = AtPos + 1;
			while (End < Text.Len() && !FChar::IsWhitespace(Text[End]))
			{
				++End;
			}
			if (End > AtPos + 1)
			{
				Out.Add({Text.Mid(AtPos, End - AtPos), FName(TEXT("Mention"))});
			}
			else
			{
				Out.Add({TEXT("@"), FName()}); // Lone '@' - plain text
			}
			Pos = End;
		}
	}

	/**
	 * Parse a single line into runs by splitting on <Tag>content</> patterns.
	 * Supports: <Bold>, <Italic>, <Code>. Unknown tags are rendered as-is.
	 * Nested tags are not supported - ToRichText() never generates them.
	 * Plain-text segments are further split on @word patterns for mention highlighting.
	 */
	TArray<FChatTextRun> ParseLineIntoRuns(const FString& Line)
	{
		TArray<FChatTextRun> Runs;
		int32 Pos = 0;
		const int32 Len = Line.Len();

		while (Pos < Len)
		{
			// Look for '<' which might start a tag
			const int32 TagOpen = Line.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
			if (TagOpen == INDEX_NONE)
			{
				// No more tags - rest is plain text (split on @mentions)
				if (Pos < Len)
				{
					SplitForMentions(Line.Mid(Pos), Runs);
				}
				break;
			}

			// Plain text before this tag (split on @mentions)
			if (TagOpen > Pos)
			{
				SplitForMentions(Line.Mid(Pos, TagOpen - Pos), Runs);
			}

			// Try to parse <TagName>content</>
			const int32 TagClose = Line.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagOpen + 1);
			if (TagClose == INDEX_NONE)
			{
				// Malformed - treat rest as plain text
				Runs.Add({Line.Mid(TagOpen), FName()});
				break;
			}

			const FString TagName = Line.Mid(TagOpen + 1, TagClose - TagOpen - 1);

			// Only handle our known style tags
			if (TagName == TEXT("Bold") || TagName == TEXT("Italic") || TagName == TEXT("Code"))
			{
				// Find closing </>
				const int32 CloseTag = Line.Find(TEXT("</>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagClose + 1);
				if (CloseTag != INDEX_NONE)
				{
					const FString Content = Line.Mid(TagClose + 1, CloseTag - TagClose - 1);
					Runs.Add({Content, FName(*TagName)});
					Pos = CloseTag + 3; // Skip past </>
					continue;
				}
			}

			// Unknown tag or no closing </> - emit '<' as plain text and continue
			Runs.Add({TEXT("<"), FName()});
			Pos = TagOpen + 1;
		}

		return Runs;
	}

	/** Unescape HTML entities that ToRichText() escaped for SRichTextBlock safety. */
	FString UnescapeHtmlEntities(const FString& Input)
	{
		FString Result = Input;
		Result.ReplaceInline(TEXT("&lt;"), TEXT("<"));
		Result.ReplaceInline(TEXT("&gt;"), TEXT(">"));
		Result.ReplaceInline(TEXT("&amp;"), TEXT("&"));
		Result.ReplaceInline(TEXT("&quot;"), TEXT("\""));
		return Result;
	}
}

TSharedRef<FCortexChatMarshaller> FCortexChatMarshaller::Create()
{
	return MakeShareable(new FCortexChatMarshaller());
}

void FCortexChatMarshaller::SetText(const FString& SourceString, FTextLayout& TargetTextLayout)
{
	const FTextBlockStyle DefaultStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));
	const ISlateStyle& RichStyles = FCortexRichTextStyle::Get();

	TArray<FTextRange> LineRanges;
	FTextRange::CalculateLineRangesFromString(SourceString, LineRanges);

	TArray<FTextLayout::FNewLineData> LinesToAdd;
	LinesToAdd.Reserve(LineRanges.Num());

	for (const FTextRange& LineRange : LineRanges)
	{
		const FString LineText = SourceString.Mid(LineRange.BeginIndex, LineRange.Len());
		const TArray<FChatTextRun> ParsedRuns = ParseLineIntoRuns(LineText);

		// Build the combined line string and styled runs
		TSharedRef<FString> ModelString = MakeShareable(new FString());
		TArray<TSharedRef<IRun>> SlateRuns;

		for (const FChatTextRun& ParsedRun : ParsedRuns)
		{
			const FString Unescaped = UnescapeHtmlEntities(ParsedRun.Text);
			const int32 RunStart = ModelString->Len();
			*ModelString += Unescaped;
			const int32 RunEnd = ModelString->Len();

			const FTextBlockStyle* RunStyle = &DefaultStyle;
			if (!ParsedRun.StyleName.IsNone() &&
				RichStyles.HasWidgetStyle<FTextBlockStyle>(ParsedRun.StyleName))
			{
				RunStyle = &RichStyles.GetWidgetStyle<FTextBlockStyle>(ParsedRun.StyleName);
			}

			SlateRuns.Add(FSlateTextRun::Create(
				FRunInfo(), ModelString, *RunStyle, FTextRange(RunStart, RunEnd)));
		}

		// Empty line - add a default run so the layout still inserts a line break
		if (SlateRuns.IsEmpty())
		{
			SlateRuns.Add(FSlateTextRun::Create(FRunInfo(), ModelString, DefaultStyle));
		}

		LinesToAdd.Emplace(MoveTemp(ModelString), MoveTemp(SlateRuns));
	}

	TargetTextLayout.AddLines(LinesToAdd);
}

void FCortexChatMarshaller::GetText(FString& TargetString, const FTextLayout& SourceTextLayout)
{
	SourceTextLayout.GetAsText(TargetString);
}
