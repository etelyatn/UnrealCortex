#pragma once

#include "CoreMinimal.h"

enum class ECortexMarkdownBlockType : uint8
{
	Paragraph,
	Header,
	CodeBlock,
	UnorderedList,
	OrderedList
};

struct FCortexMarkdownBlock
{
	ECortexMarkdownBlockType Type = ECortexMarkdownBlockType::Paragraph;
	FString RawText;
	FString Language;  // For CodeBlock only
	int32 HeaderLevel = 0;  // For Header only (1-3)
	TArray<FString> ListItems;  // For list types
};

namespace CortexMarkdownParser
{
	/** Split raw markdown text into blocks (paragraphs, headers, code blocks, lists). */
	TArray<FCortexMarkdownBlock> ParseBlocks(const FString& Markdown);

	/** Convert inline markdown (bold, italic, code, links) to SRichTextBlock markup. */
	FString ToRichText(const FString& InlineMarkdown);
}
