#pragma once

#include "CoreMinimal.h"

enum class ECortexMarkdownBlockType : uint8
{
	Paragraph,
	Header,
	CodeBlock,
	UnorderedList,
	OrderedList,
	Table
};

struct FCortexMarkdownBlock
{
	ECortexMarkdownBlockType Type = ECortexMarkdownBlockType::Paragraph;
	FString RawText;
	FString Language;  // For CodeBlock only
	FString CodeBlockTarget;  // For CodeBlock: "header", "implementation", "snippet", or empty
	int32 HeaderLevel = 0;  // For Header only (1-3)
	TArray<FString> ListItems;  // For list types
	TArray<FString> TableHeaders;           // For Table only
	TArray<TArray<FString>> TableRows;      // For Table only
};

namespace CortexMarkdownParser
{
	/** Split raw markdown text into blocks (paragraphs, headers, code blocks, lists). */
	TArray<FCortexMarkdownBlock> ParseBlocks(const FString& Markdown);

	/** Convert inline markdown (bold, italic, code, links) to SRichTextBlock markup. */
	FString ToRichText(const FString& InlineMarkdown);
}
