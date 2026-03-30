#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexDiffParser.h"
#include "Session/CortexSessionTypes.h"

/**
 * Presentation-layer row types for the chat list view.
 * Lives in the widget layer — session files must not include this.
 */
enum class ECortexChatRowType : uint8
{
	UserMessage,
	AssistantTurn,   // Tool calls (optional) + assistant text combined
	CodeBlock,
	ProcessingRow,   // Transient row shown while session is Spawning or Processing
	StatusRow,       // Step-by-step status message (e.g., "Serializing Blueprint...")
	TableBlock,
	AuthError        // Auth failure with login/retry buttons
};

struct FCortexChatDisplayRow
{
	ECortexChatRowType RowType = ECortexChatRowType::UserMessage;
	// PrimaryEntry is valid for all row types EXCEPT ProcessingRow, which has no text entry.
	// Code that iterates DisplayRows must check RowType before accessing PrimaryEntry.
	TSharedPtr<FCortexChatEntry> PrimaryEntry;
	TArray<TSharedPtr<FCortexChatEntry>> ToolCalls;  // Empty for non-assistant turns

	// Diff support (populated during row construction for diff-type code blocks)
	bool bIsDiffBlock = false;
	TArray<FCortexFrontendSearchReplacePair> SearchReplacePairs;
};
