// CortexChatEntryBuilder.h
#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"
#include "Analysis/CortexFindingTypes.h"

struct FCortexAnalysisSummary
{
	int32 Reported = 0;
	int32 EstimatedSuppressed = 0;
	FString SuppressionNotes;
};

class FCortexChatEntryBuilder
{
public:
    /**
     * Parse AI response text into chat entries.
     * If OutFindings is non-null, finding:* tagged code blocks are parsed as findings.
     * If OutSummary is non-null, analysis:summary tagged code blocks are parsed as summary.
     * Regular code blocks are returned as ECortexChatEntryType::CodeBlock entries.
     */
    static TArray<TSharedPtr<FCortexChatEntry>> BuildEntries(
        const FString& FullText,
        TArray<FCortexAnalysisFinding>* OutFindings = nullptr,
        FCortexAnalysisSummary* OutSummary = nullptr);

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

    /**
     * Parse analysis:summary JSON from a code block body.
     * Returns true if successfully parsed.
     */
    static bool ParseAnalysisSummary(const FString& JsonBody, FCortexAnalysisSummary& OutSummary);

    /**
     * Strip finding:* and analysis:summary code blocks from text.
     * Returns the text with those blocks removed (for display in chat).
     */
    static FString StripFindingBlocks(const FString& FullText);
};
