// CortexChatEntryBuilderTest.cpp
#include "Misc/AutomationTest.h"
#include "Rendering/CortexChatEntryBuilder.h"
#include "Analysis/CortexFindingTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexChatEntryBuilderFindingTagTest,
    "Cortex.Frontend.ChatEntryBuilder.ParseFindingTag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatEntryBuilderFindingTagTest::RunTest(const FString& Parameters)
{
    const FString Input = TEXT("Some analysis text.\n\n```finding:bug:critical\n{\n  \"title\": \"Null check missing\",\n  \"node\": \"node_47\",\n  \"description\": \"The Cast node has no IsValid check\",\n  \"suggestedFix\": \"Add Branch after Cast\"\n}\n```\n\nMore text.");

    TArray<FCortexAnalysisFinding> Findings;
    TArray<TSharedPtr<FCortexChatEntry>> Entries = FCortexChatEntryBuilder::BuildEntries(Input, &Findings);

    TestTrue(TEXT("Has multiple entries"), Entries.Num() >= 2);
    TestEqual(TEXT("Extracted 1 finding"), Findings.Num(), 1);

    if (Findings.Num() > 0)
    {
        TestEqual(TEXT("Finding title"), Findings[0].Title, TEXT("Null check missing"));
        TestEqual(TEXT("Finding category"), (int32)Findings[0].Category, (int32)ECortexFindingCategory::Bug);
        TestEqual(TEXT("Finding severity"), (int32)Findings[0].Severity, (int32)ECortexFindingSeverity::Critical);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexChatEntryBuilderRegularCodeBlockTest,
    "Cortex.Frontend.ChatEntryBuilder.RegularCodeBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatEntryBuilderRegularCodeBlockTest::RunTest(const FString& Parameters)
{
    const FString Input = TEXT("Here is some code:\n\n```cpp\nvoid Foo() {}\n```\n");

    TArray<FCortexAnalysisFinding> Findings;
    TArray<TSharedPtr<FCortexChatEntry>> Entries = FCortexChatEntryBuilder::BuildEntries(Input, &Findings);

    TestEqual(TEXT("No findings from regular code"), Findings.Num(), 0);

    bool bFoundCodeBlock = false;
    for (const auto& Entry : Entries)
    {
        if (Entry->Type == ECortexChatEntryType::CodeBlock)
        {
            bFoundCodeBlock = true;
            break;
        }
    }
    TestTrue(TEXT("Has code block entry"), bFoundCodeBlock);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexChatEntryBuilderMalformedFindingTest,
    "Cortex.Frontend.ChatEntryBuilder.MalformedFinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexChatEntryBuilderMalformedFindingTest::RunTest(const FString& Parameters)
{
    const FString Input = TEXT("```finding:bug:warning\n{broken json}\n```\n");

    TArray<FCortexAnalysisFinding> Findings;
    TArray<TSharedPtr<FCortexChatEntry>> Entries = FCortexChatEntryBuilder::BuildEntries(Input, &Findings);

    TestEqual(TEXT("Malformed finding discarded"), Findings.Num(), 0);
    TestTrue(TEXT("Has entries"), Entries.Num() > 0);

    return true;
}
