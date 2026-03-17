// CortexChatEntryBuilder.h
#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"
#include "Analysis/CortexFindingTypes.h"

class FCortexChatEntryBuilder
{
public:
    /**
     * Parse AI response text into chat entries.
     * If OutFindings is non-null, finding:* tagged code blocks are parsed as findings.
     * Regular code blocks are returned as ECortexChatEntryType::CodeBlock entries.
     */
    static TArray<TSharedPtr<FCortexChatEntry>> BuildEntries(
        const FString& FullText,
        TArray<FCortexAnalysisFinding>* OutFindings = nullptr);

    /**
     * Parse a finding:category:severity tag into category and severity enums.
     * Returns true if successfully parsed.
     */
    static bool ParseFindingTag(
        const FString& Tag,
        ECortexFindingCategory& OutCategory,
        ECortexFindingSeverity& OutSeverity);

    /**
     * Parse finding JSON from a code block body.
     * Returns true if valid finding with required fields (title, node).
     */
    static bool ParseFindingJson(
        const FString& JsonBody,
        ECortexFindingCategory Category,
        ECortexFindingSeverity Severity,
        FCortexAnalysisFinding& OutFinding);
};
