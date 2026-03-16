#include "Misc/AutomationTest.h"
#include "Rendering/CortexMarkdownParser.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserTaggedCodeBlockTest,
    "Cortex.Frontend.MarkdownParser.TaggedCodeBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserTaggedCodeBlockTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("Before\n\n```cpp:header\n#pragma once\nclass ATest {};\n```\n\nAfter");
    const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

    TestEqual(TEXT("Should produce 3 blocks"), Blocks.Num(), 3);
    if (Blocks.Num() >= 3)
    {
        TestEqual(TEXT("Block 1 should be CodeBlock"),
            static_cast<uint8>(Blocks[1].Type), static_cast<uint8>(ECortexMarkdownBlockType::CodeBlock));
        TestEqual(TEXT("Language should be cpp"), Blocks[1].Language, FString(TEXT("cpp")));
        TestEqual(TEXT("CodeBlockTarget should be header"), Blocks[1].CodeBlockTarget, FString(TEXT("header")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserImplementationTagTest,
    "Cortex.Frontend.MarkdownParser.ImplementationTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserImplementationTagTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("```cpp:implementation\n#include \"Test.h\"\nvoid ATest::BeginPlay() {}\n```");
    const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

    TestEqual(TEXT("Should produce 1 block"), Blocks.Num(), 1);
    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Language should be cpp"), Blocks[0].Language, FString(TEXT("cpp")));
        TestEqual(TEXT("CodeBlockTarget should be implementation"),
            Blocks[0].CodeBlockTarget, FString(TEXT("implementation")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserUntaggedBlockTest,
    "Cortex.Frontend.MarkdownParser.UntaggedCodeBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserUntaggedBlockTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("```cpp\nvoid Foo() {}\n```");
    const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

    TestEqual(TEXT("Should produce 1 block"), Blocks.Num(), 1);
    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Language should be cpp"), Blocks[0].Language, FString(TEXT("cpp")));
        TestTrue(TEXT("CodeBlockTarget should be empty for untagged"),
            Blocks[0].CodeBlockTarget.IsEmpty());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserSnippetTagTest,
    "Cortex.Frontend.MarkdownParser.SnippetTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserSnippetTagTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString Input = TEXT("```cpp:snippet\nFVector Loc = GetActorLocation();\n```");
    const TArray<FCortexMarkdownBlock> Blocks = CortexMarkdownParser::ParseBlocks(Input);

    TestEqual(TEXT("Should produce 1 block"), Blocks.Num(), 1);
    if (Blocks.Num() == 1)
    {
        TestEqual(TEXT("Language should be cpp"), Blocks[0].Language, FString(TEXT("cpp")));
        TestEqual(TEXT("CodeBlockTarget should be snippet"),
            Blocks[0].CodeBlockTarget, FString(TEXT("snippet")));
    }

    return true;
}
