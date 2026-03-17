// CortexAnalysisPromptAssembler.cpp
#include "Analysis/CortexAnalysisPromptAssembler.h"

#include "Analysis/CortexAnalysisContext.h"
#include "CortexAnalysisTypes.h"

namespace
{
    const TCHAR* SystemPromptBase = TEXT(R"(You are a Blueprint analysis assistant for Unreal Engine 5.6.

Your task is to analyze Blueprint logic and report findings in a structured format.

## Output Format

For each finding, emit a fenced code block tagged with `finding:category:severity`:

```finding:category:severity
{
  "title": "Short title",
  "node": "node_N",
  "description": "Detailed explanation",
  "suggestedFix": "How to fix this"
}
```

Categories: bug, performance, quality, cpp_candidate
Severities: critical, warning, info, suggestion

## Node References

Use node IDs from the serialized Blueprint data (e.g., "node_47"). Each node has an "id" field in the JSON.

## Rules

- Report each issue as a separate finding block
- Reference specific nodes by their ID
- Be precise about what's wrong and how to fix it
- Don't re-report engine-detected issues (listed in ENGINE DIAGNOSTICS) — instead add fix guidance
- Regular analysis text outside of finding blocks is fine for context and explanation)");

    const TCHAR* BugFocusLayer = TEXT(R"(
## Bug Analysis Focus
Look for: null reference risks (missing IsValid after Cast), unhandled edge cases, dead code paths, type mismatches, incorrect operator usage, infinite loops, race conditions with timers/delegates, wrong execution flow (missing break in switch-like patterns).)");

    const TCHAR* PerfFocusLayer = TEXT(R"(
## Performance Analysis Focus
Look for: unnecessary Tick usage (bCanEverTick with no Tick logic, or Tick doing work that should be event-driven), GetAllActorsOfClass in loops or Tick, SpawnActor in hot paths, heavy string operations in Tick, unnecessary array copies, FindComponentByClass called repeatedly instead of caching.)");

    const TCHAR* QualityFocusLayer = TEXT(R"(
## Blueprint Quality Focus
Look for: poor naming (generic variable names like "NewVar", "Bool1"), overly complex graphs (>15 nodes in a single execution chain without subfunctions), missing comments on complex logic, duplicated logic that should be a function, deeply nested branches, magic numbers without explanation.)");

    const TCHAR* CppFocusLayer = TEXT(R"(
## C++ Migration Readiness Focus
Look for: heavy math operations (vector math, matrix operations), large data iterations (ForEachLoop with >100 elements), frequently-called functions (called from Tick or per-frame delegates), complex state machines, string-heavy operations, performance-critical game logic that would benefit from native execution.)");

    const TCHAR* EngineFixGuidanceLayer = TEXT(R"(
## Engine Error Fix Guidance Focus
The ENGINE DIAGNOSTICS section lists issues already detected by the engine. For each one, provide specific fix guidance: what went wrong, step-by-step fix instructions, and potential root causes. Do NOT re-report these as new findings — reference them and add remediation context.)");

    FString PreScanFindingsToString(const TArray<FCortexPreScanFinding>& Findings, int32 MaxCount = 50)
    {
        if (Findings.Num() == 0)
        {
            return TEXT("No engine-detected issues.");
        }

        FString Result;
        const int32 Count = FMath::Min(Findings.Num(), MaxCount);
        for (int32 i = 0; i < Count; ++i)
        {
            const FCortexPreScanFinding& F = Findings[i];
            FString TypeStr;
            switch (F.Type)
            {
            case ECortexPreScanType::CompilationError:     TypeStr = TEXT("ERROR");          break;
            case ECortexPreScanType::CompilationWarning:   TypeStr = TEXT("WARNING");        break;
            case ECortexPreScanType::OrphanPin:            TypeStr = TEXT("ORPHAN_PIN");     break;
            case ECortexPreScanType::DeprecatedNode:       TypeStr = TEXT("DEPRECATED");     break;
            case ECortexPreScanType::UnhandledCastFailure: TypeStr = TEXT("UNHANDLED_CAST"); break;
            }
            Result += FString::Printf(TEXT("- [%s] %s (graph: %s)\n"), *TypeStr, *F.Description, *F.GraphName);
        }

        if (Findings.Num() > MaxCount)
        {
            Result += FString::Printf(TEXT("\n... and %d more issues\n"), Findings.Num() - MaxCount);
        }

        return Result;
    }
}

FString FCortexAnalysisPromptAssembler::Assemble(
    const FCortexAnalysisContext& Context)
{
    FString Prompt = SystemPromptBase;

    for (ECortexFindingCategory Cat : Context.SelectedFocusAreas)
    {
        switch (Cat)
        {
        case ECortexFindingCategory::Bug:              Prompt += BugFocusLayer;  break;
        case ECortexFindingCategory::Performance:      Prompt += PerfFocusLayer; break;
        case ECortexFindingCategory::Quality:          Prompt += QualityFocusLayer; break;
        case ECortexFindingCategory::CppCandidate:      Prompt += CppFocusLayer;          break;
        case ECortexFindingCategory::EngineFixGuidance: Prompt += EngineFixGuidanceLayer; break;
        }
    }

    return Prompt;
}

FString FCortexAnalysisPromptAssembler::BuildInitialUserMessage(
    const FCortexAnalysisContext& Context,
    const FString& BlueprintJson)
{
    FString Message;

    Message += TEXT("[BLUEPRINT DATA]\n");
    Message += BlueprintJson;
    Message += TEXT("\n\n");

    Message += TEXT("[ENGINE DIAGNOSTICS — already detected, do not re-report]\n");
    Message += PreScanFindingsToString(Context.Payload.PreScanFindings);
    Message += TEXT("\n\n");

    Message += TEXT("[ANALYSIS REQUEST]\n");
    Message += TEXT("Analyze this Blueprint focusing on: ");

    TArray<FString> FocusNames;
    for (ECortexFindingCategory Cat : Context.SelectedFocusAreas)
    {
        switch (Cat)
        {
        case ECortexFindingCategory::Bug:              FocusNames.Add(TEXT("Bugs & Logic Errors")); break;
        case ECortexFindingCategory::Performance:      FocusNames.Add(TEXT("Performance")); break;
        case ECortexFindingCategory::Quality:          FocusNames.Add(TEXT("Blueprint Quality")); break;
        case ECortexFindingCategory::CppCandidate:      FocusNames.Add(TEXT("C++ Migration Readiness"));       break;
        case ECortexFindingCategory::EngineFixGuidance: FocusNames.Add(TEXT("Engine Error Fix Guidance")); break;
        }
    }
    Message += FString::Join(FocusNames, TEXT(", "));

    return Message;
}
