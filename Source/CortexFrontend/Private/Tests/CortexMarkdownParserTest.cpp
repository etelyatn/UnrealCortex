#include "Misc/AutomationTest.h"
#include "Rendering/CortexMarkdownParser.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserBlockSplitTest,
	"Cortex.Frontend.MarkdownParser.BlockSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserBlockSplitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Input = TEXT("Hello\n\n## Header\n\nWorld");
	const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

	TestEqual(TEXT("Should produce 3 blocks"), Blocks.Num(), 3);
	if (Blocks.Num() >= 3)
	{
		TestEqual(TEXT("Block 0 type"), static_cast<uint8>(Blocks[0].Type), static_cast<uint8>(ECortexMarkdownBlockType::Paragraph));
		TestEqual(TEXT("Block 1 type"), static_cast<uint8>(Blocks[1].Type), static_cast<uint8>(ECortexMarkdownBlockType::Header));
		TestEqual(TEXT("Block 1 level"), Blocks[1].HeaderLevel, 2);
		TestEqual(TEXT("Block 2 type"), static_cast<uint8>(Blocks[2].Type), static_cast<uint8>(ECortexMarkdownBlockType::Paragraph));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserInlineTest,
	"Cortex.Frontend.MarkdownParser.InlineFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserInlineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Result = CortexMarkdownParser::ToRichText(TEXT("This is **bold** and `code`"));
	TestTrue(TEXT("Should contain Bold tag"), Result.Contains(TEXT("<Bold>")));
	TestTrue(TEXT("Should contain Code tag"), Result.Contains(TEXT("<Code>")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserCodeBlockTest,
	"Cortex.Frontend.MarkdownParser.CodeBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserCodeBlockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Input = TEXT("Before\n\n```cpp\nvoid Foo() {}\n```\n\nAfter");
	const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

	TestEqual(TEXT("Should produce 3 blocks"), Blocks.Num(), 3);
	if (Blocks.Num() >= 3)
	{
		TestEqual(TEXT("Block 1 should be CodeBlock"), static_cast<uint8>(Blocks[1].Type), static_cast<uint8>(ECortexMarkdownBlockType::CodeBlock));
		TestEqual(TEXT("Language should be cpp"), Blocks[1].Language, FString(TEXT("cpp")));
		TestTrue(TEXT("Code should contain Foo"), Blocks[1].RawText.Contains(TEXT("Foo")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserListTest,
	"Cortex.Frontend.MarkdownParser.List",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserListTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Input = TEXT("- Item one\n- Item two\n- Item three");
	const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

	TestEqual(TEXT("Should produce 1 block"), Blocks.Num(), 1);
	if (Blocks.Num() == 1)
	{
		TestEqual(TEXT("Should be unordered list"), static_cast<uint8>(Blocks[0].Type), static_cast<uint8>(ECortexMarkdownBlockType::UnorderedList));
		TestEqual(TEXT("Should have 3 items"), Blocks[0].ListItems.Num(), 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserEscapingTest,
	"Cortex.Frontend.MarkdownParser.AngleBracketEscaping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserEscapingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Inline code with angle brackets: `TArray<int32>` must escape the inner <>
	const FString Result = CortexMarkdownParser::ToRichText(TEXT("Use `TArray<int32>` for arrays"));

	// The angle brackets inside <Code>...</> must be escaped so SRichTextBlock
	// doesn't try to parse <int32> as a decorator tag
	TestTrue(TEXT("Should escape < inside code span"),
		Result.Contains(TEXT("TArray&lt;int32&gt;")));
	TestTrue(TEXT("Should still have Code tag"), Result.Contains(TEXT("<Code>")));
	TestFalse(TEXT("Should not have literal <int32>"), Result.Contains(TEXT("<int32>")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserUnderscoreIdentifierTest,
	"Cortex.Frontend.MarkdownParser.UnderscoreIdentifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserUnderscoreIdentifierTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Identifiers with underscores must NOT be treated as italic
	const FString Result = CortexMarkdownParser::ToRichText(TEXT("Use BP_SimpleActor_Component in the level"));

	TestFalse(TEXT("Should not contain <Italic> tag"),
		Result.Contains(TEXT("<Italic>")));
	TestTrue(TEXT("Should preserve BP_SimpleActor_Component intact"),
		Result.Contains(TEXT("BP_SimpleActor_Component")));
	return true;
}
