#pragma once

#include "CoreMinimal.h"

struct FCortexFrontendSearchReplacePair
{
    FString SearchText;
    FString ReplaceText;
};

namespace CortexDiffParser
{
    /**
     * Parse SEARCH/REPLACE markers from a code block's raw text.
     * Returns true if valid diff markers were found (all pairs well-formed).
     * Returns false if no markers found or markers are malformed — caller should
     * treat the block as a regular full-file code block.
     */
    bool Parse(const FString& CodeBlockText, TArray<FCortexFrontendSearchReplacePair>& OutPairs);

    /**
     * Normalize text for reliable diff matching: strip all \r characters and
     * trailing whitespace per line. AI-generated search text often differs from
     * stored document content in invisible trailing spaces or stray \r.
     */
    inline FString NormalizeForDiff(const FString& Text)
    {
        FString Result = Text;
        Result.ReplaceInline(TEXT("\r"), TEXT(""));

        TArray<FString> Lines;
        Result.ParseIntoArray(Lines, TEXT("\n"), false);
        for (FString& Line : Lines)
        {
            Line = Line.TrimEnd();
        }
        return FString::Join(Lines, TEXT("\n"));
    }
}
