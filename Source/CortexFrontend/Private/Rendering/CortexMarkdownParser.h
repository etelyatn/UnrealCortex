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

enum class ECortexMarkdownInlineType : uint8
{
	Text,
	Bold,
	Italic,
	InlineCode,
	Link
};

struct FCortexMarkdownInline
{
	ECortexMarkdownInlineType Type = ECortexMarkdownInlineType::Text;
	FString Text;
	FString Url;  // For links only
};

struct FCortexMarkdownBlock
{
	ECortexMarkdownBlockType Type = ECortexMarkdownBlockType::Paragraph;
	FString RawText;
	FString Language;  // For CodeBlock only
	int32 HeaderLevel = 0;  // For Header only (1-3)
	TArray<FString> ListItems;  // For list types

	/** Parse inline formatting within this block's text. */
	TArray<FCortexMarkdownInline> GetInlines() const;
};

namespace CortexMarkdownParser
{
	/** Split raw markdown text into blocks (paragraphs, headers, code blocks, lists). */
	TArray<FCortexMarkdownBlock> ParseBlocks(const FString& Markdown);

	/** Convert inline markdown (bold, italic, code, links) to SRichTextBlock markup. */
	FString ToRichText(const FString& InlineMarkdown);
}
