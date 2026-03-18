// CortexFindingTypes.h
#pragma once

#include "CoreMinimal.h"

enum class ECortexFindingSeverity : uint8
{
    Critical,
    Warning,
    Info,
    Suggestion
};

enum class ECortexFindingCategory : uint8
{
    Bug,
    Performance,
    Quality,
    CppCandidate,
    EngineFixGuidance  // Auto-checked when pre-scan finds issues
};

enum class ECortexAnalysisDepth : uint8
{
	Light,
	Standard,
	Deep
};

struct FCortexAnalysisFinding
{
    int32 FindingIndex = -1;
    ECortexFindingSeverity Severity = ECortexFindingSeverity::Info;
    ECortexFindingCategory Category = ECortexFindingCategory::Quality;
    FString Title;
    FString Description;
    FString SuggestedFix;            // optional remediation guidance
    FString GraphName;
    FString NodeDisplayName;          // human-readable, from mapping table
    FGuid NodeGuid;
    TArray<FGuid> RelatedNodeGuids;
    float Confidence = 1.0f;

    FString GetDeduplicationKey() const
    {
        // Include Category so same-node same-title findings in different categories
        // (e.g., both a bug and a performance issue on the same node) are not collapsed.
        return FString::Printf(TEXT("%s::%s::%d"),
            *NodeGuid.ToString(), *Title, (int32)Category);
    }
};
