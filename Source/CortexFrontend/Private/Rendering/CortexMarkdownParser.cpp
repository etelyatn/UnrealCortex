#include "Rendering/CortexMarkdownParser.h"

namespace
{
	int32 CountLeadingHashes(const FString& Line)
	{
		int32 Count = 0;
		while (Count < Line.Len() && Line[Count] == TEXT('#'))
		{
			++Count;
		}
		return Count;
	}

	FCortexMarkdownBlock MakeBlock(const TArray<FString>& Lines)
	{
		FCortexMarkdownBlock Block;
		if (Lines.IsEmpty())
		{
			return Block;
		}

		const FString& FirstLine = Lines[0];

		// Header
		if (FirstLine.StartsWith(TEXT("#")))
		{
			Block.Type = ECortexMarkdownBlockType::Header;
			Block.HeaderLevel = FMath::Min(CountLeadingHashes(FirstLine), 6);
			int32 ContentStart = Block.HeaderLevel;
			if (ContentStart < FirstLine.Len() && FirstLine[ContentStart] == TEXT(' '))
			{
				++ContentStart;
			}
			Block.RawText = FirstLine.Mid(ContentStart);
			return Block;
		}

		// Unordered list
		if (FirstLine.StartsWith(TEXT("- ")) || FirstLine.StartsWith(TEXT("* ")))
		{
			Block.Type = ECortexMarkdownBlockType::UnorderedList;
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("- ")) || Line.StartsWith(TEXT("* ")))
				{
					Block.ListItems.Add(Line.Mid(2));
				}
				else if (!Line.IsEmpty())
				{
					Block.ListItems.Add(Line);
				}
			}
			Block.RawText = FString::Join(Lines, TEXT("\n"));
			return Block;
		}

		// Ordered list
		if (Lines.Num() > 0 && Lines[0].Len() >= 3 && FChar::IsDigit(Lines[0][0]) && Lines[0][1] == TEXT('.') && Lines[0][2] == TEXT(' '))
		{
			Block.Type = ECortexMarkdownBlockType::OrderedList;
			for (const FString& Line : Lines)
			{
				int32 DotPos = Line.Find(TEXT(". "));
				if (DotPos != INDEX_NONE)
				{
					Block.ListItems.Add(Line.Mid(DotPos + 2));
				}
				else if (!Line.IsEmpty())
				{
					Block.ListItems.Add(Line);
				}
			}
			Block.RawText = FString::Join(Lines, TEXT("\n"));
			return Block;
		}

		// Paragraph
		Block.Type = ECortexMarkdownBlockType::Paragraph;
		Block.RawText = FString::Join(Lines, TEXT("\n"));
		return Block;
	}
}

TArray<FCortexMarkdownBlock> CortexMarkdownParser::ParseBlocks(const FString& Markdown)
{
	TArray<FCortexMarkdownBlock> Blocks;
	TArray<FString> Lines;
	Markdown.ParseIntoArray(Lines, TEXT("\n"), false);

	TArray<FString> CurrentLines;
	bool bInCodeBlock = false;
	FString CodeLanguage;
	TArray<FString> CodeLines;

	auto FlushCurrentLines = [&]()
	{
		if (!CurrentLines.IsEmpty())
		{
			Blocks.Add(MakeBlock(CurrentLines));
			CurrentLines.Empty();
		}
	};

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i];

		if (bInCodeBlock)
		{
			if (Line.TrimStartAndEnd() == TEXT("```"))
			{
				FCortexMarkdownBlock Block;
				Block.Type = ECortexMarkdownBlockType::CodeBlock;
				Block.Language = CodeLanguage;
				Block.RawText = FString::Join(CodeLines, TEXT("\n"));
				Blocks.Add(Block);
				bInCodeBlock = false;
				CodeLines.Empty();
				CodeLanguage.Reset();
			}
			else
			{
				CodeLines.Add(Line);
			}
			continue;
		}

		// Check for code block open
		if (Line.StartsWith(TEXT("```")))
		{
			FlushCurrentLines();
			bInCodeBlock = true;
			CodeLanguage = Line.Mid(3).TrimStartAndEnd();
			continue;
		}

		// Blank line = block separator
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			FlushCurrentLines();
			continue;
		}

		CurrentLines.Add(Line);
	}

	// Flush any remaining content
	if (bInCodeBlock)
	{
		FCortexMarkdownBlock Block;
		Block.Type = ECortexMarkdownBlockType::CodeBlock;
		Block.Language = CodeLanguage;
		Block.RawText = FString::Join(CodeLines, TEXT("\n"));
		Blocks.Add(Block);
	}
	else
	{
		FlushCurrentLines();
	}

	return Blocks;
}

FString CortexMarkdownParser::ToRichText(const FString& InlineMarkdown)
{
	FString Result = InlineMarkdown;

	// Bold: **text** → <Bold>text</>
	{
		FString Out;
		int32 i = 0;
		while (i < Result.Len())
		{
			if (i + 1 < Result.Len() && Result[i] == TEXT('*') && Result[i + 1] == TEXT('*'))
			{
				int32 End = Result.Find(TEXT("**"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 2);
				if (End != INDEX_NONE)
				{
					Out += TEXT("<Bold>");
					Out += Result.Mid(i + 2, End - i - 2);
					Out += TEXT("</>");
					i = End + 2;
					continue;
				}
			}
			Out += Result[i];
			++i;
		}
		Result = Out;
	}

	// Italic: *text* or _text_ → <Italic>text</>
	{
		FString Out;
		int32 i = 0;
		while (i < Result.Len())
		{
			if (Result[i] == TEXT('*') || Result[i] == TEXT('_'))
			{
				const TCHAR Marker = Result[i];
				FString MarkerStr;
				MarkerStr.AppendChar(Marker);
				int32 End = Result.Find(MarkerStr, ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
				if (End != INDEX_NONE && End > i + 1)
				{
					Out += TEXT("<Italic>");
					Out += Result.Mid(i + 1, End - i - 1);
					Out += TEXT("</>");
					i = End + 1;
					continue;
				}
			}
			Out += Result[i];
			++i;
		}
		Result = Out;
	}

	// Inline code: `code` → <Code>code</>
	{
		FString Out;
		int32 i = 0;
		while (i < Result.Len())
		{
			if (Result[i] == TEXT('`'))
			{
				int32 End = Result.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
				if (End != INDEX_NONE)
				{
					Out += TEXT("<Code>");
					Out += Result.Mid(i + 1, End - i - 1);
					Out += TEXT("</>");
					i = End + 1;
					continue;
				}
			}
			Out += Result[i];
			++i;
		}
		Result = Out;
	}

	return Result;
}

