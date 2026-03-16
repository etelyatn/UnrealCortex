#include "Conversion/CortexDiffParser.h"

namespace
{
    enum class EDiffParseState : uint8
    {
        Idle,
        InSearch,
        InReplace
    };
}

bool CortexDiffParser::Parse(const FString& CodeBlockText, TArray<FCortexFrontendSearchReplacePair>& OutPairs)
{
    OutPairs.Empty();

    // Quick check: if no SEARCH marker exists, skip parsing entirely
    if (!CodeBlockText.Contains(TEXT("<<<<<<< SEARCH")))
    {
        return false;
    }

    // Canonicalize line endings
    FString Normalized = CodeBlockText;
    Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

    TArray<FString> Lines;
    Normalized.ParseIntoArray(Lines, TEXT("\n"), false);

    EDiffParseState State = EDiffParseState::Idle;
    TArray<FString> SearchLines;
    TArray<FString> ReplaceLines;

    for (const FString& Line : Lines)
    {
        const FString Trimmed = Line.TrimStartAndEnd();

        switch (State)
        {
        case EDiffParseState::Idle:
        {
            if (Trimmed == TEXT("<<<<<<< SEARCH"))
            {
                State = EDiffParseState::InSearch;
                SearchLines.Empty();
            }
            // Lines outside markers are ignored (whitespace between pairs)
            break;
        }
        case EDiffParseState::InSearch:
        {
            if (Trimmed == TEXT("======="))
            {
                State = EDiffParseState::InReplace;
                ReplaceLines.Empty();
            }
            else if (Trimmed == TEXT(">>>>>>> REPLACE"))
            {
                // Malformed: got REPLACE end without separator
                OutPairs.Empty();
                return false;
            }
            else
            {
                SearchLines.Add(Line);
            }
            break;
        }
        case EDiffParseState::InReplace:
        {
            if (Trimmed == TEXT(">>>>>>> REPLACE"))
            {
                FCortexFrontendSearchReplacePair Pair;
                Pair.SearchText = FString::Join(SearchLines, TEXT("\n"));
                if (!SearchLines.IsEmpty())
                {
                    Pair.SearchText += TEXT("\n");
                }
                Pair.ReplaceText = FString::Join(ReplaceLines, TEXT("\n"));
                if (!ReplaceLines.IsEmpty())
                {
                    Pair.ReplaceText += TEXT("\n");
                }
                OutPairs.Add(MoveTemp(Pair));
                State = EDiffParseState::Idle;
            }
            else if (Trimmed == TEXT("<<<<<<< SEARCH"))
            {
                // Malformed: nested SEARCH
                OutPairs.Empty();
                return false;
            }
            else
            {
                ReplaceLines.Add(Line);
            }
            break;
        }
        }
    }

    // If we ended mid-parse, the block is malformed
    if (State != EDiffParseState::Idle)
    {
        OutPairs.Empty();
        return false;
    }

    return OutPairs.Num() > 0;
}
