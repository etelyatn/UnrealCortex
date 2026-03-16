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
}
