// CortexAnalysisPromptAssembler.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexFindingTypes.h"

struct FCortexAnalysisContext;

class FCortexAnalysisPromptAssembler
{
public:
    /** Assemble the system prompt from context (focus layers only — no BP data). */
    static FString Assemble(const FCortexAnalysisContext& Context);

    /** Build the initial user message (BP data + engine diagnostics + request). */
    static FString BuildInitialUserMessage(
        const FCortexAnalysisContext& Context,
        const FString& BlueprintJson);
};
