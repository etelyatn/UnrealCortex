#pragma once

#include "CoreMinimal.h"
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
};

struct FCortexChatDisplayRow
{
	ECortexChatRowType RowType = ECortexChatRowType::UserMessage;
	TSharedPtr<FCortexChatEntry> PrimaryEntry;           // Text or code block content
	TArray<TSharedPtr<FCortexChatEntry>> ToolCalls;      // Empty for non-assistant turns
};
