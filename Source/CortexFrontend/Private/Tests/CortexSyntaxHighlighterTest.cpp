#include "Misc/AutomationTest.h"
#include "Rendering/CortexSyntaxHighlighter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSyntaxHighlighterKeywordTest,
    "Cortex.Frontend.SyntaxHighlighter.Keywords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSyntaxHighlighterKeywordTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FCortexSyntaxRun> Runs = CortexSyntaxHighlighter::Tokenize(TEXT("void Foo()"));
    TestTrue(TEXT("Should have runs"), Runs.Num() > 0);

    // First run should be "void" keyword
    TestEqual(TEXT("First run text"), Runs[0].Text, FString(TEXT("void")));
    TestEqual(TEXT("First run type"), static_cast<uint8>(Runs[0].Type), static_cast<uint8>(ECortexSyntaxTokenType::Keyword));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSyntaxHighlighterStringTest,
    "Cortex.Frontend.SyntaxHighlighter.Strings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSyntaxHighlighterStringTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FCortexSyntaxRun> Runs = CortexSyntaxHighlighter::Tokenize(TEXT("FString Msg = TEXT(\"hello\");"));

    // Find a string run
    const FCortexSyntaxRun* StringRun = Runs.FindByPredicate([](const FCortexSyntaxRun& R) { return R.Type == ECortexSyntaxTokenType::String; });
    TestNotNull(TEXT("Should find a string token"), StringRun);
    if (StringRun)
    {
        TestTrue(TEXT("String should contain hello"), StringRun->Text.Contains(TEXT("hello")));
    }

    // FString should be a UE type
    const FCortexSyntaxRun* TypeRun = Runs.FindByPredicate([](const FCortexSyntaxRun& R) { return R.Type == ECortexSyntaxTokenType::UEType; });
    TestNotNull(TEXT("Should find a UE type token"), TypeRun);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSyntaxHighlighterCommentTest,
    "Cortex.Frontend.SyntaxHighlighter.Comments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSyntaxHighlighterCommentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const TArray<FCortexSyntaxRun> Runs = CortexSyntaxHighlighter::Tokenize(TEXT("int x = 5; // counter"));

    const FCortexSyntaxRun* CommentRun = Runs.FindByPredicate([](const FCortexSyntaxRun& R) { return R.Type == ECortexSyntaxTokenType::Comment; });
    TestNotNull(TEXT("Should find a comment token"), CommentRun);
    if (CommentRun)
    {
        TestTrue(TEXT("Comment should contain counter"), CommentRun->Text.Contains(TEXT("counter")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSyntaxHighlighterMultilineTest,
    "Cortex.Frontend.SyntaxHighlighter.Multiline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSyntaxHighlighterMultilineTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Code = TEXT("void Foo()\n{\n    int x = 42;\n}");
    const TArray<TArray<FCortexSyntaxRun>> Lines = CortexSyntaxHighlighter::TokenizeBlock(Code);
    TestEqual(TEXT("Should have 4 lines"), Lines.Num(), 4);

    // Line 0: "void Foo()" — should have keyword "void"
    if (Lines.Num() > 0)
    {
        const FCortexSyntaxRun* KeywordRun = Lines[0].FindByPredicate([](const FCortexSyntaxRun& R) { return R.Type == ECortexSyntaxTokenType::Keyword; });
        TestNotNull(TEXT("Line 0 should have keyword"), KeywordRun);
    }

    // Line 2: "    int x = 42;" — should have number "42"
    if (Lines.Num() > 2)
    {
        const FCortexSyntaxRun* NumRun = Lines[2].FindByPredicate([](const FCortexSyntaxRun& R) { return R.Type == ECortexSyntaxTokenType::Number; });
        TestNotNull(TEXT("Line 2 should have number"), NumRun);
    }

    return true;
}
