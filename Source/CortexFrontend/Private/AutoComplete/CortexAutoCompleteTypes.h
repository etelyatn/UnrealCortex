#pragma once
#include "CoreMinimal.h"

/** Kind of autocomplete item — controls display and behavior on selection */
enum class ECortexAutoCompleteKind : uint8
{
    ContextProvider,  // @thisAsset, @selection, @problems
    Asset,            // Fuzzy-matched UE asset under /Game/
    CoreCommand,      // /help, /clear, /compact
    Skill,            // Discovered from cortex-toolkit/skills/
    Agent,            // Discovered from cortex-toolkit/agents/
};

/** One row in the autocomplete popup */
struct FCortexAutoCompleteItem
{
    FString Name;           // "thisAsset", "BP_JumpPad", "cortex-blueprint"
    FString Description;    // Right-aligned hint text in popup row
    FString FullPath;       // "/Game/LevelPrototyping/BP_JumpPad" (assets only)
    FString RouterCommand;  // "blueprint.get_blueprint" — stamped at list build time (assets only)
    FString AssetClass;     // "Blueprint", "DataTable" — from Asset Registry (assets only)
    ECortexAutoCompleteKind Kind = ECortexAutoCompleteKind::ContextProvider;
};

/** Kind of context chip — controls resolution strategy at send time */
enum class ECortexContextChipKind : uint8
{
    Provider,  // @thisAsset, @selection, @problems — resolved via editor APIs
    Asset,     // /Game/... path — resolved via FCortexCommandRouter
    RawText,   // Passthrough — prepended as @label text
};

/** One chip in the input area chip row */
struct FCortexContextChip
{
    ECortexContextChipKind Kind = ECortexContextChipKind::RawText;
    FString Label;          // Display text + resolution key
    FString AssetClass;     // "Blueprint", "DataTable" — copied from FCortexAutoCompleteItem
    FString RouterCommand;  // "blueprint.get_blueprint" — copied from FCortexAutoCompleteItem
};

namespace CortexAutoComplete
{
    /**
     * Score how well Query matches Candidate (case-insensitive subsequence match).
     * Returns 0 if no match. Higher = better match.
     * Consecutive matching character runs score higher than scattered matches.
     */
    inline int32 FilterAndScore(const FString& Query, const FString& Candidate)
    {
        if (Query.IsEmpty()) return 1; // Empty query matches everything with minimum score

        const FString Q = Query.ToLower();
        const FString C = Candidate.ToLower();

        int32 Score = 0;
        int32 QIdx = 0;
        int32 RunLength = 0;

        for (int32 CIdx = 0; CIdx < C.Len() && QIdx < Q.Len(); ++CIdx)
        {
            if (C[CIdx] == Q[QIdx])
            {
                ++RunLength;
                Score += RunLength * 2; // Consecutive run bonus
                ++QIdx;
            }
            else
            {
                RunLength = 0;
            }
        }

        // All query chars must be found
        if (QIdx < Q.Len()) return 0;

        return Score;
    }
}
