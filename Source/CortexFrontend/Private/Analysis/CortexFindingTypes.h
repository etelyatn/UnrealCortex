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

    FString GetDeduplicationKey() const
    {
        return FString::Printf(TEXT("%s::%s"), *NodeGuid.ToString(), *Title);
    }
};
